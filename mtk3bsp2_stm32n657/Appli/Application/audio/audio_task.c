#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <math.h>
#include "wm8904.h"
#include "sai_io.h"
#include "audio_task.h"

LOCAL void task_audio(INT stacd, void *exinf);	// task execution function
LOCAL ID	tskid_audio;			// Task ID number
LOCAL T_CTSK ctsk_audio = {			// Task creation information
	.itskpri	= 10,
	.stksz		= 1024,
	.task		= task_audio,
	.tskatr		= TA_HLNG | TA_RNG3,
};

#define WM8904_EXPECT_ID	(0x8904U)
/* TODO: 動作確認用に音量/再生時間を一時的に上げている。確認できたら
 * WM8904_TEST_VOLUME=20, SINE_PLAY_LOOPS=40 (約1秒) に戻すこと。 */
#define WM8904_TEST_VOLUME	(70U)	// 0-100

/* 440Hz サイン波テーブル。16000Hz / 400サンプルで11周期 = ちょうど440Hz
 * (16000*11/400 = 440) になるよう選んだサンプル数なので、末尾から先頭に
 * 戻っても位相が繋がりループ再生できる。 */
#define SINE_SAMPLE_RATE	(16000)
#define SINE_TABLE_LEN		(400)
#define SINE_CYCLES		(11)
#define SINE_AMPLITUDE		(8192)	// 16bitフルスケールの約1/4(安全のため控えめ)
#define SINE_PLAY_LOOPS		(400)	// SINE_TABLE_LEN(25ms分)を400回 = 約10秒

LOCAL H sine_table[SINE_TABLE_LEN * 2];	// L/Rインターリーブ

LOCAL void sine_table_build(void)
{
	UINT	i;
	float	theta;
	H	v;

	for(i = 0; i < SINE_TABLE_LEN; i++) {
		theta = 2.0f * 3.14159265f * SINE_CYCLES * (float)i / (float)SINE_TABLE_LEN;
		v = (H)(SINE_AMPLITUDE * sinf(theta));
		sine_table[2*i]     = v;	// L
		sine_table[2*i + 1] = v;	// R
	}
}

LOCAL void task_audio(INT stacd, void *exinf)
{
	ER	err;
	UH	devid;
	UINT	loop;

	do {
		err = wm8904_read_device_id(&devid);
		if(err < E_OK) {
			tm_printf((UB*)"WM8904 I2C2 read error = %d\n", err);
			break;
		}
		tm_printf((UB*)"WM8904 Device ID = 0x%04X\n", devid);
		if(devid != WM8904_EXPECT_ID) {
			tm_printf((UB*)"WM8904 unexpected Device ID (expect 0x%04X)\n", WM8904_EXPECT_ID);
			break;
		}

		err = wm8904_init_headphone_16k(WM8904_TEST_VOLUME);
		if(err < E_OK) {
			tm_printf((UB*)"WM8904 init failed (err=%d)\n", err);
			break;
		}

		/* SAI1本体(クロック/GPIO/HAL_SAI_Init)はmain.cのMX_SAI1_Init()で
		 * 初期化済み。ここでは結果を確認するだけ。 */
		err = sai_out_init_check();
		if(err < E_OK) {
			tm_printf((UB*)"SAI1 peripheral init FAIL (see main.c MX_SAI1_Init)\n");
			break;
		}
		tm_printf((UB*)"SAI1 peripheral init OK\n");

		sine_table_build();
		tm_printf((UB*)"Sine table build OK (440Hz, 16kHz, amp=%d/32768)\n", SINE_AMPLITUDE);

		err = wm8904_dac_unmute();
		if(err < E_OK) {
			tm_printf((UB*)"WM8904 DAC unmute FAIL (err=%d)\n", err);
			break;
		}
		tm_printf((UB*)"WM8904 DAC unmute OK\n");

		tm_printf((UB*)"Sine wave playback start (440Hz, ~10s, blocking)\n");
		for(loop = 0; loop < SINE_PLAY_LOOPS; loop++) {
			err = sai_out_transmit(sine_table, SINE_TABLE_LEN * 2);
			if(err < E_OK) break;
		}
		if(err < E_OK) {
			tm_printf((UB*)"Sine wave playback FAIL (err=%d, loop=%d)\n", err, loop);
		} else {
			tm_printf((UB*)"Sine wave playback done\n");
		}
	} while(0);

	tk_ext_tsk();
}

EXPORT void audio_task_start(void)
{
	tskid_audio = tk_cre_tsk(&ctsk_audio);
	tk_sta_tsk(tskid_audio, 0);
}
