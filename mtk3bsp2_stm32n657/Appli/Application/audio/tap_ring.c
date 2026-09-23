#include <tk/tkernel.h>
#include <string.h>
#include "main.h"		// CMSIS (__DMB)
#include "tap_ring.h"
#include "../trace/trace.h"	// NOW(), trace_cyc_to_us(), TRACE()

/*
 * 同期の考え方
 *
 * head (書いた累計サンプル数) は書き手だけが書き、rd_want (読み手が待っている head の値) は
 * 読み手だけが書く。どちらも 32bit の1回のストアなので、相手がいつ読んでも壊れた値は見ない。
 * 累計は 2^32 で折り返す (16kHz で約74時間) ので、大小の比較は必ず UW の差で行う。
 *
 * 順序 (__DMB):
 *   このチップは単一コアで、書き手 (task_pcm) と読み手 (推論タスク) は同じコア上の別タスク。
 *   タスクの切り替わりは必ず例外 (PendSV・割り込み) を挟み、同じコアのメモリアクセスは
 *   そのコア自身からはプログラム順に見えるので、ハードウェアが順序を入れ替えて相手に見せる
 *   ことは無い。D キャッシュも同じコアの同じキャッシュで、DMA は関わらないので保守は要らない。
 *   気を付けるのはコンパイラの並べ替え: head は volatile だが tap_buf は volatile でないので、
 *   コンパイラは tap_buf の読み書きを head のアクセスの前後に動かしてよい。__DMB() は
 *   "memory" クロバー付きのインラインアセンブラ (cmsis_gcc.h) なので、コンパイラはその前後に
 *   メモリアクセスを動かさない。DMB 命令そのものは単一コアでは不要だが、数サイクルなので残す。
 *     書き手: データを書く → __DMB() → head を進める
 *     読み手: head を読む → __DMB() → データをコピー → __DMB() → head を読み直して上書きを確かめる
 *
 * 起こし方 (毎回は signal しない):
 *   書き手は head を進めたとき、読み手の待ち位置 rd_want を今回の書き込みで越えた
 *   (rd_want が (旧 head, 新 head] にある) ときだけ、セマフォを1回 signal する。
 *   読み手は rd_want を出してから head を確かめ、足りなければ待つ。
 *   - rd_want を出す前に書き手が越えていた: signal は無いが、読み手が直後の確認で気付く
 *   - rd_want を出した後・確認の前に越えた: 確認で気付いて取り出し、signal が1回余る。
 *     次の待ちが1回空振りするだけ (読み手はループで確かめ直す)。上限1なので余りは溜まらない
 *   読み手 (優先度15) は書き手 (優先度5) を横取りできないので、書き手の中で rd_want は変わらない。
 */

#define MASK		(TAP_RING_CAPACITY - 1U)
#define SR_HZ		(16000U)	/* 遅れの下限の見積もりにだけ使う (実レートは約16128Hz) */

_Static_assert((TAP_RING_CAPACITY & MASK) == 0, "TAP_RING_CAPACITY must be a power of 2");
_Static_assert(TAP_WIN_LEN <= TAP_RING_CAPACITY, "window must fit in the ring");

LOCAL H			tap_buf[TAP_RING_CAPACITY];	/* 64KB。静的領域 */
LOCAL volatile UW	tap_head = 0;			/* 書き手だけが書く */
LOCAL volatile UW	rd_want = TAP_WIN_LEN;		/* 読み手だけが書く (= rd_pos + TAP_WIN_LEN) */
LOCAL volatile UW	ready_for = 0;			/* 書き手が最後に越えた rd_want */
LOCAL volatile UW	ready_cyc = 0;			/* そのときの DWT */

/* 読み手だけが使う */
LOCAL UW		rd_pos = 0;
LOCAL BOOL		rd_resync = FALSE;

LOCAL ID		semid = 0;
LOCAL T_CSEM		csem = {
	.sematr		= TA_TFIFO | TA_FIRST,
	.isemcnt	= 0,
	.maxsem		= 1,
};

/* 統計。読み手側は読み手だけが、書き込み時間は書き手だけが更新する */
LOCAL UW		st_windows, st_overrun, st_torn, st_skipped, st_late, st_lag_max_us;
LOCAL volatile UW	st_wr_calls, st_wr_sum, st_wr_max;

EXPORT ER tap_ring_init(void)
{
	semid = tk_cre_sem(&csem);
	return (semid < E_OK) ? semid : E_OK;
}

