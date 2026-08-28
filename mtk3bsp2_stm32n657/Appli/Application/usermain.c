#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "main.h"	// hi2c2 (I2C2, WM8904 control interface; see main.c MX_I2C2_Init)

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

LOCAL void task_3(INT stacd, void *exinf);	// task execution function
LOCAL ID	tskid_3;			// Task ID number
LOCAL T_CTSK ctsk_3 = {				// Task creation information
	.itskpri	= 10,
	.stksz		= 1024,
	.task		= task_3,
	.tskatr		= TA_HLNG | TA_RNG3,
};

LOCAL void task_1(INT stacd, void *exinf)
{
	while(1) {
		tm_printf((UB*)"task 1\n");

		/* Inverts the LED on the board. */
		out_w(GPIO_ODR(O), (in_w(GPIO_ODR(O)))^(1<<1));

		tk_dly_tsk(500);
	}
}

LOCAL void task_2(INT stacd, void *exinf)
{
	while(1) {
		tm_printf((UB*)"task 2\n");

		/* Inverts the LED on the board. */
		out_w(GPIO_ODR(G), (in_w(GPIO_ODR(G)))^(1<<10));

		tk_dly_tsk(700);
	}
}

/* WM8904 オーディオ・コーデック (I2C2) */
#define WM8904_I2C_ADDR		(0x1A)	// 7bitスレーブアドレス
#define WM8904_REG_SW_RESET_ID	(0x00)	// SW Reset and ID レジスタ (期待値 0x8904)

LOCAL void task_3(INT stacd, void *exinf)	// task execution function
{
	HAL_StatusTypeDef	hal_sts;
	UB			data[2];	// レジスタ読み出しデータ (MSB first)
	UH			devid;

	hal_sts = HAL_I2C_Mem_Read(&hi2c2, WM8904_I2C_ADDR << 1,
			WM8904_REG_SW_RESET_ID, I2C_MEMADD_SIZE_8BIT,
			data, sizeof(data), 100);
	if(hal_sts != HAL_OK) {
		tm_printf((UB*)"WM8904 I2C2 read error = %d\n", hal_sts);
	} else {
		devid = ((UH)data[0] << 8) | data[1];
		tm_printf((UB*)"WM8904 Device ID = 0x%04X\n", devid);
	}

	tk_ext_tsk();
}

/* usermain関数 */
EXPORT INT usermain(void)
{
	tm_putstring((UB*)"Start User-main program.\n");

	/* Create & Start Tasks */
	tskid_1 = tk_cre_tsk(&ctsk_1);
	tk_sta_tsk(tskid_1, 0);

	tskid_2 = tk_cre_tsk(&ctsk_2);
	tk_sta_tsk(tskid_2, 0);

	tskid_3 = tk_cre_tsk(&ctsk_3);
	tk_sta_tsk(tskid_3, 0);

	tk_slp_tsk(TMO_FEVR);

	return 0;
}
