#ifndef NPU_INFER_TASK_H
#define NPU_INFER_TASK_H

#include <tk/tkernel.h>

/*
 * 推論タスク (優先度15)。NPU ランタイムを持つ唯一のタスク。
 *
 * 今の中身 (Phase 1 タスク7): NPU の初期化と自己テストの後、タップリングから窓を取り出し、
 * 窓の番号・音量・前の窓とのつなぎ目を確かめて表示する。
 * タスク9 で、取り出した窓に前処理 (log-mel) と推論を載せる。
 */

/*
 * 推論タスクを作って起動し、NPU の初期化と自己テストが終わるまで待つ。
 * usermain から、tap_ring_init() と npu_hw_init() の後・音声を始める前に1回だけ呼ぶ。
 * 戻り値: E_OK 自己テストまで終わった (結果は UART) / E_TMOUT 待ちの上限を超えた /
 *         その他 タスクを作れなかった
 */
EXPORT ER infer_task_start(void);

#endif	/* NPU_INFER_TASK_H */
