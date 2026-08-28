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

#endif	/* AUDIO_SAI_IO_H */
