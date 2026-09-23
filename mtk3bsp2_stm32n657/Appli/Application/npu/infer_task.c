#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <string.h>		// memcpy, memcmp
#include <math.h>		// sqrtf, log10f
#include "infer_task.h"
#include "npu_rt.h"
#include "npu_selftest.h"
#include "../audio/tap_ring.h"
#include "../audio/audio_task.h"
#include "../trace/trace.h"	// trace_muted(), trace_busy(), trace_cyc_per_us()

/*
 * 推論タスク (Phase 1 タスク7 で骨組み。タスク9 で前処理と推論を載せる)
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
 *   1. npu_rt_init() と自己テスト (npu_selftest.c)。終わったら usermain に知らせる
 *      (usermain は推論時間を音声の負荷なしで測るため、音声を始める前にこれを待つ)
 *   2. タップリングから窓を取り出す。書き手 (task_pcm) が窓をそろえたときにセマフォで起きる
 *      - 窓を静的バッファ win_buf へコピー
 *      - 音量 (RMS・ピーク) と、前の窓の末尾 240 サンプルとの一致を確かめて1行表示
 *      - タスク9: ここで win_buf を log-mel にして推論する
 *   3. NPU_PT_TEST が 1 なら、パススルー稼働中の最初の 10 窓で乱数入力の推論を1回ずつ行い、
 *      前後で音声の under / over / late が増えないかを見る (推論を載せたときの予行)。
 *      タスク6 の npu pt test (同じ優先度の別ループで 960ms ごとに推論) をここへ統合した。
 *      間隔はタップリングの窓 (15360 サンプル = 約 960ms) で決まる
 *
 * 優先度 15 は音声 (task_pcm 5、task_audio・task_1・task_2 10) より低く、reporter (20) より高い。
 * POLLING の推論中はこのタスクが CPU を回し続けるので、推論のあいだ reporter と dump は動けない。
 */

/* 1: パススルー中の推論の予行を行う。0 で無効 */
#define NPU_PT_TEST		(1)

#define INFER_TASK_PRI		(15)
#define INFER_TASK_STKSZ	(8 * 1024)
#define START_WAIT_MS		(30000)		/* usermain が自己テストの終わりを待つ上限 */

#define WIN_WAIT_MS		(2000)		/* 窓は約 960ms ごと。これだけ来なければ音声が止まったとみなす */
#define SUMMARY_EVERY		(10)		/* 累計を出す間隔 (窓の数) */
#define PT_REPEAT		(10)		/* 予行の回数 */

#define LVL_BAR_LEN		(20)		/* 音量のバーの長さ */
#define LVL_BAR_DB_MIN		(-60)		/* バーの左端 (dBFS)。1文字 3dB */

LOCAL void task_infer(INT stacd, void *exinf);
LOCAL ID	tskid_infer;
LOCAL T_CTSK	ctsk_infer = {
	.itskpri	= INFER_TASK_PRI,
	.stksz		= INFER_TASK_STKSZ,
	.task		= task_infer,
	.tskatr		= TA_HLNG | TA_RNG3,
};

/*
 * 自己テストが終わったことを usermain に知らせる。tk_wup_tsk を使わないのは、usermain が
 * 待ちをタイムアウトした後に起床要求だけが残ると、usermain 最後の tk_slp_tsk(TMO_FEVR) が
 * すぐ戻って usermain が終わってしまうため
 */
LOCAL ID	semid_done;
LOCAL T_CSEM	csem_done = {
	.sematr		= TA_TFIFO | TA_FIRST,
	.isemcnt	= 0,
	.maxsem		= 1,
};

/* 取り出した窓と、つなぎ目の確認用の前の窓の末尾 (静的領域。タスクのスタックに置かない) */
LOCAL H		win_buf[TAP_WIN_LEN];
LOCAL H		prev_tail[TAP_WIN_OVERLAP];
LOCAL BOOL	have_prev = FALSE;
LOCAL UW	prev_pos;
LOCAL UW	seam_ok_n, seam_ng_n;

/* ---------------------------------------------------------------- */
/* 窓の確認                                                            */
/* ---------------------------------------------------------------- */

/* 窓全体の RMS・ピーク (int16 の値) と、RMS の dBFS (フルスケール 32768 が 0dB) */
LOCAL void window_level(const H *x, UW *rms, UW *peak, INT *dbfs)
{
	uint64_t	sq = 0;
	UW		pk = 0, a;
	INT		i;
	float		r;

	for(i = 0; i < TAP_WIN_LEN; i++) {
		a = (x[i] < 0) ? (UW)(-(W)x[i]) : (UW)x[i];
		if(a > pk) pk = a;
		sq += (uint64_t)((W)x[i] * (W)x[i]);
	}
	r     = sqrtf((float)sq / (float)TAP_WIN_LEN);
	*rms  = (UW)(r + 0.5f);
	*peak = pk;
	*dbfs = (r >= 1.0f) ? (INT)(20.0f * log10f(r / 32768.0f) - 0.5f) : -99;
}

