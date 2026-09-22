#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <string.h>		// memcpy, memcmp, strcmp
#include "main.h"		// CMSIS (SCB_CleanInvalidateDCache_by_Addr, SCB_InvalidateDCache_by_Addr)
#include "npu_selftest.h"
#include "npu_rt.h"
#include "aed_test_input.h"	// 固定入力と期待値 (scripts/aed_ref.py の生成物。include はこのファイルだけ)
#include "ll_aton_NN_interface.h"	// EpochBlock_Flags_pure_sw

/*
 * ESC-10 の実録音 30 本の入力と PC の1位 (scripts/aed_clips.py の生成物)。ESC-50 由来のデータ
 * なのでコミットしない (.gitignore)。無ければ実録音の判定だけを飛ばしてビルドは通す
 */
#if __has_include("aed_test_clips.h")
#include "aed_test_clips.h"
#define NPU_HAVE_CLIPS		(1)
#else
#define NPU_HAVE_CLIPS		(0)
#endif
#include "../audio/audio_task.h"
#include "../trace/trace.h"	// NOW(), trace_cyc_to_us(), trace_busy()

/*
 * NPU タスクと、固定入力による推論の確認 (Phase 1 タスク6)
 *
 * ll_aton (npu_rt.c) を呼ぶのはこのタスクだけにする。
 *   - ll_aton はスレッドセーフでない (OSAL は BARE_METAL でロックが無い)
 *   - 推論はスタックを 1KB 以上使う。.su の値で、ll_sw_forward_softmax 360B、
 *     LL_ATON_End_EpochBlock_30 200B、ランタイムの各段 24〜40B に、ST のライブラリ
 *     (NetworkRuntime、.su 無し) の分が加わる。usermain は μT-Kernel の初期タスクで、
 *     スタックが 1KB (INITTASK_STKSZ、mtkernel/include/sys/inittask.h) しかなく、
 *     溢れても検出されない (USE_SPMON 無効)。npu_rt_init も同じ理由でこのタスクで呼ぶ
 *
 * 流れ:
 *   1. npu_rt_init()
 *   2. 自己テスト
 *      - 乱数入力で1回推論し、ONNX Runtime の期待値2通りと比べる (参考値。判定しない)
 *      - 同じ乱数入力で softmax 直前の int8 ロジットを比べる (参考値。フックの呼ばれ方も記録)
 *      - ESC-10 の実録音 30 本を推論し、1位を PC と比べる (これで PASS / FAIL)
 *   3. 乱数入力で10回連続。毎回の推論時間と、最初の推論の出力との差
 *   4. ここまで終わったら usermain に知らせる (usermain は音声を始める前にこれを待つ)
 *   5. NPU_PT_TEST が 1 なら、パススルーが立ち上がってから 960ms 間隔で10回推論し、
 *      音声の under / over / late が増えないかを見る (推論タスクの予行)
 *
 * 優先度 15 は音声 (task_pcm 5、task_audio 10) より低く、reporter (20) より高い。
 * POLLING の推論中はこのタスクが CPU を回し続けるので、推論のあいだ reporter と dump は動けない。
 */

/* 1: 手順5 (パススルー稼働中の推論) を行う。0 で無効 */
#define NPU_PT_TEST		(1)

#define NPU_TASK_PRI		(15)
#define NPU_TASK_STKSZ		(8 * 1024)

/*
 * 乱数入力の目安 (参考値。判定には使わない): 1位がこのクラスで、期待値2通りのどちらかとの差
 * (各クラスの差の最大値) が上限以内。外れたら入力バッファの先頭を出してキャッシュを確かめる
 */
#define ST_EXPECT_TOP		"helicopter"	/* 期待値2通りのどちらでも1位 (aed_test_input.h) */
#define ST_TOLERANCE		(0.05f)

#define ST_REPEAT		(10)		/* 手順3 の回数 */
#define PT_REPEAT		(10)		/* 手順5 の回数 */
#define PT_INTERVAL_MS		(960)		/* 手順5 の推論の開始間隔 */
#define PT_START_POLL		(300)		/* パススルーの立ち上がりを待つ上限 (x100ms) */
#define PT_REPORT_POLL		(1200)		/* 手順5 の結果を出す前にトレースの終わりを待つ上限 (x100ms) */
#define START_WAIT_MS		(30000)		/* usermain が手順1〜3 の終わりを待つ上限 */

