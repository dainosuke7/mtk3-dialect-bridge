#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "wm8904.h"
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
#define WM8904_DEFAULT_VOLUME	(70U)	// 0-100

LOCAL void task_audio(INT stacd, void *exinf)
{
	ER	err;
	UH	devid;

	err = wm8904_read_device_id(&devid);
	if(err < E_OK) {
		tm_printf((UB*)"WM8904 I2C2 read error = %d\n", err);
	} else {
		tm_printf((UB*)"WM8904 Device ID = 0x%04X\n", devid);
		if(devid != WM8904_EXPECT_ID) {
			tm_printf((UB*)"WM8904 unexpected Device ID (expect 0x%04X)\n", WM8904_EXPECT_ID);
		} else {
			err = wm8904_init_headphone_16k(WM8904_DEFAULT_VOLUME);
			if(err < E_OK) {
				tm_printf((UB*)"WM8904 init failed (err=%d)\n", err);
			}
		}
	}

	tk_ext_tsk();
}

EXPORT void audio_task_start(void)
{
	tskid_audio = tk_cre_tsk(&ctsk_audio);
	tk_sta_tsk(tskid_audio, 0);
}
