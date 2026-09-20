#include <tk/tkernel.h>
#include "main.h"	// CMSIS (DCB, __BKPT, __disable_irq)
#include "fault_out.h"

/*
 * USART1 のレジスタ。ベースアドレスの分岐条件は
 * mtk3_bsp2/sysdepend/stm32_cube/lib/libtm/discovery_stm32n657/tm_com.c
 * と同一にしてある(この構成では TRUSTZONE_ENABLE=1 / TRUSTZONE_SECURE=1
 * なのでセキュアエイリアス側が選ばれる)。
 */
#if (TRUSTZONE_ENABLE && TRUSTZONE_SECURE)
#define FAULT_UART_BASE		(0x52001000UL)	/* USART1 (secure alias) */
#else
#define FAULT_UART_BASE		(0x42001000UL)	/* USART1 (non-secure) */
#endif

#define FAULT_UART_ISR		(*(volatile UW*)(FAULT_UART_BASE + 0x001C))
#define FAULT_UART_TDR		(*(volatile UW*)(FAULT_UART_BASE + 0x0028))

#define UART_ISR_TXE		(0x00000080UL)	/* 送信データレジスタ空 */
#define UART_ISR_TC		(0x00000040UL)	/* 送信完了 */

/* UARTが死んでいてもハングしないための空回り上限 */
#define FAULT_UART_SPIN		(2000000UL)

EXPORT void fault_putc(char c)
{
	UW	spin = 0;

	while((FAULT_UART_ISR & UART_ISR_TXE) == 0) {
		if(++spin > FAULT_UART_SPIN) return;	/* 諦める */
	}
	FAULT_UART_TDR = (UW)(UB)c;
}

EXPORT void fault_puts(const char *s)
{
	if(s == NULL) return;
	while(*s != '\0') {
		fault_putc(*s++);
	}
}

EXPORT void fault_puthex(UW val, INT digits)
{
	static const char	hex[] = "0123456789ABCDEF";
	INT			i;

	if(digits < 1) digits = 1;
	if(digits > 8) digits = 8;

	for(i = digits - 1; i >= 0; i--) {
		fault_putc(hex[(val >> (i * 4)) & 0x0F]);
	}
}

EXPORT void fault_putdec(UW val)
{
	char	buf[11];	/* UW最大 4294967295 = 10桁 + NUL */
	INT	i = 0;

	if(val == 0) {
		fault_putc('0');
		return;
	}
	while(val > 0 && i < (INT)sizeof(buf)) {
		buf[i++] = (char)('0' + (val % 10));
		val /= 10;
	}
	while(--i >= 0) {
		fault_putc(buf[i]);
	}
}

EXPORT void fault_halt(void)
{
	UW	spin = 0;

	/* 最後の1文字が出きるまで待つ */
	while((FAULT_UART_ISR & UART_ISR_TC) == 0) {
		if(++spin > FAULT_UART_SPIN) break;
	}

	/* デバッガが繋がっていればそこで止める */
	if((DCB->DHCSR & DCB_DHCSR_C_DEBUGEN_Msk) != 0) {
		__BKPT(0);
	}

	__disable_irq();
	for(;;) { }
}
