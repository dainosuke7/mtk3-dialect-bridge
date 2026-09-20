#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "main.h"
#include "trace.h"
#include "trace_ring.h"

/* ---------------------------------------------------------------- */
/* 実効レートの母数                                                   */
/* ---------------------------------------------------------------- */

/*
 * DMAコールバック(割り込み)から加算し、周期ハンドラで差分を取る。
 * いずれも自然境界に載った32bit変数なので、Cortex-M では単独の
 * load/store がアトミックになる。読み側で DI/EI は要らない。
 */
LOCAL volatile UW	rate_in_samples;	/* MDFが取り込んだ累計サンプル数 */
LOCAL volatile UW	rate_out_samples;	/* SAIへ送った累計サンプル数 */
LOCAL volatile UW	rate_underrun;
LOCAL volatile UW	rate_overrun;

EXPORT void trace_rate_add_in(UINT samples)	{ rate_in_samples  += samples; }
EXPORT void trace_rate_add_out(UINT samples)	{ rate_out_samples += samples; }
EXPORT void trace_rate_note_underrun(void)	{ rate_underrun++; }
EXPORT void trace_rate_note_overrun(void)	{ rate_overrun++; }

/* ---------------------------------------------------------------- */
/* 周期ハンドラ -> レポータタスク                                     */
/* ---------------------------------------------------------------- */

/*
 * 周期ハンドラの中では値の取得だけを行い、tm_printf はタスクに回す。
 * 周期ハンドラ内で重い処理をすると、測ろうとしている当のタイミングを
 * 乱してしまうため。
 *
 * 受け渡しは CLAUDE.md の規約どおりメッセージバッファを使う。
 * tk_snd_mbf は in_indp() を明示的に扱っており、TMO_POL なら
 * タスク独立部から呼べる (mtkernel/kernel/tkernel/messagebuf.c:314)。
 */
typedef struct {
	UW	d_in;		/* この周期で入力したサンプル数 */
	UW	d_out;		/* この周期で出力したサンプル数 */
	UW	d_cyc;		/* この周期の実測経過サイクル数 */
	UW	ring;		/* FIFO残量 */
	UW	under;
	UW	over;
	UW	drop;		/* トレース取りこぼし累計 */
} rate_rec_t;

#define RATE_PERIOD_MS	(1000)

LOCAL ID	rate_mbfid   = 0;
LOCAL ID	rate_cycid   = 0;
LOCAL ID	tskid_report = 0;
LOCAL ID	tskid_dump   = 0;
LOCAL ID	uart_mtxid   = 0;

LOCAL UINT	(*rate_ring_level)(void);	/* FIFO残量の取得(注入) */

/* 前回スナップショット */
LOCAL UW	prev_in;
LOCAL UW	prev_out;
LOCAL UW	prev_t;

LOCAL void rate_cychdr(void *exinf)
{
	rate_rec_t	rec;
	UW		now_in, now_out, now_t;

	now_in  = rate_in_samples;
	now_out = rate_out_samples;
	now_t   = NOW();

	/* ここでは引き算だけ。Hzへの換算(64bit除算)はタスク側に回す。
	 * 経過時間は tk_get_otm(ms) ではなく DWT の実測サイクル数で持つ
	 * (CLAUDE.md の規約。周期ハンドラの起動ゆらぎもこれで吸収される) */
	rec.d_in  = (UW)(now_in  - prev_in);
	rec.d_out = (UW)(now_out - prev_out);
	rec.d_cyc = (UW)(now_t   - prev_t);
	rec.ring  = (rate_ring_level != NULL) ? (UW)rate_ring_level() : 0;
	rec.under = rate_underrun;
	rec.over  = rate_overrun;
	rec.drop  = trace_dropped();

	prev_in  = now_in;
	prev_out = now_out;
	prev_t   = now_t;

	/* 溜まっていたら捨てる。計測を止めてまで送る価値はない */
	(void)tk_snd_mbf(rate_mbfid, &rec, sizeof(rec), TMO_POL);
}

