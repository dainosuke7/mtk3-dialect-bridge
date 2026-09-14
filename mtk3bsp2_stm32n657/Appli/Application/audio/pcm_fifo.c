#include <tk/tkernel.h>
#include "main.h"	// __DMB (CMSIS)
#include "pcm_fifo.h"

#define PCM_FIFO_MASK	(PCM_FIFO_CAPACITY - 1)

LOCAL H fifo_buf[PCM_FIFO_CAPACITY];

/* head/tailは累計サンプル数として単調増加させ、実インデックスはマスクで
 * 求める。こうすると head - tail が折り返しをまたいでも正しく個数になり、
 * 満杯と空の区別に1要素を犠牲にする必要もない */
LOCAL volatile UW fifo_head;	// 生産者のみが更新
LOCAL volatile UW fifo_tail;	// 消費者のみが更新

EXPORT void pcm_fifo_reset(void)
{
	fifo_head = 0;
	fifo_tail = 0;
}

EXPORT UINT pcm_fifo_count(void)
{
	return (UINT)(fifo_head - fifo_tail);
}

EXPORT UINT pcm_fifo_push(const H *src, UINT n)
{
	UW	head, space;
	UINT	i;

	head  = fifo_head;
	space = PCM_FIFO_CAPACITY - (head - fifo_tail);
	if(n > space) n = space;

	for(i = 0; i < n; i++) {
		fifo_buf[(head + i) & PCM_FIFO_MASK] = src[i];
	}

	/* データの書き込みが head の更新より先に見えることを保証する */
	__DMB();
	fifo_head = head + n;

	return n;
}

EXPORT UINT pcm_fifo_pop(H *dst, UINT n)
{
	UW	tail, avail;
	UINT	i;

	tail  = fifo_tail;
	avail = fifo_head - tail;
	if(n > avail) n = avail;

	for(i = 0; i < n; i++) {
		dst[i] = fifo_buf[(tail + i) & PCM_FIFO_MASK];
	}

	/* データの読み出しが tail の更新より先に完了することを保証する */
	__DMB();
	fifo_tail = tail + n;

	return n;
}
