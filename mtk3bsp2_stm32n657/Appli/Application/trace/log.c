#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <stdarg.h>
#include <string.h>		// strlen
#include "log.h"
#include "trace.h"		// NOW(), trace_cyc_to_us()

/*
 * 出力の集約。考え方と制約は log.h。
 *
 * 書式化は自前で持つ (newlib の snprintf は使わない: μT-Kernel のシステムメモリと
 * ヒープが重なっていて malloc を起こせない。CLAUDE.md「既知の罠」)。
 * tm_printf と同じ部分集合で、長さの上限を必ず守る
 * (tm_sprintf は上限を取らないので、スタックに置く行バッファには使えない)。
 */

#define LOG_DEPTH	(32)	/* 溜められる行数の目安。UART が遅いので深めにする */

LOCAL ID	log_mbfid = 0;
LOCAL T_CMBF	cmbf_log = {
	.mbfatr		= TA_TFIFO,
	.bufsz		= sizeof(LOG_MSG) * LOG_DEPTH,
	.maxmsz		= sizeof(LOG_MSG),
};

/* 複数のタスク・周期ハンドラから増やすので DI/EI で守る */
LOCAL UW		n_sent, n_dropped, lag_max_us;

/* lag_max_us の測り直しの予約 (立てるのはダンプ側、下ろすのはレポータ) */
LOCAL volatile BOOL	lag_reset_pending;

/* ---------------------------------------------------------------- */
/* 書式化 (上限付き)                                                  */
/* ---------------------------------------------------------------- */

/* val を base 進数にして end の手前から詰める。戻り値は先頭 */
LOCAL char *put_num(char *end, UW val, UINT base, BOOL upper)
{
	const char	*digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

	do {
		*--end = digits[val % base];
		val /= base;
	} while(val != 0);
	return end;
}

/* buf に最大 size-1 文字 + NUL を書く。戻り値は NUL を除く長さ */
LOCAL INT vfmt(char *buf, INT size, const char *fmt, va_list ap)
{
	char		tmp[12];	/* UW は 10 進 10 桁 + 符号 */
	const char	*s;
	char		*p;
	UW		uval;
	W		sval;
	INT		n = 0, len = 0, wid, i, lim = size - 1;
	UINT		base;
	BOOL		left, zero, neg, upper;
	char		c;

	if(size <= 0) return 0;

	while((c = *fmt++) != '\0' && n < lim) {
		if(c != '%') {
			buf[n++] = c;
			continue;
		}

		/* フラグ */
		left = FALSE;
		zero = FALSE;
		for(;;) {
			c = *fmt++;
			if(c == '-')      left = TRUE;
			else if(c == '0') zero = TRUE;
			else              break;
		}

		/* 幅 */
		for(wid = 0; c >= '0' && c <= '9'; c = *fmt++) wid = wid * 10 + (c - '0');

		/* 精度は使わないので読み飛ばす */
		if(c == '.') {
			c = *fmt++;
			while(c >= '0' && c <= '9') c = *fmt++;
		}

		/* サイズ修飾子も読み飛ばす (このチップでは long も int も 32bit) */
		while(c == 'l' || c == 'h' || c == 'z') c = *fmt++;

		s     = NULL;
		neg   = FALSE;
		upper = FALSE;
		base  = 10;
		uval  = 0;
		switch(c) {
		case 'd':
		case 'i':
			sval = (W)va_arg(ap, INT);
			if(sval < 0) {
				neg  = TRUE;
				uval = (UW)(-sval);
			} else {
				uval = (UW)sval;
			}
			break;
		case 'u':
			uval = va_arg(ap, UW);
			break;
		case 'X':
			upper = TRUE;
			/* FALLTHROUGH */
		case 'x':
			base = 16;
			uval = va_arg(ap, UW);
			break;
		case 'c':
			tmp[0] = (char)va_arg(ap, INT);
			s      = tmp;
			len    = 1;
			break;
		case 's':
			s = va_arg(ap, const char *);
			if(s == NULL) s = "(null)";
			len = (INT)strlen(s);
			break;
		case '\0':
			goto done;
		default:
			/* %% と、知らない変換はその文字をそのまま出す */
			buf[n++] = c;
			continue;
		}

		if(s == NULL) {
			/* 0 埋めのときは符号を先に出す (-0012 にする) */
			if(neg && zero) {
				buf[n++] = '-';
				if(wid > 0) wid--;
				if(n >= lim) break;
			}
			p   = put_num(tmp + sizeof(tmp), uval, base, upper);
			len = (INT)(tmp + sizeof(tmp) - p);
			if(neg && !zero) {
				*--p = '-';
				len++;
			}
			s = p;
		}

		if(!left) {
			for(i = len; i < wid && n < lim; i++) buf[n++] = zero ? '0' : ' ';
		}
		for(i = 0; i < len && n < lim; i++) buf[n++] = s[i];
		if(left) {
			for(i = len; i < wid && n < lim; i++) buf[n++] = ' ';
		}
	}
done:
	buf[n] = '\0';
	return n;
}