#define HEAD_BYTES		(16)		/* 失敗時に表示する入力バッファの先頭 */
#define LINE_BYTES		(32)		/* D キャッシュのライン */

/*
 * softmax 直前の int8 ロジット (AED の network.c から読み取った番地。モデルを替えたら見直す)
 *   epoch 29 (NPU): 最後の Gemm の結果を int8 x10 で 0x34350000 に書く
 *                   (出力はストリームエンジン0。network.c:10939 で開始時にこのラインを invalidate)
 *   epoch 30 (SW) : DequantizeLinear。0x34350000 の int8 を読み、float x10 を 0x34350440 に書く。
 *                   scale (float) は 0x704a1590、zero_point (int8) は 0x704a1760 (network.c:11014-11034)
 *   epoch 31 (SW) : Softmax。0x34350440 を読んで 0x34350410 に出力し、0x34350000〜 を作業域に使う
 *                   (network.c:11079-11103) → 推論が終わると 0x34350000 の int8 は残っていない
 * そこで ll_aton の epoch コールバックで epoch 30 の終了処理 (= DequantizeLinear) の直前に取る。
 * epoch block の配列は epoch 1 から並び、最後に終端の空の block がある
 * (network.c の LL_ATON_EpochBlockItems_network)。epoch 30 は末尾から3番目。
 * Softmax の入力 (0x34350440 の float) は推論の後も残っているので、推論後に読んで int8 に戻し、
 * 取った int8 と一致するかも確かめる。
 */
#define LOGIT_Q_ADDR		(0x34350000U)
#define LOGIT_F_ADDR		(0x34350440U)		/* 64B 境界 */
#define LOGIT_SCALE_ADDR	(0x70180000U + 3282320U)
#define LOGIT_ZP_ADDR		(0x70180000U + 3282784U)
#define LOGIT_EB_FROM_END	(3)
#define LOGIT_TOL_LSB		(2)			/* 目安 (参考値): PC との差がこれ以内 */

_Static_assert(AED_TEST_INPUT_LEN == NPU_RT_IN_BYTES, "test input size");
_Static_assert(AED_TEST_CLASSES == NPU_RT_OUT_CLASSES, "test output size");

LOCAL void task_npu(INT stacd, void *exinf);
LOCAL ID	tskid_npu;
LOCAL T_CTSK	ctsk_npu = {
	.itskpri	= NPU_TASK_PRI,
	.stksz		= NPU_TASK_STKSZ,
	.task		= task_npu,
	.tskatr		= TA_HLNG | TA_RNG3,
};

/*
 * 手順1〜3 が終わったことを usermain に知らせる。tk_wup_tsk を使わないのは、usermain が
 * 待ちをタイムアウトした後に起床要求だけが残ると、usermain 最後の tk_slp_tsk(TMO_FEVR) が
 * すぐ戻って usermain が終わってしまうため
 */
LOCAL ID	semid_done;
LOCAL T_CSEM	csem_done = {
	.sematr		= TA_TFIFO | TA_FIRST,
	.isemcnt	= 0,
	.maxsem		= 1,
};

LOCAL float	ref_out[NPU_RT_OUT_CLASSES];	/* 最初の推論の出力。手順3・5 の比較の基準 */
LOCAL BOOL	ref_valid = FALSE;

LOCAL B		logit_q[NPU_RT_OUT_CLASSES];	/* epoch 30 の直前に取った int8 ロジット */
LOCAL BOOL	logit_got;
LOCAL UW	logit_flags;			/* 取ったときの epoch block の flags */

/* ---------------------------------------------------------------- */

/* tm_printf は浮動小数点を出せないので、1万倍して丸めた整数で出す */
LOCAL INT x10k(float v)
{
	return (INT)(v * 10000.0f + ((v >= 0.0f) ? 0.5f : -0.5f));
}

LOCAL INT argmax(const float *v)
{
	INT	i, top = 0;

	for(i = 1; i < NPU_RT_OUT_CLASSES; i++) {
		if(v[i] > v[top]) top = i;
	}
	return top;
}

LOCAL float max_abs_diff(const float *a, const float *b)
{
	float	d, m = 0.0f;
	INT	i;

	for(i = 0; i < NPU_RT_OUT_CLASSES; i++) {
		d = a[i] - b[i];
		if(d < 0.0f) d = -d;
		if(d > m) m = d;
	}
	return m;
}

