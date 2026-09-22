#ifndef NPU_NPU_HW_H
#define NPU_NPU_HW_H

#include <tk/tkernel.h>

/*
 * NPU を動かす前提となるハードウェアの初期化 (Phase 1 タスク3・4)
 *
 *   1. 内部メモリ: AXISRAM3〜6 と NPU キャッシュ用 RAM のクロック・電源
 *   2. NPU とNPU キャッシュ (CACHEAXI) のクロック・リセット・有効化
 *   3. RIF: NPU を CID1・セキュア・特権のバスマスタにし、
 *      NPU のレジスタをセキュア・特権からだけ触れるようにする
 *
 * 推論ランタイム (ll_aton, network.c) はまだ入れていない。
 * IC6 (NPU) / IC11 (NPU RAM) のクロックは FSBL の設定を読んで表示するだけで、変えない。
 */

#define NPU_AXISRAM3_BASE	((UW)0x34200000)	/* AXISRAM3 (448KB)。未使用 */
#define NPU_AXISRAM6_BASE	((UW)0x34350000)	/* AXISRAM6 (448KB)。NPU の activations */

/*
 * 初期化して、各ステップの結果を UART に出す。
 * 戻り値: E_OK 全ステップ成功 / E_IO 失敗あり (どれかは UART の [FAIL] / [SKIP] 行)
 *
 * - カーネル起動後に usermain から1回だけ呼ぶ (特権・セキュアで動いている必要がある)
 * - 音声の DMA を始める前 (audio_task_start より前) に呼ぶ。
 *   RAM の読み書き確認のあいだ D キャッシュを止めるため
 * - 失敗しても Error_Handler() は呼ばない。呼び出し側は結果を表示して続行してよい
 */
EXPORT ER npu_hw_init(void);

/* NPU を使える状態になったか。Phase 2 でランタイムを起動する前に確認する */
EXPORT BOOL npu_hw_ready(void);

#endif	/* NPU_NPU_HW_H */
