#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "main.h"	// CMSIS (DWT, DCB, SystemCoreClock, __DSB)
#include "trace.h"

/*
 * core_cm55.h の DWT_Type には LAR が無い (0xFB0 は RESERVED14[968] の中)。
 * Cortex-M55 でソフトウェアロック解除が必要かどうかは未検証なので、
 * 「まず解除なしで試し、駄目なら LAR を叩いて再試行」の二段構えにする。
 */
#define DWT_LAR		(*(volatile uint32_t*)(DWT_BASE + 0xFB0UL))
#define DWT_LAR_KEY	(0xC5ACCE55UL)

/* LAR解除の要否 */
#define LAR_NOT_NEEDED	(0)	/* 解除なしで動いた */
#define LAR_REQUIRED	(1)	/* 解除して初めて動いた */
#define LAR_NA		(2)	/* どちらでも動かなかった */

LOCAL BOOL	cyccnt_ok;
LOCAL UINT	lar_state;
LOCAL UW	core_clock;
LOCAL UW	cyc_per_us;

/*
 * CYCCNT が進むかどうかを短いループで確かめる。
 * volatile なカウンタを回すので -O0 でも最適化で消えない。
 */
LOCAL BOOL cyccnt_running(void)
{
	UW		t0, t1;
	volatile UINT	i;

	t0 = NOW();
	for(i = 0; i < 100; i++) { }
	t1 = NOW();

	return ((UW)(t1 - t0) != 0) ? TRUE : FALSE;
}

LOCAL void cyccnt_enable(void)
{
	DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
	__DSB();
	DWT->CYCCNT = 0;
	DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
	__DSB();
}

EXPORT void trace_init(void)
{
	cyccnt_ok = FALSE;
	lar_state = LAR_NA;

	/* SystemCoreClock は推測せず実行時の値を読む */
	core_clock = (UW)SystemCoreClock;
	cyc_per_us = core_clock / 1000000U;
	if(cyc_per_us == 0) cyc_per_us = 1;	/* 0除算よけ */

	/* NOCYCCNT が立っていたら CYCCNT の実装そのものが無い */
	if((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0) {
		tm_printf((UB*)"[TRACE] DWT CYCCNT NOT IMPLEMENTED (CTRL.NOCYCCNT=1)\n");
		tm_printf((UB*)"[TRACE] time measurement disabled\n");
		return;
	}

	cyccnt_enable();
	if(cyccnt_running()) {
		cyccnt_ok = TRUE;
		lar_state = LAR_NOT_NEEDED;
	} else {
		/* ソフトウェアロックが効いている可能性。解除して再試行 */
		DWT_LAR = DWT_LAR_KEY;
		__DSB();
		cyccnt_enable();
		if(cyccnt_running()) {
			cyccnt_ok = TRUE;
			lar_state = LAR_REQUIRED;
		}
	}

	tm_printf((UB*)"[TRACE] CYCCNT=%s SystemCoreClock=%uHz (%u cyc/us) LAR_unlock=%s\n",
			cyccnt_ok ? (UB*)"OK" : (UB*)"FAIL",
			core_clock, cyc_per_us,
			(lar_state == LAR_NOT_NEEDED) ? (UB*)"not-required"
			: (lar_state == LAR_REQUIRED) ? (UB*)"required"
			:                               (UB*)"n/a");
}

EXPORT BOOL trace_cyccnt_valid(void)	{ return cyccnt_ok; }
EXPORT UW   trace_core_clock(void)	{ return core_clock; }
EXPORT UW   trace_cyc_per_us(void)	{ return cyc_per_us; }

/*
 * サイクル -> マイクロ秒。
 * cyc は最大 4.29e9、x1e6 で 4.29e15 なので 64bit に収まる。
 * cyc_per_us で割ると SystemCoreClock が 1MHz の倍数でないときに
 * 誤差が乗るため、core_clock でそのまま割る。
 */
EXPORT UW trace_cyc_to_us(UW cyc)
{
	if(core_clock == 0) return 0;
	return (UW)(((uint64_t)cyc * 1000000ULL) / (uint64_t)core_clock);
}
