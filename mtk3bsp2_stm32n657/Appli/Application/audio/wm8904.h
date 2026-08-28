#ifndef AUDIO_WM8904_H
#define AUDIO_WM8904_H

#include <tk/tkernel.h>

/* WM8904 オーディオ・コーデック (I2C2) */
#define WM8904_I2C_ADDR		(0x1A)	// 7bitスレーブアドレス
#define WM8904_REG_SW_RESET_ID	(0x00)	// SW Reset and ID レジスタ (期待値 0x8904)

/* SW Reset and ID レジスタ(R0)を読み、Device IDを devid に格納する */
EXPORT ER wm8904_read_device_id(UH *devid);

#endif	/* AUDIO_WM8904_H */