LOCAL void show_head(const char *label, const B *p)
{
	INT	i;

	tm_printf((UB*)"    %s:", label);
	for(i = 0; i < HEAD_BYTES; i++) tm_printf((UB*)" %02x", (UW)(UB)p[i]);
	tm_printf((UB*)"\n");
}

/*
 * 固定入力を NPU の入力バッファへ写して1回推論し、出力を out に写す。
 * *us に npu_rt_run の所要時間 (DWT) を返す。
 * src は int8 の入力 NPU_RT_IN_BYTES バイト。
 * head_before が NULL でなければ、推論の直前に RAM にある入力の先頭を写す。
 * in_diff が NULL でなければ、推論の直前に RAM の入力全体を src と比べ、
 * 食い違うバイト数を返す。
 */
LOCAL ER run_once(const B *src, float *out, UW *us, B *head_before, UW *in_diff)
{
	B		*in = npu_rt_input();
	const float	*o  = npu_rt_output();
	UW		t0, n;
	INT		i;
	ER		er;

	memcpy(in, src, NPU_RT_IN_BYTES);

	/*
	 * CPU が書いた入力を RAM に出し (clean)、同じ範囲のラインをキャッシュから捨てる
	 * (invalidate)。入力の領域 (0x34350000〜) は推論中に NPU と SW epoch が中間結果と
	 * 出力の置き場に使うので、clean だけだとキャッシュに残ったラインが後で NPU の書いた
	 * 値を隠す。ST も同じ操作 (preproc_dpu.c:144)。先頭は 32B 境界 (npu_rt_init で確認済み)、
	 * 長さ 6144 は 32 の倍数
	 */
	SCB_CleanInvalidateDCache_by_Addr((volatile void *)in, NPU_RT_IN_BYTES);

	/*
	 * ラインを捨てた後なので、ここで読むと RAM の中身が見える。読んだラインはクリーンな
	 * ので推論の邪魔はしない (CPU が NPU の結果を読む箇所は、network.c が先に invalidate する)
	 */
	if(head_before != NULL) memcpy(head_before, in, HEAD_BYTES);
	if(in_diff != NULL) {
		for(i = 0, n = 0; i < NPU_RT_IN_BYTES; i++) {
			if(in[i] != src[i]) n++;
		}
		*in_diff = n;
	}

	t0  = NOW();
	er  = npu_rt_run();
	*us = trace_cyc_to_us((UW)(NOW() - t0));
	if(er != E_OK) return er;

	/*
	 * 出力 (softmax 後の float x10、0x34350410〜) は最後の SW epoch が CPU で書き、
	 * network.c がその後 clean している。CPU のキャッシュが正しい値を持っているので
	 * invalidate は要らない (0x34350410 は 32B 境界でもなく、invalidate すると隣を巻き込む)
	 */
	for(i = 0; i < NPU_RT_OUT_CLASSES; i++) out[i] = o[i];
	return E_OK;
}

/*
 * 入力バッファの先頭を、推論前 (RAM)・推論後 (キャッシュ越し)・推論後 (RAM)・期待値で並べる。
 * 推論後は領域が中間結果で上書きされているのが正常。キャッシュ越しと RAM が食い違えば、
 * CPU のキャッシュに古いラインが残っている。
 * 推論後の先頭ラインは network.c が最後に clean している (0x34350000〜+1088) ので、
 * ここで invalidate しても失うデータは無い
 */
LOCAL void show_input_heads(const B *src, const B *before)
{
	B	*in = npu_rt_input();
	B	after_cache[HEAD_BYTES], after_ram[HEAD_BYTES];

	memcpy(after_cache, in, HEAD_BYTES);
	SCB_InvalidateDCache_by_Addr((volatile void *)in, LINE_BYTES);
	memcpy(after_ram, in, HEAD_BYTES);

	tm_printf((UB*)"  input buffer head @ 0x%08x:\n", (UW)in);
	show_head("expected (source input)  ", src);
	show_head("before inference (RAM)   ", before);
	show_head("after inference (cache)  ", after_cache);
	show_head("after inference (RAM)    ", after_ram);
}

/* ---------------------------------------------------------------- */
/* softmax 直前の int8 ロジット                                        */
/* ---------------------------------------------------------------- */

