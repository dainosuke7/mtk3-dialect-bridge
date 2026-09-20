#include <tk/tkernel.h>
#include "main.h"
#include "trace.h"
#include "trace_ring.h"

LOCAL trace_ent_t	trace_ring[TRACE_RING_ENTRIES];

/*
 * 区間記録なので折り返さない。満杯になった時点で記録を止めるため、
 * head はそのまま件数になり、マスクも不要。
 */
LOCAL volatile UW		trace_head;
LOCAL volatile UW		trace_drop;
LOCAL volatile trace_state_t	trace_st = TRACE_IDLE;
LOCAL volatile UINT		trace_reason = TRACE_STOP_MANUAL;
LOCAL volatile UW		trace_dur_ms;
LOCAL volatile UW		trace_muted_n;

EXPORT trace_state_t trace_state(void)	{ return trace_st; }
EXPORT UW trace_count(void)		{ return trace_head; }
EXPORT UW trace_dropped(void)		{ return trace_drop; }
EXPORT UINT trace_stop_reason(void)	{ return trace_reason; }
EXPORT UW trace_duration_ms(void)	{ return trace_dur_ms; }
EXPORT UW trace_muted_count(void)	{ return trace_muted_n; }

EXPORT BOOL trace_busy(void)
{
	return (trace_st == TRACE_RECORDING || trace_st == TRACE_DUMPING) ? TRUE : FALSE;
}

EXPORT BOOL trace_muted(void)
{
	if(trace_st != TRACE_DUMPING) return FALSE;
	trace_muted_n++;
	return TRUE;
}

EXPORT void trace_set_state(trace_state_t st)
{
	trace_st = st;
}

EXPORT void trace_start(UW duration_ms)
{
	UW	imask;

	DI(imask);
	trace_head    = 0;
	trace_drop    = 0;
	trace_muted_n = 0;
	trace_dur_ms  = duration_ms;
	trace_reason  = TRACE_STOP_MANUAL;
	trace_st      = TRACE_RECORDING;
	EI(imask);
}

EXPORT void trace_stop_with(UINT reason)
{
	if(trace_st == TRACE_RECORDING) {
		trace_reason = reason;
		trace_st     = TRACE_FULL;
	}
}

EXPORT void trace_stop(void)
{
	trace_stop_with(TRACE_STOP_MANUAL);
}

/*
 * 時刻は DI の前に読む。TRACE() が呼ばれた瞬間に最も近い値にしたいため。
 * 並びが時刻順にならないことがあるが、解析はPC側で t を見ればよい。
 * 割り込み禁止区間は head の取得と進行だけ。
 */
EXPORT void trace_put(UB id, UB arg)
{
	UW		t, imask, idx;
	trace_ent_t	*e;

	if(trace_st != TRACE_RECORDING) return;

	t = NOW();

	DI(imask);
	idx = trace_head;
	if(idx < TRACE_RING_ENTRIES) trace_head = idx + 1;
	EI(imask);

	if(idx >= TRACE_RING_ENTRIES) {
		trace_reason = TRACE_STOP_FULL;
		trace_st     = TRACE_FULL;
		return;
	}

	e = &trace_ring[idx];
	e->t   = t;
	e->id  = id;
	e->arg = arg;
	e->pad = 0;

	if(idx == (TRACE_RING_ENTRIES - 1)) {
		trace_reason = TRACE_STOP_FULL;
		trace_st     = TRACE_FULL;
	}
}

EXPORT const trace_ent_t *trace_ring_entry(UW idx)
{
	if(idx >= trace_head) return NULL;
	return &trace_ring[idx];
}
