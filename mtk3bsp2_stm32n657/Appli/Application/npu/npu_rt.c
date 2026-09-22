#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <setjmp.h>
#include <stdio.h>		// setvbuf
#include <assert.h>		// __assert_func の宣言 (newlib)
#include "main.h"		// CMSIS (NVIC)
#include "stai_network.h"	// st/model/ (ST 生成コード)
#include "ll_aton.h"		// startWatchdog / checkWatchdog の宣言、ATON_STD_IRQn
#include "ll_aton_version.h"
#include "npu_rt.h"
#include "npu_hw.h"
#include "../trace/trace.h"	// NOW(), trace_cyc_per_us(), TRACE()
#include "../fault/fault_out.h"	// fault_putc / fault_puts / fault_halt

/*
 * ST Edge AI ランタイム (st/ll_aton) と AED モデル (st/model) をこのアプリにつなぐ部分。
 * ST のファイルは変えず、ランタイムが用意している弱いシンボルとフックだけを使う。
 *
 * 1. NPU 割り込み
 *    ll_aton は POLLING でも LL_ATON_RT_RuntimeInit() (stai_runtime_init() の中) で
 *    NPU0_IRQn を NVIC 有効にする (ll_aton_runtime.c:555-559)。ハンドラ NPU0_IRQHandler も
 *    ll_aton_runtime.c が強いシンボルで定義していてベクタに載る (fault の差し替え対象外)。
 *    POLLING でのハンドラの役目はエラー通知だけで、中身は printf して assert で止まること
 *    (ll_aton_runtime.c:1002-1004)。優先度はリセット値 0 のままで、カーネルの DI
 *    (BASEPRI=1) でも止まらない。本機は NPU 割り込みを使わない方針なので、初期化の直後に
 *    無効へ戻す。
 *
 * 2. 推論のタイムアウト (DWT CYCCNT)
 *    POLLING の待ちは ll_aton の LL_Streng_Wait() (ll_aton.c:461-484) で、中は
 *      startWatchdog(ATON_EPOCH_TIMEOUT);
 *      do { 実行中の DMA を見る; LL_ATON_ASSERT(checkWatchdog() == 0); } while (実行中);
 *    startWatchdog / checkWatchdog は ll_aton.c:142-156 の弱いシンボル (既定は何もしない)。
 *    ここで定義して、ll_aton のソースを変えずに上限を入れる。
 *    - 上限は待ち1回ごとでなく推論1回全体 (npu_rt_run の開始から NPU_RT_TIMEOUT_US)。
 *      startWatchdog の引数 (ST の既定 10 秒) は使わない
 *    - 超えたら checkWatchdog から npu_rt_run の setjmp 地点へ longjmp で戻る。
 *      checkWatchdog が 0 以外を返すと LL_ATON_ASSERT で止まってしまうため
 *    - npu_rt_run の外 (推論以外の ll_aton の待ち) では上限を掛けない (ST の既定と同じ)
 *    - NDEBUG を定義すると LL_ATON_ASSERT ごと checkWatchdog の呼び出しが消えるので下で弾く
 *    - longjmp で抜けた後の ll_aton は推論の途中の状態のまま (実行中の epoch、NPU の所有者、
 *      NPU の DMA)。後始末はタスク6。それまでは以降の npu_rt_run を E_IO で断る
 *
 * 3. newlib の出力と assert
 *    ll_aton はエラーの経路で printf / puts / assert を使う (ll_aton_util.h の
 *    LL_ATON_PRINTF, LL_ATON_ASSERT)。このままだと
 *    - 出力: syscalls.c の _write は weak の __io_putchar を呼ぶが、定義がどこにも無く番地0へ飛ぶ
 *    - stdout のバッファ: newlib は初回の出力で malloc する。_sbrk (sysmem.c) のヒープは
 *      _end から伸び、μT-Kernel のシステムメモリも _end から始まる (sys_start.c) ので重なる
 *    - assert: newlib の __assert_func は表示の後 abort() → _exit() の無限ループで黙って止まる
 *    そこで __io_putchar を fault の UART 直接出力につなぎ、stdout を無バッファにして malloc を
 *    起こさない。assert は理由を出して fault_halt() で止める (fault と同じ扱い)。
 */

