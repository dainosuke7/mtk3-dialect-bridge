#ifndef TRACE_RING_H
#define TRACE_RING_H

#include <tk/tkernel.h>
#include "trace.h"

/* ダンプ側インタフェース (trace内部用)。記録側は trace.h にある */

/* idx 番目のエントリ。idx < trace_count() であること */
EXPORT const trace_ent_t *trace_ring_entry(UW idx);

/* 状態遷移。ダンプタスクだけが呼ぶ */
EXPORT void trace_set_state(trace_state_t st);

/* 理由を指定して記録を打ち切る */
EXPORT void trace_stop_with(UINT reason);

/* 直近の停止理由 (TRACE_STOP_*) */
EXPORT UINT trace_stop_reason(void);

/* trace_start() で指定された記録時間 (サイクル換算) */
EXPORT UW trace_duration_ms(void);

#endif	/* TRACE_RING_H */
