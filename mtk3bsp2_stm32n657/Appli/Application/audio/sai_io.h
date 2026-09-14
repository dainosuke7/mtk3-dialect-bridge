#ifndef AUDIO_SAI_IO_H
#define AUDIO_SAI_IO_H

#include <tk/tkernel.h>

/*
 * SAI1_Block_A 送受信 (ブロッキング版)
 *
 * ペリフェラルのクロック(PLL2->IC7)/GPIO(PB0,PB6,PB7,PG7)/HAL_SAI_Init
 * 自体は main.c の MX_SAI1_Init() で行っている(理由は main.h の
 * g_sai1_status コメント、main.c の MX_SAI1_Init() コメントを参照)。
 * ここではそれが成功しているかどうかの確認と、実際の送信のみを行う。
 */

/* MX_SAI1_Init()(main.c)の結果を確認する */
EXPORT ER sai_out_init_check(void);

/* samples (16bit, L/Rインターリーブ) を count個、ブロッキングで送信する */
EXPORT ER sai_out_transmit(const H *samples, UINT count);

/*
 * DMA版 (ダブルバッファ連続再生)。
 * samplesはダブルバッファ全体(前半+後半)へのポインタ、countはその
 * 合計サンプル数。GPDMAが循環リンクリストで自動的に折り返し続けるため、
 * 呼び出しは1回だけでよい。次データの充填はHAL_SAI_TxHalfCpltCallback/
 * HAL_SAI_TxCpltCallback(呼び出し側で実装)で行うこと。
 * SAI1本体とTx DMAチャネル(GPDMA1 Channel2)自体はmain.cのMX_SAI1_Init()
 * で初期化済み。
 */
EXPORT ER sai_out_transmit_dma(const H *samples, UINT count);

/* DMAでの連続再生を停止する */
EXPORT ER sai_out_stop_dma(void);

/* 実際の出力サンプリングレート確認用(main.h の g_sai1_kerclk /
 * g_sai1_mckdiv をそのまま返す)。Fs = kerclk / (mckdiv * 256) */
EXPORT UW sai_out_kernel_clock(void);
EXPORT UW sai_out_mckdiv(void);

#endif	/* AUDIO_SAI_IO_H */