#ifdef NDEBUG
#error "NDEBUG では LL_ATON_ASSERT(checkWatchdog() == 0) が消え、推論のタイムアウトが効かない"
#endif

/* npu_rt.h の値とモデルが食い違っていたらビルドを止める */
_Static_assert(NPU_RT_IN_BYTES == STAI_NETWORK_IN_1_SIZE_BYTES, "input size");
_Static_assert(STAI_NETWORK_IN_1_FORMAT == STAI_FORMAT_S8, "input format");
_Static_assert(NPU_RT_OUT_CLASSES == STAI_NETWORK_OUT_1_SIZE, "output size");
_Static_assert(STAI_NETWORK_OUT_1_FORMAT == STAI_FORMAT_FLOAT32, "output format");

/* ネットワークの実行状態 (stai の context)。ST の ai_dpu.c と同じ宣言 */
LOCAL STAI_NETWORK_CONTEXT_DECLARE(net, STAI_NETWORK_CONTEXT_SIZE)

LOCAL BOOL		ready = FALSE;
LOCAL BOOL		broken = FALSE;		/* タイムアウトして ll_aton が推論の途中のまま */
LOCAL B			*in_buf = NULL;
LOCAL const float	*out_buf = NULL;
LOCAL UW		timeouts = 0;		/* 書くのは推論を呼ぶタスクだけ */

/* タイムアウトの監視 (推論を呼ぶタスクの中だけで読み書きする) */
LOCAL jmp_buf		wd_jmp;
LOCAL volatile BOOL	wd_armed = FALSE;
LOCAL UW		wd_t0;
LOCAL UW		wd_lim;
LOCAL UW		wd_elapsed;

/* 1ステップの結果を UART に出す (npu_hw.c と同じ書式) */
LOCAL BOOL rt_step(const char *name, BOOL ok)
{
	tm_printf(ok ? (UB*)"  [ OK ] %s\n" : (UB*)"  [FAIL] %s\n", name);
	return ok;
}

/* ---------------------------------------------------------------- */
/* ll_aton から呼ばれる関数 (弱いシンボルの差し替え)                    */
/* ---------------------------------------------------------------- */

/* 上限は npu_rt_run が決めるので、ここでは何もしない (timeout は ST 既定の 10 秒) */
EXPORT int startWatchdog(uint32_t timeout)
{
	(void)timeout;
	return 0;
}

/* LL_Streng_Wait のポーリング1周ごとに呼ばれる */
EXPORT int checkWatchdog(void)
{
	UW	dt;
	UW	ms;

	if(!wd_armed) return 0;

	dt = (UW)(NOW() - wd_t0);
	if(dt <= wd_lim) return 0;

	/* タイムアウト。記録して npu_rt_run へ戻る (表示は戻った先で) */
	wd_armed   = FALSE;
	wd_elapsed = dt;
	timeouts++;
	ms = trace_cyc_to_us(dt) / 1000U;
	TRACE(EV_INF_TMO, (ms > 255U) ? 255U : ms);
	longjmp(wd_jmp, 1);
}

/* ---------------------------------------------------------------- */
/* newlib から呼ばれる関数                                             */
/* ---------------------------------------------------------------- */

/* syscalls.c の _write から1文字ずつ呼ばれる (weak の宣言を実体で埋める) */
EXPORT int __io_putchar(int ch)
{
	fault_putc((char)ch);
	return ch;
}

