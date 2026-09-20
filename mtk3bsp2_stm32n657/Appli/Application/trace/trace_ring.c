#include <tk/tkernel.h>
#include "main.h"	// CMSIS (__DMB)
#include "trace.h"

#define TRACE_RING_MASK		(TRACE_RING_ENTRIES - 1)

LOCAL trace_ent_t	trace_ring[TRACE_RING_ENTRIES];

/*
 * head は累計イベント数として単調増加させ、実インデックスはマスクで求める
 * (pcm_fifo.c と同じ方式)。こうすると head - tail が折り返しをまたいでも
 * 正しく件数になる。
 */
LOCAL volatile UW	trace_head;	/* 記録側が進める */
LOCAL volatile UW	trace_tail;	/* ダンプ側が進める */
LOCAL volatile UW	trace_drop;	/* 上書きで失った件数 */
LOCAL volatile BOOL	trace_rec_on;

EXPORT void trace_enable(BOOL on)
{
	trace_rec_on = on;
}

EXPORT void trace_ring_reset(void)
{
	UW	imask;

	DI(imask);
	trace_head = 0;
	trace_tail = 0;
	trace_drop = 0;
	EI(imask);
}

EXPORT UW trace_dropped(void)
{
	return trace_drop;
}

/*
 * イベントを1件記録する。
 *
 * 時刻は DI の前に読む。TRACE() が呼ばれた瞬間に最も近い値にしたいため。
 * この結果、割り込みに割り込まれるとリング上の並びが時刻順にならない
 * ことがあるが、解析はPC側なので t でソートすればよい。
 * 順序より「その時刻が正確であること」を優先している。
 *
 * 割り込み禁止区間は head の取得と進行だけ。エントリへの書き込みは
 * 区間の外で行う (8バイトのstoreを禁止区間に入れない)。
 */
EXPORT void trace_put(UB id, UB arg)
{
	UW		t, imask, idx;
	trace_ent_t	*e;

	if(!trace_rec_on) return;

	t = NOW();

	DI(imask);
	idx = trace_head++;
	EI(imask);

	e = &trace_ring[idx & TRACE_RING_MASK];
	e->t   = t;
	e->id  = id;
	e->arg = arg;
	e->pad = 0;
}

/* ---------------------------------------------------------------- */
/* ダンプ側                                                           */
/* ---------------------------------------------------------------- */

/*
 * 未ダンプのエントリを1件取り出す。取れたら TRUE。
 *
 * ダンプが記録に追いつかれてリングを一周されていた場合、失われた分を
 * trace_drop に足して tail を有効範囲の先頭まで進める。
 */
EXPORT BOOL trace_ring_get(trace_ent_t *out)
{
	UW	head, tail, avail;

	head = trace_head;
	tail = trace_tail;
	avail = head - tail;

	if(avail == 0) return FALSE;

	if(avail > TRACE_RING_ENTRIES) {
		/* 一周されて古い分が上書きされている */
		UW lost = avail - TRACE_RING_ENTRIES;
		trace_drop += lost;
		tail += lost;
	}

	*out = trace_ring[tail & TRACE_RING_MASK];

	__DMB();
	trace_tail = tail + 1;

	return TRUE;
}

/* 未ダンプ件数 (上書きされた分は含まない) */
EXPORT UW trace_ring_pending(void)
{
	UW	avail = trace_head - trace_tail;

	return (avail > TRACE_RING_ENTRIES) ? TRACE_RING_ENTRIES : avail;
}
