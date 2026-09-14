#include <tk/tkernel.h>
#include "main.h"	// hmdf1, g_mdf1_status, g_mdf1_step
#include "mdf_io.h"

/*
 * DMA取り込みバッファ。MDFのDMAが書き、CPUが読む。
 * main.cのlinked-listノード(MdfRxNode/MdfRxQueue)と違い、こちらは
 * .noncacheableには置かず通常のキャッシュ可能SRAMに置いている:
 * このバッファは「DMAが書く→CPUが読む」の順序をコールバック側で
 * 完全に制御できるので、読む直前にSCB_InvalidateDCache_by_Addr()を
 * 呼べば足りるため(SAI出力側のdma_bufと同じ考え方)。
 * InvalidateDCacheは32バイト境界・32バイト単位で効くので、
 * 32バイトアラインし、半分のバイト数(256*4=1024)も32の倍数にしてある。
 */
LOCAL W mdf_buf[MDF_IN_TOTAL_SAMPLES] __attribute__((aligned(32)));

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

EXPORT W *mdf_in_buf_half(UINT half)
{
	return (half == 0) ? &mdf_buf[0] : &mdf_buf[MDF_IN_HALF_SAMPLES];
}

EXPORT ER mdf_in_start_dma(void)
{
	HAL_StatusTypeDef	hal_sts;
	MDF_FilterConfigTypeDef	filter_cfg = {0};
	MDF_DmaConfigTypeDef	dma_cfg;

	/*
	 * フィルタ設定。ST公式STM32N6570-DK BSP
	 * (stm32n6570-dk-bsp, stm32n6570_discovery_audio.c の
	 *  MX_MDF1_Init() 内 Audio_MdfFilterConfig と、16kHz用の
	 *  MDF_CIC_MODE/MDF_DECIMATION_RATIO/MDF_GAIN マクロ値)からの移植。
	 * 16kHz: CicMode=SINC4, DecimationRatio=32, Gain=2。
	 */
	filter_cfg.DataSource                    = MDF_DATA_SOURCE_BSMX;
	filter_cfg.Delay                         = 0U;
	filter_cfg.CicMode                       = MDF_ONE_FILTER_SINC4;
	filter_cfg.DecimationRatio               = 32U;
	filter_cfg.Offset                        = 0;
	filter_cfg.Gain                          = 2;
	filter_cfg.ReshapeFilter.Activation      = ENABLE;
	filter_cfg.ReshapeFilter.DecimationRatio = MDF_RSF_DECIMATION_RATIO_4;
	filter_cfg.HighPassFilter.Activation     = ENABLE;
	filter_cfg.HighPassFilter.CutOffFrequency = MDF_HPF_CUTOFF_0_000625FPCM;
	filter_cfg.Integrator.Activation         = DISABLE;
	filter_cfg.AcquisitionMode               = MDF_MODE_ASYNC_CONT;
	filter_cfg.FifoThreshold                 = MDF_FIFO_THRESHOLD_NOT_EMPTY;
	filter_cfg.DiscardSamples                = 0U;

	/* DataLengthはバイト数(GPDMAのブロックサイズCBR1にそのまま入る) */
	dma_cfg.Address    = (uint32_t)mdf_buf;
	dma_cfg.DataLength = sizeof(mdf_buf);
	dma_cfg.MsbOnly    = DISABLE;

	hal_sts = HAL_MDF_AcqStart_DMA(&hmdf1, &filter_cfg, &dma_cfg);
	if(hal_sts != HAL_OK) return E_IO;

	/* 飽和/overrun割り込みはHALが勝手に有効化する。今は通知が不要で、
	 * 大音量時に多発するだけなのでマスクする(飽和はmin/maxで分かる) */
	hmdf1.Instance->DFLTIER &= ~(MDF_DFLTIER_SATIE | MDF_DFLTIER_RFOVRIE);

	return E_OK;
}

EXPORT ER mdf_in_stop_dma(void)
{
	HAL_StatusTypeDef	hal_sts;

	hal_sts = HAL_MDF_AcqStop_DMA(&hmdf1);
	return (hal_sts == HAL_OK) ? E_OK : E_IO;
}
