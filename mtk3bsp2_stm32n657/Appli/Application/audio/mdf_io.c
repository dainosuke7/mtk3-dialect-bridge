#include <tk/tkernel.h>
#include "main.h"	// hmdf1, g_mdf1_status
#include "mdf_io.h"

EXPORT ER mdf_in_init_check(void)
{
	return (g_mdf1_status == HAL_OK) ? E_OK : E_SYS;
}

EXPORT UW mdf_in_init_step(void)
{
	return g_mdf1_step;
}

EXPORT UW mdf_in_init_hal_status(void)
{
	return (UW)g_mdf1_status;
}
