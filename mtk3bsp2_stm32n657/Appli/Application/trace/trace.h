#ifndef TRACE_H
#define TRACE_H

#include <tk/tkernel.h>
#include "main.h"	// CMSIS (DWT, DCB, SystemCoreClock)

/*
 * 時間計測基盤。
 *
 * 記録は常時ではなく区間方式。trace_start(ms) で始め、指定時間の経過か
 * リング満杯で自動停止し、そのままCSVダンプへ移る。停止中の TRACE() は
 * 即 return するので溢れない。
 *
 * 確認済みの前提:
 *   DI/EI は BASEPRI ベース (syslib.h:40)
 *   DWT_Type に LAR は無い。0xFB0 を直接叩く (core_cm55.h)
 *   tk_snd_mbf は TMO_POL ならタスク独立部から呼べる (messagebuf.c:314)
 *   tm_printf の書式は %d %i %u %x %X %o %p %s %c のみ
 */

/* ---------------------------------------------------------------- */
/* 時刻 (DWT CYCCNT)                                                  */
/* ---------------------------------------------------------------- */

/* 差分は必ず UW 同士の引き算で取る (32bit折り返しをまたいでも正しい) */
#define NOW()		((UW)DWT->CYCCNT)

EXPORT void trace_init(void);
EXPORT BOOL trace_cyccnt_valid(void);
EXPORT UW   trace_core_clock(void);
EXPORT UW   trace_cyc_per_us(void);
EXPORT UW   trace_cyc_to_us(UW cyc);

/* ---------------------------------------------------------------- */
/* イベントID                                                         */
/* ---------------------------------------------------------------- */

typedef enum {
	EV_NONE		= 0,
	EV_DMA_IN_HALF	= 1,	/* MDF 前半取り込み完了  arg=連番下位8bit */
	EV_DMA_IN_FULL	= 2,	/* MDF 後半取り込み完了  arg=連番下位8bit */
	EV_DMA_OUT_HALF	= 3,	/* SAI 前半再生完了      arg=連番下位8bit */
	EV_DMA_OUT_FULL	= 4,	/* SAI 後半再生完了      arg=連番下位8bit */
	EV_AUD_START	= 5,	/* 処理開始  arg: 0=入力側 1=出力側 */
	EV_AUD_END	= 6,	/* 処理終了  arg: 0=入力側 1=出力側 */
	EV_UNDERRUN	= 7,
	EV_OVERRUN	= 8,
	EV_INF_START	= 9,	/* Phase 2 */
	EV_INF_END	= 10,	/* Phase 2 */
	EV_NOTIFY	= 11,	/* Phase 2 */
	EV_MAX
} trace_ev_t;

/* ---------------------------------------------------------------- */
/* 記録                                                               */
/* ---------------------------------------------------------------- */

#define TRACE_RING_ENTRIES	(4096)		/* 8B x 4096 = 32KB */

typedef struct {
	UW	t;	/* DWT CYCCNT (生値。単調化はダンプ側で行う) */
	UB	id;
	UB	arg;
	UH	pad;
} trace_ent_t;

typedef enum {
	TRACE_IDLE = 0,		/* 記録も出力もしていない */
	TRACE_RECORDING,	/* 記録中 */
	TRACE_FULL,		/* 記録終了、ダンプ待ち */
	TRACE_DUMPING		/* CSV出力中 */
} trace_state_t;

/* 停止理由 */
#define TRACE_STOP_MANUAL	(0)
#define TRACE_STOP_TIME		(1)
#define TRACE_STOP_FULL		(2)

/* ISRからでもタスクからでも呼べる。記録中以外は何もしない */
#define TRACE(id, arg)	trace_put((UB)(id), (UB)(arg))

EXPORT void trace_put(UB id, UB arg);

/* 記録開始。duration_ms 経過かリング満杯で自動停止し、ダンプへ移る */
EXPORT void trace_start(UW duration_ms);

/* 記録を手動で打ち切る */
EXPORT void trace_stop(void);

EXPORT trace_state_t trace_state(void);

/* 記録中またはダンプ中なら TRUE。完了待ちに使う */
EXPORT BOOL trace_busy(void);

EXPORT UW trace_count(void);		/* 記録済み件数 */
EXPORT UW trace_dropped(void);		/* リング溢れで失った件数 */

/* ---------------------------------------------------------------- */
/* ダンプ中の他出力の抑制                                             */
/* ---------------------------------------------------------------- */

/*
 * TRUE の間、呼び出し側は tm_printf を行わないこと。CSVに他タスクの
 * ログが混入するのを防ぐ。TRUE を返すたびに抑制件数を1加算する
 * (何件のログが失われたかをダンプ末尾で報告するため)。
 *
 *   if(!trace_muted()) tm_printf(...);
 */
EXPORT BOOL trace_muted(void);

EXPORT UW trace_muted_count(void);

/* ---------------------------------------------------------------- */
/* 計測タスク / 実効レート                                            */
/* ---------------------------------------------------------------- */

/* レポータ(優先度20)とダンプ(優先度32)を作る。usermain から1回だけ */
EXPORT ER trace_task_start(void);

/* 1秒周期の実効レート表示。ring_level は FIFO残量を返す関数(NULL可) */
EXPORT ER   trace_rate_start(UINT (*ring_level)(void));
EXPORT void trace_rate_stop(void);

/* DMAコールバックから呼ぶ */
EXPORT void trace_rate_add_in(UINT samples);
EXPORT void trace_rate_add_out(UINT samples);
EXPORT void trace_rate_note_underrun(void);
EXPORT void trace_rate_note_overrun(void);

#endif	/* TRACE_H */
