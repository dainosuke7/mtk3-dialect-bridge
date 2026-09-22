#ifndef NPU_NPU_SELFTEST_H
#define NPU_NPU_SELFTEST_H

#include <tk/tkernel.h>

/*
 * NPU タスク (Phase 1 タスク6)
 *
 * 推論ランタイムを初期化し、固定入力 (aed_test_input.h) で推論して PC (ONNX Runtime) の
 * 期待値と比べる。NPU_PT_TEST (npu_selftest.c) が 1 なら、さらにパススルー稼働中にも
 * 推論を繰り返して音声が乱れないかを見る。
 *
 * ll_aton を呼ぶのはこのタスクだけ。理由は npu_selftest.c の先頭。
 */

/*
 * NPU タスクを作って起動し、自己テスト (初期化・1回の比較・10回連続) が終わるまで待つ。
 * usermain から、npu_hw_init() の後・音声を始める前に1回だけ呼ぶ。
 * 戻り値: E_OK 自己テストまで終わった (結果の PASS/FAIL は UART) / E_TMOUT 待ちの上限を超えた /
 *         その他 タスクを作れなかった
 * 失敗しても Error_Handler() は呼ばない。呼び出し側は結果を表示して続行してよい。
 */
EXPORT ER npu_task_start(void);

#endif	/* NPU_NPU_SELFTEST_H */