/* ---------------------------------------------------------------- */
/* 送る / 受ける                                                      */
/* ---------------------------------------------------------------- */

EXPORT ER log_init(void)
{
	ID	id;

	if(log_mbfid > 0) return E_OK;

	id = tk_cre_mbf(&cmbf_log);
	if(id < E_OK) return (ER)id;
	log_mbfid = id;
	return E_OK;
}

EXPORT BOOL log_ready(void)
{
	return (log_mbfid > 0) ? TRUE : FALSE;
}

EXPORT ER log_send(void *msg, INT body_len)
{
	LOG_MSG	*m = (LOG_MSG *)msg;
	UINT	imask;
	ER	er;

	if(log_mbfid <= 0) return E_OBJ;

	m->t_send = NOW();
	er = tk_snd_mbf(log_mbfid, m, (INT)(2 * sizeof(UW)) + body_len, TMO_POL);

	DI(imask);
	if(er < E_OK) n_dropped++;
	else          n_sent++;
	EI(imask);

	return (er < E_OK) ? E_QOVR : E_OK;
}

EXPORT void log_printf(const char *fmt, ...)
{
	LOG_MSG	m;
	va_list	ap;
	INT	len;

	va_start(ap, fmt);
	len = vfmt((char *)m.body, LOG_TEXT_MAX, fmt, ap);
	va_end(ap);

	if(log_mbfid <= 0) {
		/* レポータがまだ無い (起動直後)。その場で出す */
		tm_putstring(m.body);
		return;
	}

	m.type = LOG_TYPE_TEXT;
	(void)log_send(&m, len + 1);		/* NUL も送る */
}

EXPORT INT log_recv(LOG_MSG *msg, TMO tmout)
{
	INT	sz = tk_rcv_mbf(log_mbfid, msg, tmout);

	if(sz < 0) return sz;
	if(sz < (INT)(2 * sizeof(UW))) return E_IO;	/* 壊れた長さ */
	return sz - (INT)(2 * sizeof(UW));
}

EXPORT void log_note_done(UW t_send)
{
	UW	us = trace_cyc_to_us((UW)(NOW() - t_send));
	UINT	imask;

	DI(imask);
	if(us > lag_max_us) lag_max_us = us;
	EI(imask);
}

EXPORT UW log_sent(void)		{ return n_sent; }
EXPORT UW log_dropped(void)		{ return n_dropped; }
EXPORT UW log_lag_max_us(void)		{ return lag_max_us; }

EXPORT void log_lag_reset_when_idle(void)
{
	lag_reset_pending = TRUE;
}

EXPORT void log_lag_note_idle(void)
{
	UINT	imask;

	if(!lag_reset_pending) return;

	DI(imask);
	lag_reset_pending = FALSE;
	lag_max_us        = 0;
	EI(imask);
}
