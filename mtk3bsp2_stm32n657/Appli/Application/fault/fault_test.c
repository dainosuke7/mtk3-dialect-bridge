#include <tk/tkernel.h>
#include "main.h"	// CMSIS (NVIC_*)
#include "fault.h"
#include "fault_out.h"

/* ---------------------------------------------------------------- */
EXPORT void fault_test_busfault(void)
{
	volatile UW	v;

	fault_puts("\n[TEST] BusFault: read *(volatile uint32_t*)0xFFFFFFF0\n");
	v = *(volatile uint32_t*)0xFFFFFFF0;
	(void)v;

	/* 来ないはず */
	fault_puts("[TEST] !! no fault !!\n");
}

/* ---------------------------------------------------------------- */
EXPORT void fault_test_divzero(void)
{
	volatile int	z = 0;
	volatile int	r;

	fault_puts("\n[TEST] UsageFault: 1 / 0\n");
	r = 1 / z;
	(void)r;

	/* CCR.DIV_0_TRP が立っていなければ黙って r=0 で戻ってくる */
	fault_puts("[TEST] !! no fault (DIV_0_TRP not set?) !!\n");
}

/* ---------------------------------------------------------------- */
/*
 * 未使用IRQ。WWDG_IRQHandler は startup.s の weak alias のままで
 * Core/Src/stm32n6xx_it.c にも実体が無いことを確認済み
 * (ELF上も Default_Handler と同じアドレスに解決されている)。
 */
#define FAULT_TEST_IRQ	WWDG_IRQn	/* = 19 */

EXPORT void fault_test_unhandled_irq(void)
{
	fault_puts("\n[TEST] unhandled IRQ: pend WWDG_IRQn (19)\n");

	NVIC_ClearPendingIRQ(FAULT_TEST_IRQ);
	NVIC_EnableIRQ(FAULT_TEST_IRQ);
	NVIC_SetPendingIRQ(FAULT_TEST_IRQ);
	__DSB();
	__ISB();

	/* 来ないはず */
	fault_puts("[TEST] !! not taken !!\n");
}

/* ---------------------------------------------------------------- */
/*
 * STKOF テスト。
 *
 * このBSPの stm32_cube 構成では config_bsp.h に USE_SPMON が無いため、
 * dispatch.S のタスク毎 MSPLIM 設定がコンパイルされない。つまり MSPLIM は
 * sys_start.c が入れた INTERNAL_RAM_START (0x34000400) のまま固定で、
 * これは LRUN のコード領域の先頭でもある。
 * そのまま再帰させると自分のコードを潰しながら 500KB ほど下って
 * ようやく STKOF になり、途中で何が起きるか分からない。
 *
 * そこで「今のSPのすぐ下」に MSPLIM を一時的に上げてから再帰させる。
 * 発生する例外は本物の STKOF で、壊れる範囲は呼び出し元タスクの
 * スタック内に収まる。
 *
 * どこでリミットを踏むかによって、
 *   - タスク側の push で踏む    → UsageFault (exc=6) / STKOF
 *   - 例外スタッキングで踏む    → HardFault  (exc=3) / FORCED + STKOF
 * のどちらかになる。どちらも CFSR に STKOF が立つので判別できる。
 * (後者では stacked PC/LR/PSR が書けていないので当てにならない)
 *
 * Fault_Handler は入り口で MSPLIM を外すので、ハンドラ自身は
 * リミットより下に残っている本来のタスクスタックを使って出力できる。
 * そのため呼び出し元タスクには 2KB 以上のスタックが必要。
 */
#define STKOF_MARGIN	(512)	/* SP から何バイト下にリミットを置くか */

/* 無限再帰はこのテストの目的そのもの */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"
LOCAL UW recurse(UW n)
{
	volatile UW	pad[16];	/* 1段あたり 64バイト以上消費させる */

	pad[0] = n;
	return pad[0] + recurse(n + 1);
}
#pragma GCC diagnostic pop

EXPORT void fault_test_stkof(void)
{
	UW	sp;
	UW	limit;

	fault_puts("\n[TEST] UsageFault: stack overflow (deep recursion)\n");

	__asm volatile ("mrs %0, msp" : "=r" (sp));
	limit = (sp - STKOF_MARGIN) & ~7UL;

	fault_puts("  MSP=0x");
	fault_puthex(sp, 8);
	fault_puts(" -> MSPLIM=0x");
	fault_puthex(limit, 8);
	fault_puts("\n");

	__asm volatile ("msr msplim, %0" : : "r" (limit));
	__asm volatile ("isb");

	(void)recurse(0);

	/* 来ないはず */
	fault_puts("[TEST] !! no fault !!\n");
}

/* ---------------------------------------------------------------- */
/*
 * FAULT_TEST (fault.h) で選んだテストを1本だけ実行する。
 * usermain() から呼ばれる。FAULT_TEST==0 のときは何もせず戻る。
 */
EXPORT void fault_test_run(void)
{
#if   (FAULT_TEST == 1)
	fault_test_busfault();
#elif (FAULT_TEST == 2)
	fault_test_divzero();
#elif (FAULT_TEST == 3)
	fault_test_unhandled_irq();
#elif (FAULT_TEST == 4)
	fault_test_stkof();
#else
	/* テストなし */
#endif
}
