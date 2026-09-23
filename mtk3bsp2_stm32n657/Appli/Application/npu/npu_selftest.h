#ifndef NPU_NPU_SELFTEST_H
#define NPU_NPU_SELFTEST_H

#include <tk/tkernel.h>

/*
 * 固定入力による NPU 推論の確認 (Phase 1 タスク6)
 *
 * どちらも推論タスク (infer_task.c) から、npu_rt_init() が成功した後に呼ぶ。
 * ll_aton を呼ぶのは推論タスクだけ (理由は infer_task.c の先頭)。
 */

/* 乱数入力の出力が最初の推論とどれだけ違ってよいか (x1e-4、パススルー中の予行の判定に使う) */
#define NPU_SELFTEST_TOL_X1E4	(500)

/*
 * 自己テスト: 乱数入力 (参考値)・ロジット (参考値)・ESC-10 の実録音 (判定)・10回連続。結果は UART。
 * 戻り値: 最初の推論が完了したら TRUE (出力が期待値と合わなくても)。
 *         TRUE のときだけ npu_selftest_run_fixed() が使える
 */
EXPORT BOOL npu_selftest(void);

/*
 * 乱数入力で1回推論し、npu_selftest() の最初の推論の出力と比べる。
 * *us: 推論時間、*diff_x1e4: 各クラスの差の最大値 (x1e-4)、*same: ビット単位で同じ
 * 戻り値: E_OK / E_OBJ npu_selftest() の推論が済んでいない / その他 npu_rt_run() の戻り値
 */
EXPORT ER npu_selftest_run_fixed(UW *us, INT *diff_x1e4, BOOL *same);

#endif	/* NPU_NPU_SELFTEST_H */
