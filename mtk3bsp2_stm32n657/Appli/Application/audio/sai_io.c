#include <tk/tkernel.h>
#include "main.h"	// hsai1, g_sai1_status
#include "sai_io.h"

#define SAI_TX_TIMEOUT	(100)	// ms

EXPORT ER sai_out_init_check(void)
{
	return (g_sai1_status == HAL_OK) ? E_OK : E_SYS;
}

EXPORT ER sai_out_transmit(const H *samples, UINT count)
{
	HAL_StatusTypeDef	hal_sts;

	hal_sts = HAL_SAI_Transmit(&hsai1, (UB*)samples, (UH)count, SAI_TX_TIMEOUT);
	return (hal_sts == HAL_OK) ? E_OK : E_IO;
}
