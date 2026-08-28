#ifndef AUDIO_WM8904_H
#define AUDIO_WM8904_H

#include <tk/tkernel.h>

/*
 * WM8904 オーディオ・コーデック (I2C2, PD14=SCL/PD4=SDA)
 *
 * レジスタアドレスと初期化シーケンスは、STのSTM32N6570-DK用BSP
 * (stm32-wm8904 / stm32n6570-dk-bsp リポジトリの wm8904.c,
 *  stm32n6570_discovery_audio.c) の WM8904_Init() を参照して
 * 移植したもの。データシートから独自に起こしたものではない。
 */
#define WM8904_I2C_ADDR			(0x1A)	// 7bitスレーブアドレス

/* レジスタアドレス (使用するもののみ) */
#define WM8904_REG_SW_RESET_ID		(0x00)	// SW Reset and ID (期待値 0x8904)
#define WM8904_REG_BIAS_CONTROL0	(0x04)
#define WM8904_REG_VMID_CONTROL0	(0x05)
#define WM8904_REG_MIC_BIAS_CONTROL0	(0x06)
#define WM8904_REG_ANALOG_ADC0		(0x0A)
#define WM8904_REG_PWR_MANAGEMENT0	(0x0C)
#define WM8904_REG_PWR_MANAGEMENT2	(0x0E)
#define WM8904_REG_PWR_MANAGEMENT6	(0x12)
#define WM8904_REG_CLOCK_RATES1	(0x15)
#define WM8904_REG_CLOCK_RATES2	(0x16)
#define WM8904_REG_AUDIO_INTERFACE0	(0x18)
#define WM8904_REG_AUDIO_INTERFACE1	(0x19)
#define WM8904_REG_DAC_DIGITAL1	(0x21)
#define WM8904_REG_ADC_DIGITAL_VOL_LEFT (0x24)
#define WM8904_REG_DIGITAL_MICROPHONE0	(0x27)
#define WM8904_REG_DRC0		(0x28)
#define WM8904_REG_ANALOG_OUTPUT1_LEFT	(0x39)
#define WM8904_REG_ANALOG_OUTPUT1_RIGHT (0x3A)
#define WM8904_REG_DC_SERVO0		(0x43)
#define WM8904_REG_DC_SERVO1		(0x44)
#define WM8904_REG_DC_SERVO_READBACK0	(0x4D)
#define WM8904_REG_ANALOG_HP0		(0x5A)
#define WM8904_REG_CHARGE_PUMP0	(0x62)
#define WM8904_REG_CLASS_W0		(0x68)
#define WM8904_REG_GPIO_CONTROL1	(0x79)

/* SW Reset and ID レジスタ(R0)を読み、Device IDを devid に格納する */
EXPORT ER wm8904_read_device_id(UH *devid);

/*
 * WM8904を初期化し、ヘッドホン出力を有効にする(サンプリングレート16kHz固定)。
 * DACはミュートしたままなので、この時点では音は出ない
 * (Play時にDAC_DIGITAL1のDAC_MUTEを解除する処理は別途必要)。
 * 各初期化ステップの成否をUARTに出力する。
 * volume_percent: 出力音量 0～100
 */
EXPORT ER wm8904_init_headphone_16k(UB volume_percent);

#endif	/* AUDIO_WM8904_H */
