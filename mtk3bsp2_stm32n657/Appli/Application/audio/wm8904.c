#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "main.h"	// hi2c2 (I2C2, WM8904制御用。main.cのMX_I2C2_Init参照)
#include "wm8904.h"

#define WM8904_I2C_TIMEOUT	(100)	// ms

/* WM8904レジスタ(16bit, MSBファースト)への書き込み/読み出し */
LOCAL ER wm8904_write_reg(UB reg, UH val)
{
	HAL_StatusTypeDef	hal_sts;
	UB			data[2];

	data[0] = (UB)(val >> 8);
	data[1] = (UB)(val & 0xFFU);

	hal_sts = HAL_I2C_Mem_Write(&hi2c2, WM8904_I2C_ADDR << 1,
			reg, I2C_MEMADD_SIZE_8BIT, data, sizeof(data), WM8904_I2C_TIMEOUT);
	return (hal_sts == HAL_OK) ? E_OK : E_IO;
}

LOCAL ER wm8904_read_reg(UB reg, UH *val)
{
	HAL_StatusTypeDef	hal_sts;
	UB			data[2];

	hal_sts = HAL_I2C_Mem_Read(&hi2c2, WM8904_I2C_ADDR << 1,
			reg, I2C_MEMADD_SIZE_8BIT, data, sizeof(data), WM8904_I2C_TIMEOUT);
	if(hal_sts != HAL_OK) return E_IO;

	*val = ((UH)data[0] << 8) | data[1];
	return E_OK;
}

EXPORT ER wm8904_read_device_id(UH *devid)
{
	return wm8904_read_reg(WM8904_REG_SW_RESET_ID, devid);
}

/* 初期化1ステップの成否をUARTに出力する */
LOCAL ER wm8904_step(const UB *name, ER err)
{
	if(err == E_OK) {
		tm_printf((UB*)"  [ OK ] %s\n", name);
	} else {
		tm_printf((UB*)"  [FAIL] %s (err=%d)\n", name, err);
	}
	return err;
}

#define STEP(name, expr)	do { err = wm8904_step((const UB*)(name), (expr)); if(err < E_OK) return err; } while(0)

/*
 * ヘッドホン出力(16kHz)有効化までの初期化シーケンス。
 * ST製 STM32N6570-DK BSP (stm32-wm8904 / stm32n6570-dk-bsp の
 * WM8904_Init, InputDevice=WM8904_IN_NONE / OutputDevice=WM8904_OUT_HEADPHONE
 * 相当)のレジスタ書き込み順をそのまま踏襲している。
 * DAC_DIGITAL1のDAC_MUTEは1のままなので、この関数だけでは音は出ない。
 */
