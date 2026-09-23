#ifndef AUDIO_TAP_RING_H
#define AUDIO_TAP_RING_H

#include <tk/tkernel.h>

/*
 * 推論用のタップリング (Phase 1 タスク7)
 *
 * マイクの int16 (task_pcm が pcm_fifo に入れるのと同じ値) を溜め、推論用の窓で取り出す。
 * pcm_fifo (パススルー専用、SPSC) とは別。
 *
 *   書き手: task_pcm (優先度5) だけ。決して待たない。読み手が遅れていれば古いデータを上書きする
 *   読み手: 推論タスク (優先度15) だけ。自分の読み出し位置を持ち、上書きされたら検出して数え、
 *           最新側へ読み位置を進める
 *
 * 窓の切り方は ST の GettingStarted-Audio (audio_bm.c:223-236) と同じ:
 *   窓 = 15600 サンプル (975ms、log-mel 96 列 = 160 x 95 + 400)、次の窓は 15360 サンプル後から。
 *   前の窓の末尾 240 サンプル (= 400 - 160) が次の窓の先頭と重なる
 */

#define TAP_RING_CAPACITY	(32768)		/* 2 の冪 (マスクで位置を出す)。16kHz で約2秒 */
#define TAP_WIN_LEN		(15600)
#define TAP_WIN_HOP		(15360)
#define TAP_WIN_OVERLAP		(TAP_WIN_LEN - TAP_WIN_HOP)	/* 240 */

typedef struct {
	UW	seq;		/* 取り出した窓の通し番号 (0 から) */
	UW	pos;		/* 窓の先頭の、書き込み開始からの累計サンプル番号 */
	UW	t_ready;	/* 窓の最後のサンプルを書いた時刻 (DWT CYCCNT)。通知の遅れの起点 */
	UW	lag_us;		/* 窓がそろってから取り出すまで */
	BOOL	lag_exact;	/* FALSE: 読み手が待ち位置を出す前に窓がそろっていたので、lag_us と t_ready は下限 */
	BOOL	resync;		/* 直前に上書きで読み位置を飛ばした (前の窓とつながっていない) */
} TAP_WIN_INFO;

typedef struct {
	UW	head;		/* 書いた累計サンプル数 */
	UW	windows;	/* 取り出した窓の数 */
	UW	overrun;	/* 取り出す前に上書きされていた回数 */
	UW	torn;		/* コピー中に上書きされて捨てた回数 */
	UW	skipped;	/* 上書きで読み飛ばしたサンプル数 */
	UW	late;		/* 待ち位置を出す前に窓がそろっていた回数 (lag が下限値) */
	UW	lag_max_us;	/* 窓がそろってから取り出すまでの最大 */
	UW	wr_calls;	/* tap_ring_write の回数 */
	UW	wr_max_cyc;	/* tap_ring_write 1回の処理時間の最大 (DWT) */
	UW	wr_sum_cyc;	/* 同、合計 (平均 = wr_sum_cyc / wr_calls。約7秒で折り返すので区間で差を取る) */
} TAP_STATS;

/* セマフォを作り、位置を 0 にする。書き手・読み手のどちらも動き出す前に1回 (usermain) */
EXPORT ER tap_ring_init(void);

/* 書き手 (task_pcm): n サンプル書く (n <= TAP_RING_CAPACITY)。待たない */
EXPORT void tap_ring_write(const H *src, UINT n);

/*
 * 読み手: 次の窓 TAP_WIN_LEN サンプルを dst にコピーし、読み位置を TAP_WIN_HOP 進める。
 * 窓がそろうまで最大 tmout ms 待つ (書き手が窓をそろえたときに1回だけ起こす)。
 * 戻り値: E_OK 取り出した / E_TMOUT そろわなかった /
 *         E_OBJ 取り出す前に上書きされていた / E_IO コピー中に上書きされた
 *         (E_OBJ・E_IO は数えたうえで読み位置を最新の窓へ進めている。次の呼び出しで取り出せる)
 */
EXPORT ER tap_ring_get_window(H *dst, TMO tmout, TAP_WIN_INFO *info);

/* 統計のスナップショット */
EXPORT void tap_ring_stats(TAP_STATS *st);

/* 書いたサンプル数から見た、そろっているはずの窓の数 (上書きで読み飛ばしていなければ windows と同じか1多い) */
EXPORT UW tap_ring_expected_windows(void);

#endif	/* AUDIO_TAP_RING_H */
