#ifndef TRACE_H
#define TRACE_H

#include <tk/tkernel.h>
#include "main.h"	// CMSIS (DWT, DCB, SystemCoreClock)

/*
 * 時間計測基盤
 *
 * ねらい: 「たぶん16kHzで動いている」を実測値に置き換える。
 *         ボード上では数えるだけで、統計はPC側で出す。
 *
 * 確認済みの前提 (該当ヘッダを読んで確定させた事実):
 *
 *  1. DI/EI は BASEPRI ベース。
 *     mtk3_bsp2/include/tk/sysdepend/stm32_cube/cpu/core/armv8m/syslib.h
 *       #define DI(intsts)  ( (intsts) = disint() )   … UW disint(void)
 *       #define EI(intsts)  ( set_basepri(intsts) )   … void set_basepri(UW)
 *     PRIMASK ではないので NMI とフォルトは止まらない。
 *     カーネル管理下の割り込み(SAI/MDF)は止まるので TRACE の排他には十分。
 *
 *  2. core_cm55.h の DWT_Type に LAR フィールドは無い。
 *     0xFB0 は RESERVED14[968] の中なので直アドレスで叩く必要がある。
 *
 *  3. tk_snd_mbf は in_indp() を明示的に扱っており、TMO_POL なら
 *     周期ハンドラ(タスク独立部)から呼べる (messagebuf.c:314)。
 *
 *  4. tm_printf が対応する書式は %d %i %u %x %X %o %p %s %c のみ。
 *     %f と 64bit 長は使えないので、出すのは整数だけにしてある。
 */

/* ---------------------------------------------------------------- */
/* 時刻 (DWT CYCCNT)                                                  */
/* ---------------------------------------------------------------- */

/*
 * 現在のサイクルカウンタ。
 * 差分は必ず UW 同士の引き算で取ること。32bit の折り返しをまたいでも
 * 正しい経過サイクル数になる (600MHz なら約7.16秒で一周する)。
 *   UW t0 = NOW(); ... ; UW elapsed = NOW() - t0;   ← これが正しい
 */
#define NOW()		((UW)DWT->CYCCNT)

/* DWT CYCCNT を有効化する。usermain() の先頭付近で1回だけ呼ぶ */
EXPORT void trace_init(void);

/* CYCCNT が実際に動いているか。FALSE なら以降の計測値は無意味 */
EXPORT BOOL trace_cyccnt_valid(void);

/* 実行時に読んだ SystemCoreClock の値 (推測値ではない) */
EXPORT UW trace_core_clock(void);

/* SystemCoreClock / 1000000。表示用の目安 */
EXPORT UW trace_cyc_per_us(void);

/* サイクル数をマイクロ秒に変換する (64bit経由なので丸め誤差が乗らない) */
EXPORT UW trace_cyc_to_us(UW cyc);

/* ---------------------------------------------------------------- */
/* イベントID                                                         */
/* ---------------------------------------------------------------- */

typedef enum {
	EV_NONE		= 0,
	EV_DMA_IN_HALF	= 1,	/* MDF 前半取り込み完了    arg=連番下位8bit */
	EV_DMA_IN_FULL	= 2,	/* MDF 後半取り込み完了    arg=連番下位8bit */
	EV_DMA_OUT_HALF	= 3,	/* SAI 前半再生完了        arg=連番下位8bit */
	EV_DMA_OUT_FULL	= 4,	/* SAI 後半再生完了        arg=連番下位8bit */
	EV_AUD_START	= 5,	/* 音声処理 開始  arg: 0=入力側 1=出力側 */
	EV_AUD_END	= 6,	/* 音声処理 終了  arg: 0=入力側 1=出力側 */
	EV_UNDERRUN	= 7,	/* SAI側でFIFOが不足 */
	EV_OVERRUN	= 8,	/* MDF側でFIFOが満杯 */
	/* --- 以下 Phase 2 用。今は定義のみで発行しない --- */
	EV_INF_START	= 9,	/* 推論 開始 */
	EV_INF_END	= 10,	/* 推論 終了 */
	EV_NOTIFY	= 11,	/* 通知 */
	EV_MAX
} trace_ev_t;

/* ---------------------------------------------------------------- */
/* トレースリング                                                     */
/* ---------------------------------------------------------------- */

/* 1エントリ8バイト x 4096 = 32KB */
#define TRACE_RING_ENTRIES	(4096)

typedef struct {
	UW	t;	/* DWT CYCCNT */
	UB	id;	/* trace_ev_t */
	UB	arg;
	UH	pad;
} trace_ent_t;

/*
 * イベントを1件記録する。ISRからでもタスクからでも呼べる。
 * 割り込み禁止区間は head のインクリメントだけ (数命令)。
 */
#define TRACE(id, arg)	trace_put((UB)(id), (UB)(arg))

EXPORT void trace_put(UB id, UB arg);

/* 記録の有効/無効。初期状態は無効 */
EXPORT void trace_enable(BOOL on);

/* リングを空にして溢れカウントも0に戻す */
EXPORT void trace_ring_reset(void);

/* ダンプが追いつかず上書きで失われた件数 */
EXPORT UW trace_dropped(void);

/* ---------------------------------------------------------------- */
/* 出力制御                                                           */
/* ---------------------------------------------------------------- */

/*
 * 計測タスク群を作る。usermain() から1回だけ呼ぶ。
 *   - レポータタスク (優先度20): 周期ハンドラからの実効レートを表示
 *   - ダンプタスク   (優先度32=最低): トレースをCSVで吐く
 * UART出力は両者で共有する mutex (TA_INHERIT) で直列化する。
 */
EXPORT ER trace_task_start(void);

/* トレースのCSVダンプを開始/停止する (常時垂れ流しにはしない) */
EXPORT void trace_dump_start(void);
EXPORT void trace_dump_stop(void);

/* 残っている分を吐き切ったら自動で止まる。記録を止めてから呼ぶ */
EXPORT void trace_dump_drain(void);

/* ダンプ中かどうか。trace_dump_drain() の完了待ちに使う */
EXPORT BOOL trace_dump_busy(void);

/* 1秒周期の実効レート表示を開始/停止する。
 * ring_level は FIFO残量を返す関数 (trace が audio に依存しないよう注入する)。
 * NULL可。 */
EXPORT ER   trace_rate_start(UINT (*ring_level)(void));
EXPORT void trace_rate_stop(void);

/* ---------------------------------------------------------------- */
/* 実効レートの母数。DMAコールバックから呼ぶ                          */
/* ---------------------------------------------------------------- */

EXPORT void trace_rate_add_in(UINT samples);
EXPORT void trace_rate_add_out(UINT samples);
EXPORT void trace_rate_note_underrun(void);
EXPORT void trace_rate_note_overrun(void);

#endif	/* TRACE_H */
