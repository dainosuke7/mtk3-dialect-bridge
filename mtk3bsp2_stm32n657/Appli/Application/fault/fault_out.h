#ifndef FAULT_OUT_H
#define FAULT_OUT_H

#include <tk/tkernel.h>

/*
 * フォルト経路専用の文字出力。
 * libtm が使っている USART を同じ条件でそのまま直接叩く。

 */

/* 1文字送信 (TXEポーリング)。UART無反応時は一定回数で諦める */
EXPORT void fault_putc(char c);

/* NUL終端文字列を送信する */
EXPORT void fault_puts(const char *s);

/* val を digits 桁のゼロ埋め16進で送信する (0xは付けない) */
EXPORT void fault_puthex(UW val, INT digits);

/* val を10進で送信する */
EXPORT void fault_putdec(UW val);

/*
 * 送信完了(TC)を待ってから停止する。
 * デバッガ接続中(DHCSR.C_DEBUGEN)なら __BKPT(0) でブレークし、
 * そうでなければ割り込みを止めて無限ループする。
 */
EXPORT void fault_halt(void);

#endif	/* FAULT_OUT_H */