/* 話しかけたときに伸びるのが一目で分かるバー (LVL_BAR_DB_MIN から 3dB ごとに1文字) */
LOCAL void level_bar(INT dbfs, char *bar)
{
	INT	i, n = (dbfs - LVL_BAR_DB_MIN) / 3;

	if(n < 0) n = 0;
	if(n > LVL_BAR_LEN) n = LVL_BAR_LEN;
	for(i = 0; i < LVL_BAR_LEN; i++) bar[i] = (i < n) ? '#' : '.';
	bar[LVL_BAR_LEN] = '\0';
}

/*
 * 窓の先頭 240 サンプルが、前の窓の末尾 240 サンプルと同じか。前の窓と連続していない
 * (最初の窓、上書きで読み位置を飛ばした直後) ときは確かめない
 */
LOCAL const char *seam_check(const TAP_WIN_INFO *info)
{
	const char	*r;

	if(!have_prev || info->resync || info->pos != prev_pos + TAP_WIN_HOP) {
		r = "-";
	} else if(memcmp(win_buf, prev_tail, sizeof(prev_tail)) == 0) {
		seam_ok_n++;
		r = "ok";
	} else {
		seam_ng_n++;
		r = "NG";
	}
	memcpy(prev_tail, &win_buf[TAP_WIN_HOP], sizeof(prev_tail));
	prev_pos  = info->pos;
	have_prev = TRUE;
	return r;
}

LOCAL UW cyc_to_ns(UW cyc)
{
	return (UW)(((uint64_t)cyc * 1000ULL) / trace_cyc_per_us());
}

/* 累計 (10分の確認で見る値をまとめて出す) */
LOCAL void show_summary(const char *why)
{
	TAP_STATS	st;
	UW		under, over, late;

	tap_ring_stats(&st);
	audio_pt_counts(&under, &over, &late);
	tm_printf((UB*)"tap %s: windows=%u (expected %u from %u samples) overrun=%u torn=%u skipped=%u"
			" seam ok=%u NG=%u | lag max=%uus (%u of them lower bounds)"
			" | tap write max=%uns avg=%uns (%u calls) | audio under=%u over=%u late=%u\n",
			why, st.windows, tap_ring_expected_windows(), st.head, st.overrun, st.torn, st.skipped,
			seam_ok_n, seam_ng_n, st.lag_max_us, st.late,
			cyc_to_ns(st.wr_max_cyc), (st.wr_calls > 0) ? cyc_to_ns(st.wr_sum_cyc / st.wr_calls) : 0,
			st.wr_calls, under, over, late);
}

/* ---------------------------------------------------------------- */
/* パススルー中の推論の予行                                            */
/* ---------------------------------------------------------------- */

#if NPU_PT_TEST
LOCAL INT	pt_n = 0;			/* 済んだ回数 */
LOCAL BOOL	pt_reported = FALSE;
LOCAL UW	pt_us[PT_REPEAT];
LOCAL ER	pt_er[PT_REPEAT];
LOCAL INT	pt_dif[PT_REPEAT];		/* 最初の推論の出力との差の最大値 (x1e-4) */
LOCAL BOOL	pt_same[PT_REPEAT];
LOCAL UW	pt_u0, pt_o0, pt_l0, pt_u1, pt_o1, pt_l1;

LOCAL void pt_report(void)
{
	UW	t_min = 0xFFFFFFFFU, t_max = 0, t_sum = 0, n_ok = 0;
	INT	k;
	BOOL	pass;

	tm_printf((UB*)"npu pt test: fixed-input inference on %d tap windows during passthrough (task pri %d)\n",
			PT_REPEAT, INFER_TASK_PRI);
	for(k = 0; k < PT_REPEAT; k++) {
		if(pt_er[k] != E_OK) {
			tm_printf((UB*)"  [%2d] er=%d after %u us\n", k + 1, pt_er[k], pt_us[k]);
			continue;
		}
		tm_printf((UB*)"  [%2d] %6u us, max diff vs first = %d x1e-4%s\n", k + 1, pt_us[k], pt_dif[k],
				pt_same[k] ? " (bit-identical)" : "");
		if(pt_us[k] < t_min) t_min = pt_us[k];
		if(pt_us[k] > t_max) t_max = pt_us[k];
		t_sum += pt_us[k];
		n_ok++;
	}
	if(n_ok > 0) {
		tm_printf((UB*)"  inference time: min=%u mean=%u max=%u us (%u of %d runs OK)\n",
				t_min, t_sum / n_ok, t_max, n_ok, PT_REPEAT);
	}
	tm_printf((UB*)"  audio before: under=%u over=%u late=%u / after: under=%u over=%u late=%u\n",
			pt_u0, pt_o0, pt_l0, pt_u1, pt_o1, pt_l1);

	pass = (n_ok == PT_REPEAT) && (pt_u1 == pt_u0) && (pt_o1 == pt_o0) && (pt_l1 == pt_l0);
	for(k = 0; k < PT_REPEAT; k++) {
		if(pt_er[k] == E_OK && pt_dif[k] > NPU_SELFTEST_TOL_X1E4) pass = FALSE;
	}
	tm_printf((UB*)"npu pt test %s (all runs OK, output within %d x1e-4 of first, no new under/over/late)\n",
			pass ? "PASS" : "FAIL", NPU_SELFTEST_TOL_X1E4);
}

