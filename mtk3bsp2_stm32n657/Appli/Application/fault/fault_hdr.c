#include <tk/tkernel.h>
#include "main.h"	// CMSIS (SCB, __get_IPSR)
#include "fault_out.h"

/*
 * SFSR / SFAR は core_cm55.h の SCB_Type にオフセット 0x0E4 / 0x0E8 として
 * 無条件で定義されている (0xE000ED00 + 0xE4 = 0xE000EDE4)。
 */

/* ---------------------------------------------------------------- */
/*
 * 未実装IRQ用ハンドラ
 */
EXPORT void GenericUnhandled_Handler(void)
{
	UW	excno = __get_IPSR();

	fault_puts("\n[UNHANDLED IRQ] ");
	if(excno >= 16) {
		fault_puts("IRQn=");
		fault_putdec(excno - 16);
		fault_puts(" (exc=");
		fault_putdec(excno);
		fault_puts(")\n");
	} else {
		/* 本来ここには来ない(16以降にしか差し込んでいない) */
		fault_puts("exc=");
		fault_putdec(excno);
		fault_puts("\n");
	}
	fault_halt();
}

/* ---------------------------------------------------------------- */
/*
 * フォルト例外用ハンドラ
 */

LOCAL const char *exc_name(UW excno)
{
	switch(excno) {
	case 3:  return "HardFault";
	case 4:  return "MemManage";
	case 5:  return "BusFault";
	case 6:  return "UsageFault";
	case 7:  return "SecureFault";
	default: return "?";
	}
}

/* フラグが立っていれば名前を出す */
LOCAL void put_flag(UW val, UW msk, const char *name)
{
	if((val & msk) != 0) {
		fault_putc(' ');
		fault_puts(name);
	}
}

EXPORT void fault_report(const UW *sp, UW exc_return);

/*
 * naked で入り、EXC_RETURN の bit2 で MSP/PSP を選んで C側へ渡す。
 * bit2==0 → 例外前はMSP使用、bit2==1 → PSP使用。
 *
 */
__attribute__((naked)) EXPORT void Fault_Handler(void)
{
	__asm volatile (
		"	mov	r2, #0			\n"
		"	msr	msplim, r2		\n"
		"	msr	psplim, r2		\n"
		"	tst	lr, #4			\n"
		"	ite	eq			\n"
		"	mrseq	r0, msp			\n"
		"	mrsne	r0, psp			\n"
		"	mov	r1, lr			\n"
		"	b	fault_report		\n"
	);
}