/* npu_rt の epoch フック。推論の途中 (このタスクの中) で呼ばれるので表示はしない */
LOCAL void logit_hook(INT idx, INT n, UW flags)
{
	if(idx != n - LOGIT_EB_FROM_END) return;

	/*
	 * NPU (epoch 29) が RAM に書いた値を読むので、読む前に invalidate する。このラインは
	 * epoch 29 の開始時に network.c が invalidate しており、その後 CPU は書いていないので、
	 * もう一度 invalidate しても失うデータは無い
	 */
	SCB_InvalidateDCache_by_Addr((volatile void *)LOGIT_Q_ADDR, LINE_BYTES);
	memcpy(logit_q, (const void *)LOGIT_Q_ADDR, sizeof(logit_q));
	logit_flags = flags;
	logit_got   = TRUE;
}

/*
 * 1回推論して、softmax 直前の int8 ロジットを PC (ONNX Runtime) と比べる (乱数入力。参考値)。
 * 目安: 10個すべてが PC のどちらかの値と ±LOGIT_TOL_LSB 以内で、scale / zero_point が PC と同じ。
 * フックが呼ばれない件 (npu_rt.h の「未解決」) を確かめるため、epoch コールバックが推論の
 * 前後で設定されているかと、推論中の呼び出しを全部記録して表示する
 */
