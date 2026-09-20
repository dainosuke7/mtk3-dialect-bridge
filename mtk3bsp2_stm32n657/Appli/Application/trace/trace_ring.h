#ifndef TRACE_RING_H
#define TRACE_RING_H

#include <tk/tkernel.h>
#include "trace.h"

/*
 * リングのダンプ側インタフェース (trace内部用)。
 * 記録側 (trace_put / trace_enable / trace_ring_reset / trace_dropped) は
 * trace.h に公開してある。
 */

/* 未ダンプのエントリを1件取り出す。取れたら TRUE。
 * 一周されて失われた分があれば trace_drop に加算して読み飛ばす */
EXPORT BOOL trace_ring_get(trace_ent_t *out);

/* 未ダンプ件数 (上書きで失われた分は含まない) */
EXPORT UW trace_ring_pending(void);

#endif	/* TRACE_RING_H */
