#ifndef AUDIO_MDF_IO_H
#define AUDIO_MDF_IO_H

#include <tk/tkernel.h>

/*
 * MDF1 (オンボードPDM MEMSマイク U13/U14) 入力。
 *
 * ペリフェラルのクロック(PLL3->IC8)/GPIO(PE2=CCK0, PE8=DATIN0)/
 * HAL_MDF_Init/Rx DMAチャネル(GPDMA1 Channel0)自体はmain.cの
 * MX_MDF1_Init()で行っている(理由はmain.hのg_mdf1_statusコメント、
 * main.cのMX_MDF1_Init()コメントを参照)。
 * ここではフィルタ設定と取得の開始/停止、およびバッファの提供を行う。
 */

/*
 * DMA取り込みバッファ(ダブルバッファ)。SAI出力側と同じ構成で、
 * 前半/後半それぞれMDF_IN_HALF_SAMPLES個。MDFの出力は32bit
 * (上位24bitに有効データ)なので W(int32) 配列で受ける。
 * 16kHzなので 256サンプル = 16ms ぶん、コールバックは約62.5Hz。
 */
#define MDF_IN_HALF_SAMPLES	(256)
#define MDF_IN_TOTAL_SAMPLES	(MDF_IN_HALF_SAMPLES * 2)

/* MX_MDF1_Init()(main.c)の結果を確認する */
EXPORT ER mdf_in_init_check(void);

/* 失敗時の切り分け用。main.cのg_mdf1_step/g_mdf1_statusをそのまま返す。
 * step: 0=未実行, 1=RCC_OscConfig(PLL3), 2=RCCEx_PeriphCLKConfig(IC8),
 *       3=GPIO設定, 4=HAL_MDF_Init, 5=Rx DMA設定, 6=全ステップ完了
 * hal_status: 失敗したステップでのHAL_StatusTypeDef(全ステップ完了時はHAL_OK) */
EXPORT UW mdf_in_init_step(void);
EXPORT UW mdf_in_init_hal_status(void);

/* 取り込みバッファの先頭。half=0で前半、half=1で後半の先頭を返す */
EXPORT W *mdf_in_buf_half(UINT half);

/* DMAでの連続取り込みを開始/停止する */
EXPORT ER mdf_in_start_dma(void);
EXPORT ER mdf_in_stop_dma(void);

#endif	/* AUDIO_MDF_IO_H */