LOCAL BOOL logits_check(void)
{
	float	out[NPU_RT_OUT_CLASSES], lf[NPU_RT_OUT_CLASSES];
	float	b_scale, pc_scale = AED_TEST_LOGIT_SCALE, v;
	B	b_zp;
	UW	us;
	INT	i, d_opt, d_flt, max_opt = 0, max_flt = 0, max_pc = 0, back;
	BOOL	back_ok = TRUE, scale_ok, zp_ok, pass_opt, pass_flt, pass, cb_before, cb_after;
	ER	er;

	logit_got = FALSE;
	(void)npu_rt_set_epoch_hook(logit_hook);
	cb_before = npu_rt_epoch_cb_installed();
	npu_rt_cb_log(TRUE);
	er = run_once(aed_test_input, out, &us, NULL, NULL);
	npu_rt_cb_log(FALSE);
	cb_after = npu_rt_epoch_cb_installed();
	(void)npu_rt_set_epoch_hook(NULL);

	tm_printf((UB*)"npu logits (reference only; int8 softmax input captured just before epoch 30 @ 0x%08x):\n",
			LOGIT_Q_ADDR);
	tm_printf((UB*)"  [npu] epoch callback installed: before run=%d, after run=%d\n",
			cb_before ? 1 : 0, cb_after ? 1 : 0);
	npu_rt_cb_log_show();
	if(er != E_OK || !logit_got) {
		tm_printf((UB*)"  not captured (er=%d, captured=%d)\n", er, logit_got ? 1 : 0);
		return FALSE;
	}
	if((logit_flags & EpochBlock_Flags_pure_sw) == 0) {
		/* 末尾から3番目が SW の block でない = network.c が想定と違う */
		tm_printf((UB*)"  [WARN] captured block is not pure SW (flags=0x%x): check LOGIT_* against network.c\n",
				logit_flags);
	}

	/* ボードの scale / zero_point: DequantizeLinear が外部フラッシュ (重み) から読む値 */
	b_scale  = *(const volatile float *)LOGIT_SCALE_ADDR;
	b_zp     = *(const volatile B *)LOGIT_ZP_ADDR;
	scale_ok = (memcmp(&b_scale, &pc_scale, sizeof(float)) == 0);
	zp_ok    = ((INT)b_zp == AED_TEST_LOGIT_ZP);
	tm_printf((UB*)"  scale: board %u e-9 (flash 0x%08x), pc %u e-9 -> %s\n",
			(UW)(b_scale * 1e9f + 0.5f), LOGIT_SCALE_ADDR, (UW)(pc_scale * 1e9f + 0.5f),
			scale_ok ? "same" : "DIFFERENT");
	tm_printf((UB*)"  zero_point: board %d (flash 0x%08x), pc %d -> %s\n",
			(INT)b_zp, LOGIT_ZP_ADDR, AED_TEST_LOGIT_ZP, zp_ok ? "same" : "DIFFERENT");

	/*
	 * Softmax の入力 (epoch 30 の出力 float) を読み、int8 に戻して取った値と比べる。
	 * CPU (epoch 30) が書いて network.c が clean 済みのラインなので、invalidate しても
	 * 失うものは無い (0x34350440〜0x3435047F。epoch 31 はここに書かない)
	 */
	SCB_InvalidateDCache_by_Addr((volatile void *)LOGIT_F_ADDR, 2 * LINE_BYTES);
	memcpy(lf, (const void *)LOGIT_F_ADDR, sizeof(lf));

	tm_printf((UB*)"   # class            npu  ort noopt  d_ort d_noopt  logit(x1e-4) back\n");
	for(i = 0; i < NPU_RT_OUT_CLASSES; i++) {
		d_opt = (INT)logit_q[i] - (INT)aed_test_logits_ort[i];
		d_flt = (INT)logit_q[i] - (INT)aed_test_logits_ort_noopt[i];
		if(d_opt < 0) d_opt = -d_opt;
		if(d_flt < 0) d_flt = -d_flt;
		if(d_opt > max_opt) max_opt = d_opt;
		if(d_flt > max_flt) max_flt = d_flt;
		back = (INT)aed_test_logits_ort[i] - (INT)aed_test_logits_ort_noopt[i];
		if(back < 0) back = -back;
		if(back > max_pc) max_pc = back;

		v    = lf[i] / b_scale;
		back = (INT)(v + ((v >= 0.0f) ? 0.5f : -0.5f)) + (INT)b_zp;
		if(back != (INT)logit_q[i]) back_ok = FALSE;

		tm_printf((UB*)"  %2d %-15s %4d %4d %5d %6d %7d  %12d %4d%s\n", i, aed_test_class_names[i],
				(INT)logit_q[i], (INT)aed_test_logits_ort[i], (INT)aed_test_logits_ort_noopt[i],
				(INT)logit_q[i] - (INT)aed_test_logits_ort[i],
				(INT)logit_q[i] - (INT)aed_test_logits_ort_noopt[i],
				x10k(lf[i]), back, (back == (INT)logit_q[i]) ? "" : " <-");
	}
	tm_printf((UB*)"  max |npu - ort| = %d LSB, max |npu - ort_noopt| = %d LSB"
			" (the two PC references differ by up to %d LSB)\n", max_opt, max_flt, max_pc);
	tm_printf((UB*)"  softmax input float -> int8 matches captured int8: %s\n", back_ok ? "yes" : "NO");

	pass_opt = (max_opt <= LOGIT_TOL_LSB);
	pass_flt = (max_flt <= LOGIT_TOL_LSB);
	for(i = 0; i < NPU_RT_OUT_CLASSES && !pass_opt && !pass_flt; i++) {
		d_opt = (INT)logit_q[i] - (INT)aed_test_logits_ort[i];
		d_flt = (INT)logit_q[i] - (INT)aed_test_logits_ort_noopt[i];
		if(d_opt > LOGIT_TOL_LSB || d_opt < -LOGIT_TOL_LSB || d_flt > LOGIT_TOL_LSB || d_flt < -LOGIT_TOL_LSB) {
			tm_printf((UB*)"  outside +-%d LSB: %d %s (ort %+d, noopt %+d)\n", LOGIT_TOL_LSB,
					i, aed_test_class_names[i], d_opt, d_flt);
		}
	}

	pass = (pass_opt || pass_flt) && scale_ok && zp_ok && back_ok;
	tm_printf((UB*)"  logits %s +-%d LSB of a PC reference (reference only)\n",
			pass ? "within" : "NOT within", LOGIT_TOL_LSB);
	return pass;
}

/* ---------------------------------------------------------------- */
/* ESC-10 の実録音 (判定)                                              */
/* ---------------------------------------------------------------- */

#if NPU_HAVE_CLIPS
/*
 * 実録音を順に推論し、1位を PC (ONNX Runtime) の1位と比べる。
 * 合否: 全クリップで NPU の1位が PC のどちらか (最適化あり / なし) の1位と一致すれば PASS。
 * 乱数入力は学習データの分布外で上位2クラスが拮抗し、整数演算の丸めの違いの積み重ねで
 * 確率が大きく動くので、判定には実録音を使う (乱数入力の結果は参考値)
 */
