#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "main.h"
#include "trace.h"
#include "trace_ring.h"
#include "log.h"

/* ---------------------------------------------------------------- */
/* 実効レートの母数                                                   */
/* ---------------------------------------------------------------- */

/* 自然境界の32bit変数なので単独のload/storeはアトミック。読み側のDI/EI不要 */
LOCAL volatile UW	rate_in_samples;
LOCAL volatile UW	rate_out_samples;
LOCAL volatile UW	rate_underrun;
LOCAL volatile UW	rate_overrun;

EXPORT void trace_rate_add_in(UINT samples)	{ rate_in_samples  += samples; }
EXPORT void trace_rate_add_out(UINT samples)	{ rate_out_samples += samples; }
EXPORT void trace_rate_note_underrun(void)	{ rate_underrun++; }
EXPORT void trace_rate_note_overrun(void)	{ rate_overrun++; }

/* ---------------------------------------------------------------- */
/* 周期ハンドラ -> レポータタスク                                     */
/* ---------------------------------------------------------------- */

typedef struct {
	UW	d_in;
	UW	d_out;
	UW	d_cyc;
	UW	ring;
	UW	under;
	UW	over;
} rate_rec_t;

/* レポータに渡す形。先頭 2 語は LOG_MSG と同じ (log.h) */
typedef struct {
	UW		type;
	UW		t_send;
	rate_rec_t	rec;
} rate_msg_t;

#define RATE_PERIOD_MS	(1000)

LOCAL ID	rate_cycid   = 0;
LOCAL ID	tskid_report = 0;
LOCAL ID	tskid_dump   = 0;
LOCAL ID	uart_mtxid   = 0;

LOCAL UINT	(*rate_ring_level)(void);

LOCAL UW	prev_in;
LOCAL UW	prev_out;
LOCAL UW	prev_t;

/* 周期ハンドラ内は引き算だけ。Hz換算と表示はタスクに回す */
LOCAL void rate_cychdr(void *exinf)
{
	rate_msg_t	m;
	UW		now_in, now_out, now_t;

	now_in  = rate_in_samples;
	now_out = rate_out_samples;
	now_t   = NOW();

	m.rec.d_in  = (UW)(now_in  - prev_in);
	m.rec.d_out = (UW)(now_out - prev_out);
	m.rec.d_cyc = (UW)(now_t   - prev_t);
	m.rec.ring  = (rate_ring_level != NULL) ? (UW)rate_ring_level() : 0;
	m.rec.under = rate_underrun;
	m.rec.over  = rate_overrun;

	prev_in  = now_in;
	prev_out = now_out;
	prev_t   = now_t;

	/* 書式化はレポータに任せる。tk_snd_mbf は TMO_POL なのでタスク独立部から呼べる */
	m.type = LOG_TYPE_RATE;
	(void)log_send(&m, sizeof(m.rec));
}

/* ---------------------------------------------------------------- */
/* UART出力の直列化                                                   */
/* ---------------------------------------------------------------- */

LOCAL void uart_lock(void)
{
	if(uart_mtxid > 0) (void)tk_loc_mtx(uart_mtxid, TMO_FEVR);
}

LOCAL void uart_unlock(void)
{
	if(uart_mtxid > 0) (void)tk_unl_mtx(uart_mtxid);
}

/* ---------------------------------------------------------------- */
/* レポータタスク (優先度20)                                          */
/* ---------------------------------------------------------------- */

