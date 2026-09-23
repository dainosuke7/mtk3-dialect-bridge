#ifndef TRACE_LOG_H
#define TRACE_LOG_H

#include <tk/tkernel.h>

/*
 * 出力の集約 (Phase 1 タスク9)
 *
 * UART に書くのはレポータタスク (優先度20、trace_task.c) だけにする。書き手は1行ぶんを
 * その場で文字列にしてメッセージバッファへ入れ、すぐ戻る。
 *
 *   - tk_snd_mbf は必ず TMO_POL。いっぱいなら捨てて log_dropped() で数える
 *     (音声タスクや推論タスクを UART の速さ〈115200bps ≒ 1文字 87us〉に引きずられて
 *      待たせないため)。ミューテックスは使わない
 *   - 文字列化は書き手のスタックで行う。LOG_MSG が 1つ載る (170B 程度) ので、
 *     スタックの小さいタスクから長い行を出さないこと
 *   - ISR (DMA コールバック) からは呼ばない。TRACE() とカウンタだけにする
 *     (書式化はコールバックでやる処理ではない)
 *   - レポータができる前 (log_init() より前) に呼ぶと、その場で直接 UART に出す。
 *     起動時の初期化ログはこれで今までどおり出る
 *
 * 行の順序: レポータを通る出力どうしは送った順に出る。tm_printf を直接呼ぶ処理
 * (起動時の初期化、npu_selftest、トレースの CSV ダンプ、fault の直叩き) とは前後し得る。
 * それらは音声・推論が動き出す前か、ダンプ中 (trace_muted() で他を止める) なので混ざらない。
 */

#define LOG_TEXT_MAX	(160)	/* 1行の上限 (改行と終端の NUL を含む)。超えた分は切り捨てる */

/* メッセージの種類 */
#define LOG_TYPE_TEXT	(0)	/* body = NUL 終端の文字列 */
#define LOG_TYPE_RATE	(1)	/* body = trace_task.c の rate_rec_t */

/*
 * メッセージの形。先頭 2 語を共通にして、body の位置を 4 バイト境界に保つ
 * (LOG_TYPE_RATE の body は UW を含む構造体)。
 * 送り手は type と t_send を入れ、log_send() に body の長さを渡す
 */
typedef struct {
	UW	type;		/* LOG_TYPE_* */
	UW	t_send;		/* 送った時刻 (DWT CYCCNT)。待ち時間の測定用 */
	UB	body[LOG_TEXT_MAX];
} LOG_MSG;

/* メッセージバッファを作る。レポータタスクを起こす前に1回 (trace_task_start から) */
EXPORT ER log_init(void);

/* log_init() が済んでいるか (済んでいなければ log_printf は直接 UART に出す) */
EXPORT BOOL log_ready(void);

/*
 * 1行を書式化してレポータに渡す。改行は呼び出し側が書式に入れる。
 * 書式は tm_printf の部分集合: %d %i %u %x %X %c %s %% と、フラグ '-' '0'・幅
 * (%-15s, %08x, %4d など)。精度・浮動小数点は使えない (サイズ修飾子 l/h/z は読み飛ばす)。
 *
 * 書式と引数の数が合っているかを確かめたいときは、この宣言に一時的に
 *   __attribute__((format(printf, 1, 2)))
 * を付けてビルドし、-Wformat-extra-args と "too few arguments" だけを見る
 * (タスク9 でこれを使い、audio_task.c の2行で引数の数が合っていないのを見つけた)。
 * UW は unsigned long なので %u は毎回警告になる。付けたままにはしない
 */
EXPORT void log_printf(const char *fmt, ...);

/*
 * 計測にかかわる出力を囲む区切り (目で見つけやすくし、ログから抜き出す目印にする)。
 *
 *   log_block_begin("TAP TOTAL win=%u", n)  区切り2行 + 見出し + 区切り2行
 *   log_printf(...)                         本文 (書式と数値は囲む前と同じまま)
 *   log_block_end()                         区切り2行
 *
 * 見出しだけで本文が無いもの (READY) は log_block_begin() だけを呼ぶ。
 * 見出しに改行は入れない (log_block_begin が足す)
 */
#define LOG_RULE	"===================================="

EXPORT void log_block_begin(const char *fmt, ...);
EXPORT void log_block_end(void);

/*
 * 組み立て済みのメッセージを渡す。msg は先頭が LOG_MSG と同じ 2 語 (type, t_send) で
 * 始まる構造体、body_len は body の長さ。t_send は log_send が入れる。
 * 戻り値: E_OK / E_QOVR いっぱいで捨てた / E_OBJ まだ log_init していない
 */
EXPORT ER log_send(void *msg, INT body_len);

/* レポータタスクだけが呼ぶ。戻り値: body の長さ / 負なら tk_rcv_mbf のエラー */
EXPORT INT log_recv(LOG_MSG *msg, TMO tmout);

/*
 * レポータが1件出し終えるたびに、送られてから出し終わるまでを入れる。
 * 通知の遅れ (JSON の lat_ms) に載る、UART の待ちぶんの実測値になる
 */
EXPORT void log_note_done(UW t_send);

EXPORT UW log_sent(void);	/* 渡せた数 */
EXPORT UW log_dropped(void);	/* いっぱいで捨てた数 */
EXPORT UW log_lag_max_us(void);	/* 送ってから出し終わるまでの最大 */

/*
 * log_lag_max_us() の測り直しを予約する。トレースの CSV ダンプが終わったところで呼ぶ。
 * 0 に戻すのは「キューを出し切った時点」で、ダンプが終わった瞬間ではない。
 * ダンプ中はレポータが行を捨てるだけで測らず、ダンプの前後に溜まった行はダンプ直後に
 * まとめて出るので、終わった瞬間に 0 にしてもその行たちでまた大きな値が立つ
 */
EXPORT void log_lag_reset_when_idle(void);

/*
 * レポータがキューを出し切ったときに呼ぶ (レポータタスクだけが呼ぶ)。
 * 上の予約があれば、ここで log_lag_max_us() を 0 に戻す
 */
EXPORT void log_lag_note_idle(void);

#endif	/* TRACE_LOG_H */