EXPORT void tap_ring_write(const H *src, UINT n)
{
	UW	t0 = NOW();
	UW	h = tap_head;
	UW	i = h & MASK;
	UW	first = TAP_RING_CAPACITY - i;
	UW	want, dt;

	if(first > n) first = n;
	memcpy(&tap_buf[i], src, first * sizeof(H));
	if(n > first) memcpy(&tap_buf[0], src + first, (n - first) * sizeof(H));

	__DMB();		/* データを書き終えてから head を進める */
	tap_head = h + n;

	/* 読み手の待ち位置を今回越えたら、1回だけ起こす */
	want = rd_want;
	if((UW)(want - h - 1U) < n) {
		ready_for = want;
		ready_cyc = NOW();
		if(semid > 0) (void)tk_sig_sem(semid, 1);
		TRACE(EV_TAP_READY, (UB)((want - TAP_WIN_LEN) / TAP_WIN_HOP));
	}

	dt = (UW)(NOW() - t0);
	st_wr_calls++;
	st_wr_sum += dt;
	if(dt > st_wr_max) st_wr_max = dt;
}

/* 上書きされていた: いま取り出せる最新の窓へ読み位置を進める (h - rd_pos > 容量 >= 窓 なので前進する) */
LOCAL void resync_to(UW h)
{
	UW	np = h - TAP_WIN_LEN;

	st_skipped += (UW)(np - rd_pos);
	rd_pos    = np;
	rd_resync = TRUE;
	rd_want   = rd_pos + TAP_WIN_LEN;	/* = h: もうそろっている */
}

EXPORT ER tap_ring_get_window(H *dst, TMO tmout, TAP_WIN_INFO *info)
{
	UW	want = rd_pos + TAP_WIN_LEN;	/* rd_want に出してある値 */
	UW	h, h2, i, first, rf, rc, lag, t_ready;
	UINT	imask;
	BOOL	exact;
	ER	er;

	for(;;) {
		h = tap_head;
		if((W)(h - want) >= 0) break;
		er = tk_wai_sem(semid, 1, tmout);
		if(er < E_OK) return er;	/* E_TMOUT ほか */
	}

	if((UW)(h - rd_pos) > TAP_RING_CAPACITY) {
		st_overrun++;
		resync_to(h);
		return E_OBJ;
	}

	__DMB();		/* head を読んでからデータを読む */
	i = rd_pos & MASK;
	first = TAP_RING_CAPACITY - i;
	if(first > TAP_WIN_LEN) first = TAP_WIN_LEN;
	memcpy(dst, &tap_buf[i], first * sizeof(H));
	if(first < TAP_WIN_LEN) memcpy(dst + first, &tap_buf[0], (TAP_WIN_LEN - first) * sizeof(H));
	__DMB();		/* データを読み終えてから head を読み直す */

	h2 = tap_head;
	if((UW)(h2 - rd_pos) > TAP_RING_CAPACITY) {
		/* コピーしている間に書き手が窓の先頭まで回ってきた。コピーした値は信用できない */
		st_torn++;
		resync_to(h2);
		return E_IO;
	}

	/* 窓がそろってからの遅れ。書き手が待ち位置を越えた時刻を記録していればそこから測る */
	DI(imask);
	rf = ready_for;
	rc = ready_cyc;
	EI(imask);
	if(rf == want && !rd_resync) {
		t_ready = rc;
		lag     = trace_cyc_to_us((UW)(NOW() - rc));
		exact   = TRUE;
	} else {
		/* 待ち位置を出す前にそろっていた: 窓の後ろに溜まっていた分の長さが遅れの下限 */
		lag     = (UW)(((uint64_t)(UW)(h2 - want) * 1000000ULL) / SR_HZ);
		t_ready = (UW)(NOW() - (UW)((uint64_t)lag * (uint64_t)trace_cyc_per_us()));
		exact   = FALSE;
		st_late++;
	}
	if(lag > st_lag_max_us) st_lag_max_us = lag;

	info->seq       = st_windows;
	info->pos       = rd_pos;
	info->t_ready   = t_ready;
	info->lag_us    = lag;
	info->lag_exact = exact;
	info->resync    = rd_resync;
	TRACE(EV_TAP_GET, (UB)st_windows);
	st_windows++;
	rd_resync = FALSE;

	/*
	 * 次の窓の待ち位置をすぐ出す。窓の処理 (タスク9 では前処理と推論) をしている間に
	 * 次の窓がそろっても、書き手が時刻を記録できる
	 */
	rd_pos += TAP_WIN_HOP;
	rd_want = rd_pos + TAP_WIN_LEN;
	return E_OK;
}

EXPORT void tap_ring_stats(TAP_STATS *st)
{
	UINT	imask;

	DI(imask);
	st->head       = tap_head;
	st->wr_calls   = st_wr_calls;
	st->wr_sum_cyc = st_wr_sum;
	st->wr_max_cyc = st_wr_max;
	EI(imask);
	st->windows    = st_windows;
	st->overrun    = st_overrun;
	st->torn       = st_torn;
	st->skipped    = st_skipped;
	st->late       = st_late;
	st->lag_max_us = st_lag_max_us;
}

EXPORT UW tap_ring_expected_windows(void)
{
	UW	h = tap_head;

	return (h >= TAP_WIN_LEN) ? ((h - TAP_WIN_LEN) / TAP_WIN_HOP + 1U) : 0U;
}
