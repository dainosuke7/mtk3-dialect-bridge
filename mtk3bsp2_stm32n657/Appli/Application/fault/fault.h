#ifndef FAULT_H
#define FAULT_H

#include <tk/tkernel.h>

/*
 * フォルト可視化機構
 *
 * ねらい: 未実装IRQの発火やフォルト例外で「黙って止まる」のをやめ、
 *         必ず UART に理由を吐いてから停止させる。
 */


EXPORT void app_fault_init(void);

/* ---------------------------------------------------------------- */
/*
 * 受け入れテストの選択スイッチ。usermain() が fault_test_run() を呼ぶ。
 *   0 : テストしない (通常動作)
 *   1 : BusFault    — *(volatile uint32_t*)0xFFFFFFF0 の読み出し
 *   2 : UsageFault  — 1/0
 *   3 : 未実装IRQ   — WWDG_IRQn(19) を pend
 *   4 : UsageFault  — 深い再帰 (STKOF)
 * 1〜4 はいずれも出力後に停止し、戻ってこない。
 */
#define FAULT_TEST	(0)

/* FAULT_TEST の値に応じて下のテストを1本だけ実行する */
EXPORT void fault_test_run(void);

/* ---------------------------------------------------------------- */
/*
 * 受け入れテスト。いずれも「戻ってこない」。
 */

/* 0xFFFFFFF0 を読む → BusFault / PRECISERR / BFAR=0xFFFFFFF0 */
EXPORT void fault_test_busfault(void);

/* 1/0 → UsageFault / DIVBYZERO (CCR.DIV_0_TRP が必要) */
EXPORT void fault_test_divzero(void);

/* 未使用IRQを pend → [UNHANDLED IRQ] IRQn=19 (WWDG_IRQn) */
EXPORT void fault_test_unhandled_irq(void);

/*
 * 深い再帰 → UsageFault / STKOF
 * 呼び出し元タスクのスタックは 2KB 以上を確保しておくこと
 * (詳しい理由は fault_test.c のコメントを参照)。
 */
EXPORT void fault_test_stkof(void);

#endif	/* FAULT_H */