EXPORT void fault_report(const UW *sp, UW exc_return)
{
	UW	excno = __get_IPSR();
	UW	cfsr  = SCB->CFSR;
	UW	hfsr  = SCB->HFSR;
	UW	mmfar = SCB->MMFAR;
	UW	bfar  = SCB->BFAR;
	UW	sfsr  = SCB->SFSR;
	UW	sfar  = SCB->SFAR;

	fault_puts("\n*** FAULT ***\n");

	fault_puts("  exc        : ");
	fault_putdec(excno);
	fault_puts(" (");
	fault_puts(exc_name(excno));
	fault_puts(")\n");

	fault_puts("  CFSR       : 0x");
	fault_puthex(cfsr, 8);
	/* MMFSR */
	put_flag(cfsr, SCB_CFSR_IACCVIOL_Msk,   "IACCVIOL");
	put_flag(cfsr, SCB_CFSR_DACCVIOL_Msk,   "DACCVIOL");
	put_flag(cfsr, SCB_CFSR_MUNSTKERR_Msk,  "MUNSTKERR");
	put_flag(cfsr, SCB_CFSR_MSTKERR_Msk,    "MSTKERR");
	put_flag(cfsr, SCB_CFSR_MLSPERR_Msk,    "MLSPERR");
	put_flag(cfsr, SCB_CFSR_MMARVALID_Msk,  "MMARVALID");
	/* BFSR */
	put_flag(cfsr, SCB_CFSR_IBUSERR_Msk,    "IBUSERR");
	put_flag(cfsr, SCB_CFSR_PRECISERR_Msk,  "PRECISERR");
	put_flag(cfsr, SCB_CFSR_IMPRECISERR_Msk,"IMPRECISERR");
	put_flag(cfsr, SCB_CFSR_UNSTKERR_Msk,   "UNSTKERR");
	put_flag(cfsr, SCB_CFSR_STKERR_Msk,     "STKERR");
	put_flag(cfsr, SCB_CFSR_LSPERR_Msk,     "LSPERR");
	put_flag(cfsr, SCB_CFSR_BFARVALID_Msk,  "BFARVALID");
	/* UFSR */
	put_flag(cfsr, SCB_CFSR_UNDEFINSTR_Msk, "UNDEFINSTR");
	put_flag(cfsr, SCB_CFSR_INVSTATE_Msk,   "INVSTATE");
	put_flag(cfsr, SCB_CFSR_INVPC_Msk,      "INVPC");
	put_flag(cfsr, SCB_CFSR_NOCP_Msk,       "NOCP");
	put_flag(cfsr, SCB_CFSR_STKOF_Msk,      "STKOF");
	put_flag(cfsr, SCB_CFSR_UNALIGNED_Msk,  "UNALIGNED");
	put_flag(cfsr, SCB_CFSR_DIVBYZERO_Msk,  "DIVBYZERO");
	fault_puts("\n");

	fault_puts("  HFSR       : 0x");
	fault_puthex(hfsr, 8);
	put_flag(hfsr, SCB_HFSR_VECTTBL_Msk,  "VECTTBL");
	put_flag(hfsr, SCB_HFSR_FORCED_Msk,   "FORCED");
	put_flag(hfsr, SCB_HFSR_DEBUGEVT_Msk, "DEBUGEVT");
	fault_puts("\n");

	fault_puts("  MMFAR      : 0x");
	fault_puthex(mmfar, 8);
	if((cfsr & SCB_CFSR_MMARVALID_Msk) == 0) fault_puts(" (invalid)");
	fault_puts("\n");

	fault_puts("  BFAR       : 0x");
	fault_puthex(bfar, 8);
	if((cfsr & SCB_CFSR_BFARVALID_Msk) == 0) fault_puts(" (invalid)");
	fault_puts("\n");

	fault_puts("  SFSR       : 0x");
	fault_puthex(sfsr, 8);
	put_flag(sfsr, SAU_SFSR_INVEP_Msk,     "INVEP");
	put_flag(sfsr, SAU_SFSR_INVIS_Msk,     "INVIS");
	put_flag(sfsr, SAU_SFSR_INVER_Msk,     "INVER");
	put_flag(sfsr, SAU_SFSR_AUVIOL_Msk,    "AUVIOL");
	put_flag(sfsr, SAU_SFSR_INVTRAN_Msk,   "INVTRAN");
	put_flag(sfsr, SAU_SFSR_LSPERR_Msk,    "LSPERR");
	put_flag(sfsr, SAU_SFSR_SFARVALID_Msk, "SFARVALID");
	put_flag(sfsr, SAU_SFSR_LSERR_Msk,     "LSERR");
	fault_puts("\n");

	fault_puts("  SFAR       : 0x");
	fault_puthex(sfar, 8);
	if((sfsr & SAU_SFSR_SFARVALID_Msk) == 0) fault_puts(" (invalid)");
	fault_puts("\n");

	/*
	 * 例外スタックフレーム: R0,R1,R2,R3,R12,LR,PC,xPSR の順。
	 * STKOF ではフレームの書き込み自体が失敗していることがあり、
	 * その場合はここの値は当てにならない(HFSR に FORCED が立つ)。
	 */
	fault_puts("  stacked PC : 0x");
	fault_puthex(sp[6], 8);
	fault_puts("\n  stacked LR : 0x");
	fault_puthex(sp[5], 8);
	fault_puts("\n  stacked PSR: 0x");
	fault_puthex(sp[7], 8);
	fault_puts("\n  EXC_RETURN : 0x");
	fault_puthex(exc_return, 8);
	fault_puts(((exc_return & 0x4UL) != 0) ? " (PSP)\n" : " (MSP)\n");

	fault_halt();
}
