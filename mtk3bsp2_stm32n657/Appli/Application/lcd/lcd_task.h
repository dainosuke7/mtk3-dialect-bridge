#ifndef LCD_LCD_TASK_H
#define LCD_LCD_TASK_H

#include <tk/tkernel.h>

/*
 * 表示タスク (優先度25。Phase 2 タスク2-1・2-2)
 *
 * 音声 (task_pcm 5、task_audio・task_1 10) と推論 (15) とレポータ (20) より下に置く。
 * 表示が遅れても音は途切れないし、UART のログも詰まらない。
 *
 * 中身:
 *   起動時に LCD を初期化して待機表示 ("READY") を出す。
 *   以後は notify.c から届いた検出をクラス名 (英字) で画面中央に大きく出し、
 *   LCD_HOLD_MS の間そのままにする。その間に別のクラスが来たら上書きする。
 *   時間が過ぎたら待機表示に戻す。画面の隅には1秒ごとに変わる数字 (生存表示) を出す。
 *   描くのは書き換える行だけで、全画面は消さない。
 *
 * 出力 (UART) はすべて log_printf 経由。
 */

/* 検出を表示しておく時間 */
#define LCD_HOLD_MS		(3000)

/*
 * 表示タスクを作って起動する。npu_hw_init() (AXISRAM3 のクロックと電源) の後に
 * usermain から1回だけ呼ぶ。LCD の初期化はタスクの中で行うのですぐ戻る。
 * 戻り値: E_OK / その他 タスクかメッセージバッファを作れなかった
 */
EXPORT ER lcd_task_start(void);

/*
 * 検出を表示タスクへ渡す (notify.c から、通知を出すときに呼ぶ)。
 *   cls:      クラス番号 (notify.h の並び)
 *   p100:     確率 x100
 *   win:      窓の通し番号
 *   t_notify: 通知した時刻 (DWT CYCCNT)。表示までの時間を測るのに使う
 *
 * tk_snd_mbf は TMO_POL で、いっぱいなら捨てて数える (JSON と同じ流儀。
 * 推論タスクを表示の都合で待たせない)。戻り値: E_OK / E_QOVR 捨てた / E_OBJ 未初期化
 */
EXPORT ER lcd_post(INT cls, INT p100, UW win, UW t_notify);

/*
 * *sent 渡せた数 / *dropped いっぱいで捨てた数 /
 * *lat_max_us *lat_avg_us 通知から描き終わりまで (集計行に出す)
 */
EXPORT void lcd_post_stats(UW *sent, UW *dropped, UW *lat_max_us, UW *lat_avg_us);

#endif	/* LCD_LCD_TASK_H */