/* ---------------------------------------------------------------- */
/* UART出力の直列化                                                   */
/* ---------------------------------------------------------------- */

/*
 * レポータ(優先度20)とダンプ(優先度32)が同時に tm_printf すると行が
 * 混ざるため mutex で囲む。CLAUDE.md の規約どおり優先度継承付き。
 * なお audio_task 等の既存の tm_printf はこの mutex を通らないので、
 * そちらとは依然として混ざりうる。
 */
LOCAL void uart_lock(void)
{
	if(uart_mtxid > 0) (void)tk_loc_mtx(uart_mtxid, TMO_FEVR);
}

LOCAL void uart_unlock(void)
{
	if(uart_mtxid > 0) (void)tk_unl_mtx(uart_mtxid);
}

/* ---------------------------------------------------------------- */
/* レポータタスク                                                     */
/* ---------------------------------------------------------------- */

LOCAL void task_report(INT stacd, void *exinf)
{
	rate_rec_t	rec;
	INT		sz;
	UW		dt_us, in_hz, out_hz;

	while(1) {
		sz = tk_rcv_mbf(rate_mbfid, &rec, TMO_FEVR);
		if(sz < (INT)sizeof(rec)) continue;

		/* 実効レート = サンプル数 / 実測経過時間。分母は周期ハンドラが
		 * DWT で測った値なので、起動ゆらぎがあっても正しく出る */
		dt_us = trace_cyc_to_us(rec.d_cyc);
		if(dt_us == 0) {
			in_hz  = 0;
			out_hz = 0;
		} else {
			in_hz  = (UW)(((uint64_t)rec.d_in  * 1000000ULL) / dt_us);
			out_hz = (UW)(((uint64_t)rec.d_out * 1000000ULL) / dt_us);
		}

		uart_lock();
		tm_printf((UB*)"in=%uHz out=%uHz ring=%u under=%u over=%u",
				in_hz, out_hz, rec.ring, rec.under, rec.over);
		if(rec.drop != 0) tm_printf((UB*)" tracedrop=%u", rec.drop);
		tm_printf((UB*)" (dt=%uus)\n", dt_us);
		uart_unlock();
	}
}

/* ---------------------------------------------------------------- */
/* ダンプタスク (最低優先度)                                          */
/* ---------------------------------------------------------------- */

LOCAL volatile BOOL	dump_req;	/* 1でダンプ中 */
LOCAL volatile BOOL	dump_header;	/* ヘッダ未出力 */
LOCAL volatile BOOL	dump_drain;	/* 1なら吐き切った時点で自動停止 */

/* 1回のロック保持で出す件数。115200bpsだと1行約18文字=1.6msなので
 * 16行で約25ms。ここを大きくするとレポータの1秒行がその分遅れる */
#define DUMP_CHUNK	(16)

LOCAL void task_dump(INT stacd, void *exinf)
{
	trace_ent_t	e;
	UINT		n;

	while(1) {
		if(!dump_req) {
			tk_dly_tsk(50);
			continue;
		}

		if(dump_header) {
			uart_lock();
			/* t_us は CYCCNT を変換した値なので wrap_us で折り返す。
			 * PC側はこれを使って巻き戻りを検出すること */
			tm_printf((UB*)"#trace begin clk=%uHz wrap_us=%u\n",
					trace_core_clock(), trace_cyc_to_us(0xFFFFFFFFUL));
			tm_printf((UB*)"t_us,id,arg\n");
			uart_unlock();
			dump_header = FALSE;
		}

		if(trace_ring_pending() == 0) {
			if(dump_drain) {
				dump_drain = FALSE;
				dump_req   = FALSE;
				uart_lock();
				tm_printf((UB*)"#trace end dropped=%u\n", trace_dropped());
				uart_unlock();
				continue;
			}
			tk_dly_tsk(20);
			continue;
		}

		uart_lock();
		for(n = 0; n < DUMP_CHUNK; n++) {
			if(!trace_ring_get(&e)) break;
			/* ボード上では統計を出さない。生データのまま流す */
			tm_printf((UB*)"%u,%u,%u\n",
					trace_cyc_to_us(e.t), (UW)e.id, (UW)e.arg);
		}
		uart_unlock();

		/* 他タスクに譲る。最低優先度なので本来不要だが、
		 * 同優先度が増えたときのために明示しておく */
		tk_dly_tsk(1);
	}
}