LOCAL BOOL clips_check(void)
{
	float	out[NPU_RT_OUT_CLASSES];
	UW	us;
	INT	k, top, truth, t_ort, t_flt;
	INT	n_ort = 0, n_flt = 0, n_pc = 0, n_npu_truth = 0, n_ort_truth = 0, n_flt_truth = 0, n_run = 0;
	BOOL	same_pc, pass;
	ER	er;

	tm_printf((UB*)"npu clip test: %d ESC-10 recordings, NPU top-1 vs PC top-1 (p in x1e-4)\n", AED_CLIP_COUNT);
	tm_printf((UB*)"   # file               truth           PC top-1 (ort/noopt)    p_pc NPU top-1        p_npu     us\n");
	for(k = 0; k < AED_CLIP_COUNT; k++) {
		truth = aed_clip_truth[k];
		t_ort = aed_clip_top_ort[k];
		t_flt = aed_clip_top_noopt[k];
		if(t_ort == truth) n_ort_truth++;
		if(t_flt == truth) n_flt_truth++;

		er = run_once((const B *)aed_clip_input[k], out, &us, NULL, NULL);
		if(er != E_OK) {
			tm_printf((UB*)"  %2d %-18s %-15s inference failed: er=%d\n", k, aed_clip_file[k],
					aed_test_class_names[truth], er);
			continue;
		}
		n_run++;
		top = argmax(out);
		same_pc = (top == t_ort) || (top == t_flt);
		if(top == t_ort) n_ort++;
		if(top == t_flt) n_flt++;
		if(same_pc) n_pc++;
		if(top == truth) n_npu_truth++;

		tm_printf((UB*)"  %2d %-18s %-15s %-15s%s %5d %-15s %5d %6u %s\n", k, aed_clip_file[k],
				aed_test_class_names[truth], aed_test_class_names[t_ort], (t_flt == t_ort) ? "       " : "/diff  ",
				x10k(aed_clip_prob_ort[k]), aed_test_class_names[top], x10k(out[top]), us,
				same_pc ? "ok" : "MISMATCH");
	}
	tm_printf((UB*)"  NPU top-1 == PC top-1: %d/%d (ort %d, noopt %d), inference completed %d/%d\n",
			n_pc, AED_CLIP_COUNT, n_ort, n_flt, n_run, AED_CLIP_COUNT);
	tm_printf((UB*)"  top-1 == truth: NPU %d/%d, PC ort %d/%d, PC noopt %d/%d\n",
			n_npu_truth, AED_CLIP_COUNT, n_ort_truth, AED_CLIP_COUNT, n_flt_truth, AED_CLIP_COUNT);

	pass = (n_pc == AED_CLIP_COUNT);
	tm_printf((UB*)"npu clip test %s (NPU top-1 must match PC top-1 for every clip)\n", pass ? "PASS" : "FAIL");
	return pass;
}
#endif	/* NPU_HAVE_CLIPS */

/* ---------------------------------------------------------------- */
/* 手順2・3                                                           */
/* ---------------------------------------------------------------- */

