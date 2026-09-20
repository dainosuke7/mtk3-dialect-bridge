#include <tk/tkernel.h>
#include "main.h"	// CMSIS (SCB, SCB_CleanDCache_by_Addr, __DSB, __ISB)
#include "fault.h"
#include "fault_out.h"

/*
 * ベクタ数の根拠:
 *   Core/Startup/startup_stm32n657x0hxq.s の g_pfnVectors は .word が
 *   211個 (0:_estack, 1..15:システム例外, 16..210:IRQ0..194)。
 *   IRQn_Type の最大は LTDC_UP_ERR_IRQn = 194 (stm32n657xx.h) なので
 *   211 で全部そろっている。
 * アラインメントの根拠:
 *   VTOR はテーブルサイズ以上の2の冪境界を要求する。211 を2の冪に
 *   切り上げて 256エントリ、その4倍 = 1024バイト境界。
 *   (カーネルの knl_exctbl も EXCTBL_ALIGN=1024 で同じ)
 */
#define APP_VECT_DEFINED	(211)
#define APP_VECT_ENTRIES	(256)
#define APP_VECT_ALIGN		(APP_VECT_ENTRIES * 4)

LOCAL UW	app_vector[APP_VECT_ENTRIES] __attribute__((aligned(APP_VECT_ALIGN)));

/*
 * Default_Handler は startup.s で .global されている(31行目)ので直接extern
 * できる。ただしラベルに .type %function / .thumb_func が付いておらず、
 * ELF上は NOTYPE (値 0x…3c) になる。一方テーブルに載っているのは
 * .thumb_set 経由の別名 (値 0x…3d) で、Thumbビットの有無が食い違う。
 * そのため比較は必ず bit0 を落としてから行う。
 */
IMPORT void	Default_Handler(void);
IMPORT void	GenericUnhandled_Handler(void);
IMPORT void	Fault_Handler(void);

#define ADDR_OF(f)	((UW)(void*)&(f))
#define NO_THUMB(a)	((a) & ~1UL)

/* 差し替える例外番号 (3:HardFault 〜 7:SecureFault) */
#define FAULT_EXC_FIRST		(3)
#define FAULT_EXC_LAST		(7)

EXPORT void app_fault_init(void)
{
	const UW	*src;
	UW		old_vtor;
	UW		def_addr;
	INT		i;
	INT		n_irq = 0;
	INT		n_exc = 0;

	/*
	 * 現在のVTORからコピーする。この時点では knl_start_mtkernel() が
	 * すでに knl_exctbl[] へ張り替えているので、そのRAM上の内容
	 * (= カーネルのSVC/PendSV/SysTickハンドラ入り) が複製される。
	 */
	old_vtor = SCB->VTOR;
	src      = (const UW*)old_vtor;

	for(i = 0; i < APP_VECT_DEFINED; i++) {
		app_vector[i] = src[i];
	}
	/* 211..255 は実在しない例外。コピー元も無いので0で埋める */
	for(; i < APP_VECT_ENTRIES; i++) {
		app_vector[i] = 0;
	}

	/* 例外16以降で Default_Handler のままのものを可視化ハンドラへ */
	def_addr = NO_THUMB(ADDR_OF(Default_Handler));
	for(i = 16; i < APP_VECT_DEFINED; i++) {
		if(NO_THUMB(app_vector[i]) == def_addr) {
			app_vector[i] = ADDR_OF(GenericUnhandled_Handler);
			n_irq++;
		}
	}

	/* HardFault/MemManage/BusFault/UsageFault/SecureFault */
	for(i = FAULT_EXC_FIRST; i <= FAULT_EXC_LAST; i++) {
		app_vector[i] = ADDR_OF(Fault_Handler);
		n_exc++;
	}
	/* 11(SVC) / 14(PendSV) / 15(SysTick) はカーネルのもの。触らない */

	/*
	 * テーブルはD-cacheに載っているのでクリーンしてから張り替える。
	 * 順序: CleanDCache -> DSB -> VTOR -> DSB -> ISB
	 */
	SCB_CleanDCache_by_Addr((uint32_t*)app_vector, (int32_t)sizeof(app_vector));
	__DSB();
	SCB->VTOR = (UW)(void*)app_vector;
	__DSB();
	__ISB();

	/*
	 * 個別フォルトを有効化する。カーネルは SHCSR に
	 * USGFAULTENA|BUSFAULTENA|MEMFAULTENA を「代入」しており
	 * SECUREFAULTENA が落ちているので、読み出してORで足す。
	 */
	SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk
		    | SCB_SHCSR_BUSFAULTENA_Msk
		    | SCB_SHCSR_USGFAULTENA_Msk
		    | SCB_SHCSR_SECUREFAULTENA_Msk;

	/* 0除算をUsageFaultにする (既定では無視されて結果0になる) */
	SCB->CCR |= SCB_CCR_DIV_0_TRP_Msk;

	__DSB();
	__ISB();

	fault_puts("\n[FAULT] vector table installed\n");
	fault_puts("  VTOR     : 0x");
	fault_puthex(old_vtor, 8);
	fault_puts(" -> 0x");
	fault_puthex((UW)(void*)app_vector, 8);
	fault_puts("\n  entries  : ");
	fault_putdec((UW)APP_VECT_ENTRIES);
	fault_puts(" (defined ");
	fault_putdec((UW)APP_VECT_DEFINED);
	fault_puts(")\n  replaced : ");
	fault_putdec((UW)(n_irq + n_exc));
	fault_puts(" (unhandled IRQ ");
	fault_putdec((UW)n_irq);
	fault_puts(", fault exc ");
	fault_putdec((UW)n_exc);
	fault_puts(")\n");
}
