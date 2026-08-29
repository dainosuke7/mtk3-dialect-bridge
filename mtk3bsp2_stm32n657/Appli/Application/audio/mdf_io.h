#ifndef AUDIO_MDF_IO_H
#define AUDIO_MDF_IO_H

#include <tk/tkernel.h>

/*
 * MDF1 (オンボードPDM MEMSマイク U13/U14) 入力。
 *
 * ペリフェラルのクロック(PLL3->IC8)/GPIO(PE2=CCK0, PE8=DATIN0)/
 * HAL_MDF_Init自体はmain.cのMX_MDF1_Init()で行っている(理由はmain.hの
 * g_mdf1_statusコメント、main.cのMX_MDF1_Init()コメントを参照)。
 *
 * 現段階ではここは初期化結果の確認のみ。フィルタ/チャンネル設定・
 * DMA・データ取得は次のステップ。
 */

/* MX_MDF1_Init()(main.c)の結果を確認する */
EXPORT ER mdf_in_init_check(void);

/* 失敗時の切り分け用。main.cのg_mdf1_step/g_mdf1_statusをそのまま返す。
 * step: 0=未実行, 1=RCC_OscConfig(PLL3), 2=RCCEx_PeriphCLKConfig(IC8),
 *       3=GPIO設定, 4=HAL_MDF_Init, 5=全ステップ完了
 * hal_status: 失敗したステップでのHAL_StatusTypeDef(全ステップ完了時はHAL_OK) */
EXPORT UW mdf_in_init_step(void);
EXPORT UW mdf_in_init_hal_status(void);

#endif	/* AUDIO_MDF_IO_H */
