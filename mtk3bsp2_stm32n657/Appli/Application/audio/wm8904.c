#include <tk/tkernel.h>
#include "main.h"	// hi2c2 (I2C2, WM8904制御用。main.cのMX_I2C2_Init参照)
#include "wm8904.h"

EXPORT ER wm8904_read_device_id(UH *devid)
{
	HAL_StatusTypeDef	hal_sts;
	UB			data[2];	// レジスタ読み出しデータ (MSB first)

	hal_sts = HAL_I2C_Mem_Read(&hi2c2, WM8904_I2C_ADDR << 1,
			WM8904_REG_SW_RESET_ID, I2C_MEMADD_SIZE_8BIT,
			data, sizeof(data), 100);
	if(hal_sts != HAL_OK) return E_IO;

	*devid = ((UH)data[0] << 8) | data[1];
	return E_OK;
}