/* assert() の失敗。ll_aton の LL_ATON_ASSERT もここに来る */
EXPORT void __assert_func(const char *file, int line, const char *func, const char *expr)
{
	fault_puts("\n[ASSERT] ");
	fault_puts((expr != NULL) ? expr : "?");
	fault_puts("\n  at ");
	fault_puts((file != NULL) ? file : "?");
	fault_putc(':');
	fault_putdec((UW)line);
	if(func != NULL) {
		fault_puts(" (");
		fault_puts(func);
		fault_putc(')');
	}
	fault_putc('\n');
	fault_halt();
	for(;;) { }	/* fault_halt は戻らない。noreturn の宣言に合わせる */
}

/* ---------------------------------------------------------------- */

/* shape を "1x64x96x1" の形で出す */
LOCAL void show_shape(const stai_shape *s)
{
	stai_size	i;

	for(i = 0; i < s->size; i++) {
		tm_printf((i == 0) ? (UB*)"%d" : (UB*)"x%d", (INT)s->data[i]);
	}
}

LOCAL void show_tensor(const char *name, const stai_tensor *t, uintptr_t addr)
{
	tm_printf((UB*)"  %s \"%s\" @ 0x%08x: %u B, format=0x%08x, shape=",
			name, (t->name != NULL) ? t->name : "", (UW)addr, (UW)t->size_bytes, (UW)t->format);
	show_shape(&t->shape);
	if(t->scale.size > 0 && t->zeropoint.size > 0) {
		/* tm_printf は浮動小数点を出せないので 1e-9 単位の整数で出す */
		tm_printf((UB*)", scale=%u e-9, zero_point=%d",
				(UW)(t->scale.data[0] * 1e9f + 0.5f), (INT)t->zeropoint.data[0]);
	}
	tm_printf((UB*)"\n");
}

