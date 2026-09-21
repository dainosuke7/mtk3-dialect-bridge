#ifndef EXTFLASH_EXTFLASH_H
#define EXTFLASH_EXTFLASH_H

#include <tk/tkernel.h>

/*
 * 外部 NOR フラッシュ (MX66UW1G45G, 1Gbit=128MB, XSPI2/Port2/NCS1)
 *
 * BSP2 の FSBL は EXTMEM(SFDP) 初期化がこのボードで失敗していて
 * (CLAUDE.md「既知の罠」)、外部フラッシュをメモリマップしないまま
 * アプリに跳んでくる。そこでアプリ側で XSPI2 をリセットして初期化し直し、
 * DTR-OPI のメモリマップドモードにする。成功すると EXTFLASH_BASE から
 * CPU/DMA/NPU が普通のメモリとして読める (AED の重みは 0x70180000〜)。
 * 書き込み・消去はしない (重みは CubeProgrammer で書く)。
 */
#define EXTFLASH_BASE		((UW)0x70000000)	/* XSPI2 のメモリマップ先頭 */

/*
 * 初期化してメモリマップする。各ステップの成否を UART に出す。
 * 戻り値: E_OK 成功 / E_IO 失敗 (どのステップかは UART の [FAIL] 行)
 *
 * - カーネル起動後にタスクから呼ぶ (待ちに tk_dly_tsk を使う)
 * - HAL のタイムアウトが効くよう、usermain の HAL ティック周期ハンドラを
 *   先に起動しておくこと (CLAUDE.md「既知の罠」)
 * - 失敗しても Error_Handler() は呼ばない。呼び出し側は戻り値を見て、
 *   失敗なら外部フラッシュを読まずに続行する
 */
EXPORT ER extflash_init(void);

/* メモリマップ済みか。外部フラッシュを読む前に確認する */
EXPORT BOOL extflash_mapped(void);

/*
 * 切り分け用: XSPI2 まわりのレジスタ (PWR VDDIO3 / RCC クロック・リセット /
 * XSPIM CR / XSPI2 CR,DCR,SR,校正 / GPION) を UART にダンプする。読むだけで
 * 副作用は無い。extflash_init() が初期化前・初期化後・失敗時に呼ぶ
 */
EXPORT void extflash_dump_regs(const char *when);

#endif	/* EXTFLASH_EXTFLASH_H */