/* 戻り値: 最初の推論が完了したら TRUE (出力が期待値と合わなくても)。そのときだけ手順5 を行う */
LOCAL BOOL npu_selftest(void)
{
	float	out[NPU_RT_OUT_CLASSES];
	B	before[HEAD_BYTES];
	UW	us, in_diff, t_min, t_max, t_sum, n_ok;
	INT	i, top;
	float	d_opt, d_flt;
	BOOL	pass = FALSE, near;
	ER	er;

	tm_printf((UB*)"npu random-input test (reference only, not a pass/fail criterion): seed=%d\n",
			AED_TEST_SEED);

	/* 手順2: 乱数入力で1回推論して PC と比べる (参考値) */
	er = run_once(aed_test_input, out, &us, before, &in_diff);
	tm_printf((UB*)"  input in RAM before inference: %u of %d bytes differ from aed_test_input\n",
			in_diff, NPU_RT_IN_BYTES);
	if(er != E_OK) {
		tm_printf((UB*)"  inference failed: er=%d after %u us\n", er, us);
		show_input_heads(aed_test_input, before);
		tm_printf((UB*)"npu selftest FAIL (inference did not complete)\n");
		return FALSE;
	}
	memcpy(ref_out, out, sizeof(ref_out));
	ref_valid = TRUE;

	tm_printf((UB*)"  inference: %u us\n", us);
	tm_printf((UB*)"   # class              npu    ort  ort_noopt  (x1e-4)\n");
	for(i = 0; i < NPU_RT_OUT_CLASSES; i++) {
		tm_printf((UB*)"  %2d %-15s %6d %6d %6d\n", i, aed_test_class_names[i],
				x10k(out[i]), x10k(aed_test_expect_ort[i]), x10k(aed_test_expect_ort_float[i]));
	}
	top   = argmax(out);
	d_opt = max_abs_diff(out, aed_test_expect_ort);
	d_flt = max_abs_diff(out, aed_test_expect_ort_float);
	tm_printf((UB*)"  top1: %d %s (%d x1e-4)\n", top, aed_test_class_names[top], x10k(out[top]));
	tm_printf((UB*)"  max |npu - ort| = %d, max |npu - ort_noopt| = %d (x1e-4)\n", x10k(d_opt), x10k(d_flt));

	/* 大きく外れたら、キャッシュの取り違えを確かめられるよう入力バッファの先頭を出す */
	near = (strcmp(aed_test_class_names[top], ST_EXPECT_TOP) == 0)
		&& ((d_opt <= ST_TOLERANCE) || (d_flt <= ST_TOLERANCE));
	if(!near) show_input_heads(aed_test_input, before);
	tm_printf((UB*)"  random input: top1 %s PC, output %s %d x1e-4 of a PC reference (reference only)\n",
			(strcmp(aed_test_class_names[top], ST_EXPECT_TOP) == 0) ? "matches" : "differs from",
			((d_opt <= ST_TOLERANCE) || (d_flt <= ST_TOLERANCE)) ? "within" : "NOT within",
			x10k(ST_TOLERANCE));

	/* softmax 直前の int8 ロジットを PC と比べる (推論をもう1回。参考値) */
	(void)logits_check();

	/* 判定: ESC-10 の実録音で1位を PC と比べる */
#if NPU_HAVE_CLIPS
	pass = clips_check();
	tm_printf((UB*)"npu selftest %s (judged by the clip test)\n", pass ? "PASS" : "FAIL");
#else
	(void)pass;
	tm_printf((UB*)"npu clip test: SKIP (aed_test_clips.h not found: run scripts/aed_clips.py)\n");
	tm_printf((UB*)"npu selftest NOT JUDGED\n");
#endif

	/* 手順3: 同じ入力で連続実行 */
	tm_printf((UB*)"npu repeat x%d (same input):\n", ST_REPEAT);
	t_min = 0xFFFFFFFFU;
	t_max = 0;
	t_sum = 0;
	n_ok  = 0;
	for(i = 0; i < ST_REPEAT; i++) {
		er = run_once(aed_test_input, out, &us, NULL, NULL);
		if(er != E_OK) {
			tm_printf((UB*)"  [%2d] er=%d after %u us\n", i + 1, er, us);
			continue;
		}
		tm_printf((UB*)"  [%2d] %6u us, max diff vs first = %d x1e-4%s\n", i + 1, us,
				x10k(max_abs_diff(out, ref_out)),
				(memcmp(out, ref_out, sizeof(out)) == 0) ? " (bit-identical)" : "");
		if(us < t_min) t_min = us;
		if(us > t_max) t_max = us;
		t_sum += us;
		n_ok++;
	}
	if(n_ok > 0) {
		tm_printf((UB*)"  inference time: min=%u mean=%u max=%u us (%u of %d runs OK)\n",
				t_min, t_sum / n_ok, t_max, n_ok, ST_REPEAT);
	} else {
		tm_printf((UB*)"  no run completed\n");
	}
	return TRUE;
}

/* ---------------------------------------------------------------- */
/* 手順5: パススルー稼働中の推論                                        */
/* ---------------------------------------------------------------- */