/* 窓ごとに1回呼ぶ */
LOCAL void pt_step(BOOL npu_ok)
{
	if(!npu_ok || pt_reported) return;

	if(pt_n < PT_REPEAT) {
		if(!audio_passthrough_active()) return;
		if(pt_n == 0) audio_pt_counts(&pt_u0, &pt_o0, &pt_l0);
		pt_er[pt_n] = npu_selftest_run_fixed(&pt_us[pt_n], &pt_dif[pt_n], &pt_same[pt_n]);
		pt_n++;
		if(pt_n == PT_REPEAT) audio_pt_counts(&pt_u1, &pt_o1, &pt_l1);
		return;
	}

	/*
	 * usermain がパススルーの立ち上がりと同時に10秒のトレースを始め、その後 CSV をダンプする。
	 * ダンプ中の出力は CSV に混ざるので、記録とダンプが終わってから出す
	 */
	if(trace_busy()) return;
	pt_report();
	pt_reported = TRUE;
}
#endif	/* NPU_PT_TEST */

/* ---------------------------------------------------------------- */

LOCAL void task_infer(INT stacd, void *exinf)
{
	TAP_WIN_INFO	info;
	TAP_STATS	st;
	UW		rms, peak;
	INT		dbfs;
	char		bar[LVL_BAR_LEN + 1];
	const char	*seam;
	BOOL		npu_ok = FALSE, idle = FALSE;
	ER		er;

	er = npu_rt_init();
	tm_printf((UB*)"npu_rt_init: ret=%d\n", er);
	if(er == E_OK) npu_ok = npu_selftest();
	(void)tk_sig_sem(semid_done, 1);

	for(;;) {
		er = tap_ring_get_window(win_buf, WIN_WAIT_MS, &info);
		if(er == E_TMOUT) {
			/* 音声が止まった (パススルーの規定時間が過ぎた等)。始まる前 (head=0) は黙る */
			tap_ring_stats(&st);
			if(!idle && st.head > 0 && !trace_muted()) {
				tm_printf((UB*)"tap: no window for %d ms (audio stopped?)\n", WIN_WAIT_MS);
				show_summary("final");
				idle = TRUE;
			}
			continue;
		}
		if(er != E_OK) {
			/* 上書きされていた (E_OBJ) / コピー中に上書きされた (E_IO)。読み位置は最新の窓へ進んでいる */
			if(!trace_muted()) {
				tm_printf((UB*)"tap: window lost (%s), jumped to the newest window\n",
						(er == E_OBJ) ? "overwritten before read" : "overwritten while copying");
			}
			continue;
		}
		idle = FALSE;

		window_level(win_buf, &rms, &peak, &dbfs);
		seam = seam_check(&info);

		/* タスク9: ここで win_buf を log-mel にして推論する */

#if NPU_PT_TEST
		pt_step(npu_ok);
#else
		(void)npu_ok;
#endif

		if(!trace_muted()) {
			level_bar(dbfs, bar);
			tm_printf((UB*)"win %4u pos=%8u %4ddBFS [%s] rms=%5u peak=%5u seam=%s lag=%s%uus\n",
					info.seq, info.pos, dbfs, bar, rms, peak, seam,
					info.lag_exact ? "" : ">=", info.lag_us);
			if(((info.seq + 1U) % SUMMARY_EVERY) == 0) show_summary("total");
		}
	}
}

EXPORT ER infer_task_start(void)
{
	ER	er;

	semid_done = tk_cre_sem(&csem_done);
	if(semid_done < E_OK) return semid_done;

	tskid_infer = tk_cre_tsk(&ctsk_infer);
	if(tskid_infer < E_OK) return tskid_infer;

	er = tk_sta_tsk(tskid_infer, 0);
	if(er < E_OK) return er;

	return tk_wai_sem(semid_done, 1, START_WAIT_MS);
}