EXPORT ER wm8904_init_headphone_16k(UB volume_percent)
{
	ER	err;
	UB	vol;
	UH	tmp;
	UH	servo;
	UINT	iter;

	vol = (volume_percent >= 100U) ? 63U : (UB)((volume_percent / 2U) + 13U);

	tm_printf((UB*)"WM8904 init (headphone, 16kHz) start\n");

	STEP("SW Reset",                     wm8904_write_reg(WM8904_REG_SW_RESET_ID, 0x0000U));
	STEP("Clock: CLK_SYS_ENA",           wm8904_write_reg(WM8904_REG_CLOCK_RATES2, 0x0004U));
	STEP("Bias control (ISEL)",          wm8904_write_reg(WM8904_REG_BIAS_CONTROL0, 0x0018U));
	STEP("VMID control (enable)",        wm8904_write_reg(WM8904_REG_VMID_CONTROL0, 0x0047U));
	tk_dly_tsk(100);
	STEP("VMID control (settle)",        wm8904_write_reg(WM8904_REG_VMID_CONTROL0, 0x0043U));
	STEP("Bias control (BIAS_EN)",       wm8904_write_reg(WM8904_REG_BIAS_CONTROL0, 0x0019U));
	STEP("Mic bias enable",              wm8904_write_reg(WM8904_REG_MIC_BIAS_CONTROL0, 0x0001U));
	STEP("Power mgmt0 (input disable)",  wm8904_write_reg(WM8904_REG_PWR_MANAGEMENT0, 0x0000U));
	STEP("Power mgmt2 (HP PGA enable)",  wm8904_write_reg(WM8904_REG_PWR_MANAGEMENT2, 0x0003U));
	STEP("Clock: CLK_DSP_ENA",           wm8904_write_reg(WM8904_REG_CLOCK_RATES2, 0x0006U));
	STEP("Power mgmt6 (DAC/ADC enable)", wm8904_write_reg(WM8904_REG_PWR_MANAGEMENT6, 0x000EU));
	STEP("Digital mic disable",          wm8904_write_reg(WM8904_REG_DIGITAL_MICROPHONE0, 0x0000U));
	STEP("GPIO1 control (default)",      wm8904_write_reg(WM8904_REG_GPIO_CONTROL1, 0x0014U));
	STEP("DRC0 (disabled)",              wm8904_write_reg(WM8904_REG_DRC0, 0x01AFU));
	STEP("Analog ADC0 (OSR128)",         wm8904_write_reg(WM8904_REG_ANALOG_ADC0, 0x0001U));
	STEP("DAC digital1 (muted)",         wm8904_write_reg(WM8904_REG_DAC_DIGITAL1, 0x0648U));
	STEP("Audio interface0 (ADC src)",   wm8904_write_reg(WM8904_REG_AUDIO_INTERFACE0, 0x0010U));
	STEP("Audio interface1 (16b/I2S)",   wm8904_write_reg(WM8904_REG_AUDIO_INTERFACE1, 0x0002U));
	STEP("Clock rates1 (16kHz)",         wm8904_write_reg(WM8904_REG_CLOCK_RATES1, 0x0C02U));

	tmp = (UH)vol;
	STEP("HP volume (left)",             wm8904_write_reg(WM8904_REG_ANALOG_OUTPUT1_LEFT, tmp));
	tmp = (UH)(vol | 0x0080U);	/* Volume update */
	STEP("HP volume (right)",            wm8904_write_reg(WM8904_REG_ANALOG_OUTPUT1_RIGHT, tmp));

	STEP("ADC digital volume (muted)",   wm8904_write_reg(WM8904_REG_ADC_DIGITAL_VOL_LEFT, 0x0100U));
	STEP("Charge pump enable",           wm8904_write_reg(WM8904_REG_CHARGE_PUMP0, 0x0001U));
	STEP("Analog HP (enable)",           wm8904_write_reg(WM8904_REG_ANALOG_HP0, 0x0011U));
	STEP("Analog HP (enable delay)",     wm8904_write_reg(WM8904_REG_ANALOG_HP0, 0x0033U));
	STEP("DC servo (channel enable)",    wm8904_write_reg(WM8904_REG_DC_SERVO0, 0x0003U));
	STEP("DC servo (startup trigger)",   wm8904_write_reg(WM8904_REG_DC_SERVO1, 0x0030U));

	/* DCサーボの起動完了を待つ(最大30回x10ms) */
	servo = 0x0000U;
	err = E_OK;
	for(iter = 0; iter < 30U; iter++) {
		tk_dly_tsk(10);
		err = wm8904_read_reg(WM8904_REG_DC_SERVO_READBACK0, &servo);
		if(err < E_OK) break;
		if((servo & 0x0003U) == 0x0003U) break;
	}
	if(err == E_OK && (servo & 0x0003U) != 0x0003U) err = E_TMOUT;
	err = wm8904_step((const UB*)"DC servo (startup wait)", err);
	if(err < E_OK) return err;

	STEP("Analog HP (output enable)",    wm8904_write_reg(WM8904_REG_ANALOG_HP0, 0x0077U));
	STEP("Analog HP (remove short)",     wm8904_write_reg(WM8904_REG_ANALOG_HP0, 0x00FFU));
	STEP("Class W (dynamic power)",      wm8904_write_reg(WM8904_REG_CLASS_W0, 0x0001U));

	tm_printf((UB*)"WM8904 init (headphone, 16kHz) done\n");
	return E_OK;
}

EXPORT ER wm8904_dac_unmute(void)
{
	return wm8904_write_reg(WM8904_REG_DAC_DIGITAL1, 0x0640U);	/* DAC_MUTE = 0 */
}