#if NPU_PT_TEST
LOCAL void pt_test(void)
{
	float	out[NPU_RT_OUT_CLASSES];
	UW	us[PT_REPEAT];
	ER	ers[PT_REPEAT];
	INT	dif[PT_REPEAT];		/* 最初の推論の出力との差の最大値 (x1e-4) */
	BOOL	same[PT_REPEAT];
	UW	u0, o0, l0, u1, o1, l1, t0, el_ms, t_min, t_max, t_sum, n_ok;
	INT	i, k;
	BOOL	pass;

	for(i = 0; i < PT_START_POLL && !audio_passthrough_active(); i++) tk_dly_tsk(100);
	if(!audio_passthrough_active()) {
		tm_printf((UB*)"npu pt test: SKIP (passthrough did not start)\n");
		return;
	}

	audio_pt_counts(&u0, &o0, &l0);
	for(k = 0; k < PT_REPEAT; k++) {
		t0      = NOW();
		ers[k]  = run_once(aed_test_input, out, &us[k], NULL, NULL);
		dif[k]  = (ers[k] == E_OK) ? x10k(max_abs_diff(out, ref_out)) : -1;
		same[k] = (ers[k] == E_OK) && (memcmp(out, ref_out, sizeof(out)) == 0);
		if(k == PT_REPEAT - 1) break;
		el_ms = trace_cyc_to_us((UW)(NOW() - t0)) / 1000U;
		tk_dly_tsk((el_ms < PT_INTERVAL_MS) ? (PT_INTERVAL_MS - el_ms) : 1);
	}
	audio_pt_counts(&u1, &o1, &l1);

	/*
	 * usermain がパススルーの立ち上がりと同時に10秒のトレースを始め、その後 CSV をダンプする。
	 * ダンプ中の出力は CSV に混ざるので、記録とダンプが終わってからまとめて出す
	 * (推論の前後は TRACE の EV_INF_START / EV_INF_END として CSV に残っている)
	 */
	for(i = 0; i < PT_REPORT_POLL && trace_busy(); i++) tk_dly_tsk(100);

	tm_printf((UB*)"npu pt test: %d runs every %d ms during passthrough (task pri %d)\n",
			PT_REPEAT, PT_INTERVAL_MS, NPU_TASK_PRI);
	t_min = 0xFFFFFFFFU;
	t_max = 0;
	t_sum = 0;
	n_ok  = 0;
	for(k = 0; k < PT_REPEAT; k++) {
		if(ers[k] != E_OK) {
			tm_printf((UB*)"  [%2d] er=%d after %u us\n", k + 1, ers[k], us[k]);
			continue;
		}
		tm_printf((UB*)"  [%2d] %6u us, max diff vs first = %d x1e-4%s\n", k + 1, us[k], dif[k],
				same[k] ? " (bit-identical)" : "");
		if(us[k] < t_min) t_min = us[k];
		if(us[k] > t_max) t_max = us[k];
		t_sum += us[k];
		n_ok++;
	}
	if(n_ok > 0) {
		tm_printf((UB*)"  inference time: min=%u mean=%u max=%u us (%u of %d runs OK)\n",
				t_min, t_sum / n_ok, t_max, n_ok, PT_REPEAT);
	}
	tm_printf((UB*)"  audio before: under=%u over=%u late=%u / after: under=%u over=%u late=%u\n",
			u0, o0, l0, u1, o1, l1);

	pass = (n_ok == PT_REPEAT) && (u1 == u0) && (o1 == o0) && (l1 == l0);
	for(k = 0; k < PT_REPEAT; k++) {
		if(ers[k] == E_OK && dif[k] > x10k(ST_TOLERANCE)) pass = FALSE;
	}
	tm_printf((UB*)"npu pt test %s (all runs OK, output within %d x1e-4 of first, no new under/over/late)\n",
			pass ? "PASS" : "FAIL", x10k(ST_TOLERANCE));
}
#endif	/* NPU_PT_TEST */

/* ---------------------------------------------------------------- */

LOCAL void task_npu(INT stacd, void *exinf)
{
	ER	er;
	BOOL	ran = FALSE;

	er = npu_rt_init();
	tm_printf((UB*)"npu_rt_init: ret=%d\n", er);
	if(er == E_OK) ran = npu_selftest();

	(void)tk_sig_sem(semid_done, 1);

#if NPU_PT_TEST
	if(ran) {
		pt_test();
	} else {
		tm_printf((UB*)"npu pt test: SKIP (selftest inference did not complete)\n");
	}
#else
	(void)ran;
#endif

	tk_ext_tsk();
}

EXPORT ER npu_task_start(void)
{
	ER	er;

	semid_done = tk_cre_sem(&csem_done);
	if(semid_done < E_OK) return semid_done;

	tskid_npu = tk_cre_tsk(&ctsk_npu);
	if(tskid_npu < E_OK) return tskid_npu;

	er = tk_sta_tsk(tskid_npu, 0);
	if(er < E_OK) return er;

	return tk_wai_sem(semid_done, 1, START_WAIT_MS);
}