/* 実効レートの1行 (換算と表示はここで行う) */
LOCAL void report_rate(const rate_rec_t *rec)
{
	UW	dt_us, in_hz, out_hz, drop;

	dt_us = trace_cyc_to_us(rec->d_cyc);
	if(dt_us == 0) {
		in_hz  = 0;
		out_hz = 0;
	} else {
		in_hz  = (UW)(((uint64_t)rec->d_in  * 1000000ULL) / dt_us);
		out_hz = (UW)(((uint64_t)rec->d_out * 1000000ULL) / dt_us);
	}
	drop = log_dropped();

	uart_lock();
	tm_printf((UB*)"in=%uHz out=%uHz ring=%u under=%u over=%u (dt=%uus)",
			in_hz, out_hz, rec->ring, rec->under, rec->over, dt_us);
	if(drop != 0) {
		/* 行があふれて捨てられている。LOG_DEPTH か行数を見直す目印 */
		tm_printf((UB*)" logdrop=%u", drop);
	}
	tm_printf((UB*)" loglag=%uus\n", log_lag_max_us());
	uart_unlock();
}

/*
 * UART に書くのはこのタスクだけ (トレースの CSV ダンプと、レポータができる前の
 * 起動時のログを除く)。送られた順に出すので、行が混ざらない
 */
LOCAL void task_report(INT stacd, void *exinf)
{
	LOG_MSG	msg;
	INT	sz;

	while(1) {
		/*
		 * まず溜まっている分を取る。空なら「出し切った」ので、予約されている
		 * loglag の測り直しをここで済ませてから待ちに入る (log.h)
		 */
		sz = log_recv(&msg, TMO_POL);
		if(sz == E_TMOUT) {
			log_lag_note_idle();
			sz = log_recv(&msg, TMO_FEVR);
		}
		if(sz < 0) continue;

		/* ダンプ中は黙る。CSVに混ざるのを防ぐ */
		if(trace_muted()) continue;

		switch(msg.type) {
		case LOG_TYPE_TEXT:
			uart_lock();
			tm_putstring(msg.body);
			uart_unlock();
			break;

		case LOG_TYPE_RATE:
			if(sz >= (INT)sizeof(rate_rec_t)) report_rate((const rate_rec_t *)msg.body);
			break;

		default:
			break;
		}

		/* 送られてから出し終わるまで (通知の遅れに載る UART の待ちぶん) */
		log_note_done(msg.t_send);
	}
}

/* ---------------------------------------------------------------- */
/* ダンプタスク (優先度32 = 最低)。状態機械の駆動も担当               */
/* ---------------------------------------------------------------- */

#define DUMP_CHUNK	(16)
#define POLL_MS		(20)

/* 記録中の経過監視。書くのはこのタスクだけなので排他不要 */
LOCAL uint64_t	rec_elapsed;
LOCAL UW	rec_prev;
LOCAL BOOL	rec_armed;

/*
 * 32bit CYCCNT の差分。生値は約7.16秒(600MHz)で折り返すので、必ず
 * これを通して64bitに積み上げる。逆転(並びの前後)は0とみなす。
 */
LOCAL UW cyc_delta(UW now, UW prev)
{
	UW	d = (UW)(now - prev);

	return (d & 0x80000000UL) ? 0 : d;
}

LOCAL void record_watch(void)
{
	UW		now = NOW();
	uint64_t	dur_cyc;

	if(!rec_armed) {
		rec_armed   = TRUE;
		rec_elapsed = 0;
		rec_prev    = now;
		return;
	}
	rec_elapsed += (uint64_t)cyc_delta(now, rec_prev);
	rec_prev     = now;

	dur_cyc = (uint64_t)trace_duration_ms() * (uint64_t)trace_core_clock() / 1000ULL;
	if(dur_cyc != 0 && rec_elapsed >= dur_cyc) {
		trace_stop_with(TRACE_STOP_TIME);
	}
}

/* 最初と最後のタイムスタンプの差 */
LOCAL UW recorded_span_us(void)
{
	const trace_ent_t	*e;
	UW			n = trace_count(), i, prev;
	uint64_t		acc = 0;

	if(n < 2 || trace_core_clock() == 0) return 0;

	e    = trace_ring_entry(0);
	prev = e->t;
	for(i = 1; i < n; i++) {
		e = trace_ring_entry(i);
		acc += (uint64_t)cyc_delta(e->t, prev);
		prev = e->t;
	}
	return (UW)((acc * 1000000ULL) / (uint64_t)trace_core_clock());
}