EXPORT ER npu_rt_init(void)
{
	stai_return_code	rc;
	stai_network_info	info;
	stai_ptr		in[STAI_NETWORK_IN_NUM];
	stai_ptr		out[STAI_NETWORK_OUT_NUM];
	stai_size		n_in = 0, n_out = 0;
	BOOL			ok;

	tm_printf((UB*)"npu rt init (ll_aton %s, model %s)\n",
			LL_ATON_VERSION_NAME, STAI_NETWORK_ORIGIN_MODEL_NAME);

	if(!npu_hw_ready()) {
		/* NPU にクロックが無いと LL_ATON_Init がバージョンの読み直しを続けて戻らない */
		tm_printf((UB*)"  [SKIP] runtime init (npu_hw_init was not OK)\n");
		return E_OBJ;
	}
	if(!trace_cyccnt_valid()) {
		tm_printf((UB*)"  [WARN] DWT CYCCNT not running: inference timeout cannot be detected\n");
	}

	/* 最初の出力より前に。以後 ll_aton の printf / puts は malloc を起こさない */
	(void)setvbuf(stdout, NULL, _IONBF, 0);

	/* NPU (ATON) のクロックゲート・バス I/F・割り込みコントローラの初期化 */
	rc = stai_runtime_init();

	/* 上の中で有効にされた NPU の割り込みを戻す (結果にかかわらず) */
	NVIC_DisableIRQ(ATON_STD_IRQn);
	NVIC_ClearPendingIRQ(ATON_STD_IRQn);
	__DSB();
	__ISB();

	tm_printf((UB*)"  stai_runtime_init: rc=0x%x\n", (UW)rc);
	if(!rt_step("stai_runtime_init", rc == STAI_SUCCESS)) return E_IO;

	tm_printf((UB*)"  NVIC IRQ %d (NPU0): enabled=%d pending=%d\n", (INT)ATON_STD_IRQn,
			(INT)NVIC_GetEnableIRQ(ATON_STD_IRQn), (INT)NVIC_GetPendingIRQ(ATON_STD_IRQn));
	(void)rt_step("NPU interrupt disabled (polling only)", NVIC_GetEnableIRQ(ATON_STD_IRQn) == 0);

	rc = stai_network_init(net);
	tm_printf((UB*)"  stai_network_init: rc=0x%x (context %u B)\n", (UW)rc, (UW)STAI_NETWORK_CONTEXT_SIZE);
	if(!rt_step("stai_network_init", rc == STAI_SUCCESS)) return E_IO;

	rc = stai_network_get_info(net, &info);
	if(!rt_step("stai_network_get_info", rc == STAI_SUCCESS)) {
		tm_printf((UB*)"  rc=0x%x\n", (UW)rc);
		return E_IO;
	}
	tm_printf((UB*)"  c_model=%s (%s), runtime %d.%d.%d, tool %d.%d.%d, macc=%u, nodes=%u\n",
			info.c_model_name, info.c_model_datetime,
			info.runtime_version.major, info.runtime_version.minor, info.runtime_version.micro,
			info.tool_version.major, info.tool_version.minor, info.tool_version.micro,
			(UW)info.n_macc, (UW)info.n_nodes);
	tm_printf((UB*)"  n_inputs=%d n_outputs=%d n_activations=%d n_weights=%d\n",
			info.n_inputs, info.n_outputs, info.n_activations, info.n_weights);

	rc = stai_network_get_inputs(net, in, &n_in);
	ok = (rc == STAI_SUCCESS) && (n_in == 1) && (in[0] != NULL);
	rc = stai_network_get_outputs(net, out, &n_out);
	ok = ok && (rc == STAI_SUCCESS) && (n_out == 1) && (out[0] != NULL);
	if(!rt_step("stai_network_get_inputs/outputs", ok)) {
		tm_printf((UB*)"  rc=0x%x n_in=%u n_out=%u\n", (UW)rc, (UW)n_in, (UW)n_out);
		return E_IO;
	}
	show_tensor("input ", &info.inputs[0], (uintptr_t)in[0]);
	show_tensor("output", &info.outputs[0], (uintptr_t)out[0]);

	/* D キャッシュの保守を 32B 単位で行うので、入力の先頭がラインの境界にあること */
	if(!rt_step("input buffer 32B aligned", ((uintptr_t)in[0] & 0x1FU) == 0)) return E_IO;

	in_buf  = (B *)in[0];
	out_buf = (const float *)out[0];
	ready   = TRUE;
	tm_printf((UB*)"npu rt ready (inference timeout %u us)\n", (UW)NPU_RT_TIMEOUT_US);
	return E_OK;
}

EXPORT BOOL npu_rt_ready(void)
{
	return ready;
}

EXPORT B *npu_rt_input(void)
{
	return ready ? in_buf : NULL;
}

EXPORT const float *npu_rt_output(void)
{
	return ready ? out_buf : NULL;
}

EXPORT ER npu_rt_run(void)
{
	stai_return_code	rc;

	if(!ready) return E_OBJ;
	if(broken) return E_IO;

	wd_lim = trace_cyc_per_us() * NPU_RT_TIMEOUT_US;
	wd_t0  = NOW();

	if(setjmp(wd_jmp) != 0) {
		/* checkWatchdog からの longjmp。ll_aton は推論の途中で止まっている */
		broken = TRUE;
		tm_printf((UB*)"[npu] inference TIMEOUT: %u us > %u us (count=%u)."
				" runtime left mid-inference, further runs refused until recovery is implemented\n",
				trace_cyc_to_us(wd_elapsed), (UW)NPU_RT_TIMEOUT_US, timeouts);
		return E_TMOUT;
	}

	wd_armed = TRUE;
	rc = stai_network_run(net, STAI_MODE_SYNC);
	wd_armed = FALSE;

	if(rc != STAI_SUCCESS) {
		tm_printf((UB*)"[npu] stai_network_run: rc=0x%x\n", (UW)rc);
		return E_IO;
	}
	return E_OK;
}

EXPORT UW npu_rt_timeouts(void)
{
	return timeouts;
}
