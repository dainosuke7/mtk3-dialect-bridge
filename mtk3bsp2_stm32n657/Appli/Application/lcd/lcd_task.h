#ifndef LCD_LCD_TASK_H
#define LCD_LCD_TASK_H

#include <tk/tkernel.h>

/*
 * 表示タスク (優先度25。Phase 2 タスク2-1)
 *
 * 音声 (task_pcm 5、task_audio・task_1 10) と推論 (15) とレポータ (20) より下に置く。
 * 表示が遅れても音は途切れないし、UART のログも詰まらない。
 *
 * 今の中身: LCD を初期化して "HELLO" を1行描いたら寝る (検出結果との連動はタスク2-2 以降)。
 * 出力はすべて log_printf 経由 (UART に書くのはレポータだけ)。
 */

/*
 * 表示タスクを作って起動する。npu_hw_init() (AXISRAM3 のクロックと電源) の後に
 * usermain から1回だけ呼ぶ。初期化はタスクの中で行うのですぐ戻る。
 * 戻り値: E_OK / その他 タスクを作れなかった
 */
EXPORT ER lcd_task_start(void);

#endif	/* LCD_LCD_TASK_H */
