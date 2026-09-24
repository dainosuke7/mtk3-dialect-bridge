#include <tk/tkernel.h>
#include "lcd_task.h"
#include "lcd.h"
#include "../trace/log.h"	// log_printf()

/*
 * 表示タスク。役割と優先度の理由は lcd_task.h。
 * タスク2-1 では固定の文字を1回描くだけなので、描いたら寝る。
 */

#define LCD_TASK_PRI	(25)
#define LCD_TASK_STKSZ	(1536)		/* log_printf が LOG_MSG (約170B) をスタックに置く */

#define HELLO_TEXT	"HELLO"
#define HELLO_SCALE	(3)		/* Font24 (17x24) の3倍 = 51x72 画素 */

LOCAL void task_lcd(INT stacd, void *exinf);
LOCAL ID	tskid_lcd;
LOCAL T_CTSK	ctsk_lcd = {
	.itskpri	= LCD_TASK_PRI,
	.stksz		= LCD_TASK_STKSZ,
	.task		= task_lcd,
	.tskatr		= TA_HLNG | TA_RNG3,
};

LOCAL void task_lcd(INT stacd, void *exinf)
{
	UINT	x, y, w, h;
	ER	er;

	er = lcd_init();
	log_printf("lcd_init: ret=%d\n", er);

	if(er == E_OK) {
		/* 黒地に白で1行。画面の中央に置く */
		w = lcd_text_width(HELLO_TEXT, HELLO_SCALE);
		h = lcd_text_height(HELLO_SCALE);
		x = (LCD_WIDTH > w) ? ((LCD_WIDTH - w) / 2U) : 0U;
		y = (LCD_HEIGHT > h) ? ((LCD_HEIGHT - h) / 2U) : 0U;

		lcd_clear(LCD_BLACK);
		lcd_text(x, y, HELLO_TEXT, LCD_WHITE, HELLO_SCALE);
		lcd_flush_rows(0, LCD_HEIGHT);		/* 全面を塗ったので全行 clean */

		log_printf("lcd: [ OK ] draw \"%s\" at (%u,%u) %ux%u px, scale %d\n",
				HELLO_TEXT, x, y, w, h, HELLO_SCALE);
	}

	/* タスク2-2 以降はここで検出結果を待って描き替える */
	tk_slp_tsk(TMO_FEVR);
}

EXPORT ER lcd_task_start(void)
{
	tskid_lcd = tk_cre_tsk(&ctsk_lcd);
	if(tskid_lcd < E_OK) return (ER)tskid_lcd;

	return tk_sta_tsk(tskid_lcd, 0);
}
