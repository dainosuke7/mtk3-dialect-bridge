#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "audio/audio_task.h"
#include "fault/fault.h"
#include "trace/trace.h"

#define TRACE_WINDOW_MS		(10000)	/* 記録する区間の長さ */
#define TRACE_WAIT_POLL		(100)	/* パススルー待ちの上限 (x100ms) */

LOCAL void task_1(INT stacd, void *exinf);	// task execution function
LOCAL ID	tskid_1;			// Task ID number
LOCAL T_CTSK ctsk_1 = {				// Task creation information
	.itskpri	= 10,
	.stksz		= 1024,
	.task		= task_1,
	.tskatr		= TA_HLNG | TA_RNG3,
};

LOCAL void task_2(INT stacd, void *exinf);	// task execution function
LOCAL ID	tskid_2;			// Task ID number
LOCAL T_CTSK ctsk_2 = {				// Task creation information
	.itskpri	= 10,
	.stksz		= 1024,
	.task		= task_2,
	.tskatr		= TA_HLNG | TA_RNG3,
};

LOCAL void task_1(INT stacd, void *exinf)
{
	while(1) {
		if(!trace_muted()) tm_printf((UB*)"task 1\n");

		/* Inverts the LED on the board. */
		out_w(GPIO_ODR(O), (in_w(GPIO_ODR(O)))^(1<<1));

		tk_dly_tsk(500);
	}
}

LOCAL void task_2(INT stacd, void *exinf)
{
	while(1) {
		if(!trace_muted()) tm_printf((UB*)"task 2\n");

		/* Inverts the LED on the board. */
		out_w(GPIO_ODR(G), (in_w(GPIO_ODR(G)))^(1<<10));

		tk_dly_tsk(700);
	}
}

/* usermain関数 */
EXPORT INT usermain(void)
{
	INT	i;

	/* 何よりも先に。以降のフォルト・未実装IRQはUARTに出てから止まる */
	app_fault_init();

	/* DWT CYCCNT を起こす。以降の計測はすべてこれが基準 */
	trace_init();

	tm_putstring((UB*)"Start User-main program.\n");

	/* 受け入れテスト (fault.h の FAULT_TEST)。1〜4なら戻ってこない */
	fault_test_run();

	/* Create & Start Tasks */
	tskid_1 = tk_cre_tsk(&ctsk_1);
	tk_sta_tsk(tskid_1, 0);

	tskid_2 = tk_cre_tsk(&ctsk_2);
	tk_sta_tsk(tskid_2, 0);

	/* 計測タスク(レポータ/ダンプ)と1秒周期ハンドラを用意する。
	 * audio 側が trace_rate_start() を呼ぶので、その前に作っておく */
	if(trace_task_start() < E_OK) {
		tm_printf((UB*)"trace_task_start FAILED\n");
	}

	audio_task_start();

	/* ---- 計測したい区間はここ ----
	 * パススルーが立ち上がるのを待ってから10秒間だけ記録する。
	 * 記録は時間経過かリング満杯で自動停止し、そのままCSVダンプに移る。 */
	for(i = 0; i < TRACE_WAIT_POLL; i++) {
		if(audio_passthrough_active()) break;
		tk_dly_tsk(100);
	}
	if(audio_passthrough_active()) {
		trace_start(TRACE_WINDOW_MS);
	} else {
		tm_printf((UB*)"trace: passthrough did not start, skip\n");
	}

	tk_slp_tsk(TMO_FEVR);

	return 0;
}