LOCAL const char *reason_name(void)
{
	switch(trace_stop_reason()) {
	case TRACE_STOP_TIME: return "time";
	case TRACE_STOP_FULL: return "ring-full";
	default:              return "manual";
	}
}

LOCAL void dump_all(void)
{
	const trace_ent_t	*e;
	UW			n, i, prev, span_us, t_us, clk;
	uint64_t		acc;
	UINT			c;

	n       = trace_count();
	clk     = trace_core_clock();
	span_us = recorded_span_us();

	uart_lock();
	tm_printf((UB*)"#trace begin clk=%uHz entries=%u span_us=%u stop=%s\n",
			clk, n, span_us, reason_name());
	if(trace_dropped() != 0) {
		tm_printf((UB*)"#trace ***********************************************\n");
		tm_printf((UB*)"#trace *** WARNING: ring overflow, dropped=%u ***\n",
				trace_dropped());
		tm_printf((UB*)"#trace ***********************************************\n");
	}
	tm_printf((UB*)"t_us,id,arg\n");
	uart_unlock();

	/* t_us は先頭0からの単調増加。生値の折り返しはここで吸収する */
	acc  = 0;
	prev = 0;
	i    = 0;
	while(i < n) {
		uart_lock();
		for(c = 0; c < DUMP_CHUNK && i < n; c++, i++) {
			e = trace_ring_entry(i);
			if(e == NULL) break;
			if(i == 0) prev = e->t;
			acc += (uint64_t)cyc_delta(e->t, prev);
			prev = e->t;
			t_us = (clk != 0) ? (UW)((acc * 1000000ULL) / (uint64_t)clk) : 0;
			tm_printf((UB*)"%u,%u,%u\n", t_us, (UW)e->id, (UW)e->arg);
		}
		uart_unlock();
		tk_dly_tsk(1);
	}

	uart_lock();
	tm_printf((UB*)"#trace end dropped=%u muted=%u\n",
			trace_dropped(), trace_muted_count());
	uart_unlock();

	/*
	 * ダンプのあいだレポータは行を捨てるだけで、ダンプの前後に溜まった行は
	 * この直後にまとめて出る。通知の遅れの目安として見る値なので測り直すが、
	 * 0 に戻すのはレポータが溜まった行を出し切ってから (log.h)
	 */
	log_lag_reset_when_idle();
}

LOCAL void task_dump(INT stacd, void *exinf)
{
	while(1) {
		switch(trace_state()) {
		case TRACE_RECORDING:
			record_watch();
			tk_dly_tsk(POLL_MS);
			break;

		case TRACE_FULL:
			rec_armed = FALSE;
			trace_set_state(TRACE_DUMPING);
			dump_all();
			trace_set_state(TRACE_IDLE);
			break;

		default:
			rec_armed = FALSE;
			tk_dly_tsk(POLL_MS * 5);
			break;
		}
	}
}

/* ---------------------------------------------------------------- */
/* 生成 / 開始 / 停止                                                 */
/* ---------------------------------------------------------------- */

LOCAL T_CMTX cmtx_uart = {
	.mtxatr	 = TA_TFIFO | TA_INHERIT,
	.ceilpri = 0,
};

LOCAL T_CTSK ctsk_report = {
	.itskpri = 20,
	.stksz	 = 1536,		/* LOG_MSG (約170B) をスタックに置く */
	.task	 = task_report,
	.tskatr	 = TA_HLNG | TA_RNG3,
};

LOCAL T_CTSK ctsk_dump = {
	.itskpri = 32,			/* CNF_MAX_TSKPRI */
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
	ER	er;

	id = tk_cre_mtx(&cmtx_uart);
	if(id < E_OK) return (ER)id;
	uart_mtxid = id;

	/* 出力を集めるメッセージバッファ。レポータを起こす前に作る */
	er = log_init();
	if(er < E_OK) return er;

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