EXPORT void trace_dump_start(void)
{
	dump_header = TRUE;
	dump_drain  = FALSE;
	dump_req    = TRUE;
}

/* 残っている分を吐き切ったら自動で止まる。記録を止めてから呼ぶこと */
EXPORT void trace_dump_drain(void)
{
	dump_header = TRUE;
	dump_drain  = TRUE;
	dump_req    = TRUE;
}

EXPORT BOOL trace_dump_busy(void)
{
	return dump_req;
}

EXPORT void trace_dump_stop(void)
{
	dump_req   = FALSE;
	dump_drain = FALSE;
}

/* ---------------------------------------------------------------- */
/* 生成 / 開始 / 停止                                                 */
/* ---------------------------------------------------------------- */

LOCAL T_CMTX cmtx_uart = {
	.mtxatr	 = TA_TFIFO | TA_INHERIT,	/* 優先度継承 (CLAUDE.md) */
	.ceilpri = 0,
};

LOCAL T_CMBF cmbf_rate = {
	.mbfatr	= TA_TFIFO,
	.bufsz	= sizeof(rate_rec_t) * 4,
	.maxmsz	= sizeof(rate_rec_t),
};

LOCAL T_CTSK ctsk_report = {
	.itskpri = 20,			/* audio(10) より下、dump より上 */
	.stksz	 = 1024,
	.task	 = task_report,
	.tskatr	 = TA_HLNG | TA_RNG3,
};

LOCAL T_CTSK ctsk_dump = {
	.itskpri = 32,			/* CNF_MAX_TSKPRI = 最低優先度 */
	.stksz	 = 1024,
	.task	 = task_dump,
	.tskatr	 = TA_HLNG | TA_RNG3,
};

LOCAL T_CCYC ccyc_rate = {
	.cycatr	= TA_HLNG,
	.cychdr	= (FP)rate_cychdr,
	.cyctim	= RATE_PERIOD_MS,
	.cycphs	= RATE_PERIOD_MS,
};

EXPORT ER trace_task_start(void)
{
	ID	id;

	id = tk_cre_mtx(&cmtx_uart);
	if(id < E_OK) return (ER)id;
	uart_mtxid = id;

	id = tk_cre_mbf(&cmbf_rate);
	if(id < E_OK) return (ER)id;
	rate_mbfid = id;

	id = tk_cre_tsk(&ctsk_report);
	if(id < E_OK) return (ER)id;
	tskid_report = id;
	(void)tk_sta_tsk(tskid_report, 0);

	id = tk_cre_tsk(&ctsk_dump);
	if(id < E_OK) return (ER)id;
	tskid_dump = id;
	(void)tk_sta_tsk(tskid_dump, 0);

	id = tk_cre_cyc(&ccyc_rate);
	if(id < E_OK) return (ER)id;
	rate_cycid = id;

	return E_OK;
}

EXPORT ER trace_rate_start(UINT (*ring_level)(void))
{
	if(rate_cycid <= 0) return E_OBJ;

	rate_ring_level  = ring_level;
	rate_in_samples  = 0;
	rate_out_samples = 0;
	rate_underrun    = 0;
	rate_overrun     = 0;
	prev_in  = 0;
	prev_out = 0;
	prev_t   = NOW();

	return tk_sta_cyc(rate_cycid);
}

EXPORT void trace_rate_stop(void)
{
	if(rate_cycid > 0) (void)tk_stp_cyc(rate_cycid);
}
