#ifndef NPU_NPU_RT_H
#define NPU_NPU_RT_H

#include <tk/tkernel.h>

/*
 * NPU 推論ランタイム (ST Edge AI の ll_aton + stai API) の起動と、推論1回分の実行枠
 * (Phase 1 タスク5)
 *
 * ランタイムとモデルは st/ 以下 (ST のファイルを無改変で同梱。st/LICENSE.md)。
 *   動作モード: LL_ATON_PLATFORM=STM32N6 / OSAL=BARE_METAL / RT_MODE=POLLING / SW_FALLBACK
 *   (定義は .cproject のプリプロセッサ定義)
 *   モデル: AED (yamnet_1024_64x96_tl_qdq_int8)。入力 int8 1x64x96x1、出力 float32 x10
 *   重み: 外部フラッシュ 0x70180000〜 (extflash_init でメモリマップ済みであること)
 *   作業領域: AXISRAM6 0x34350000〜 (144KB。入力・出力もこの中。番地は network.c に固定)
 *
 * NPU の割り込みは使わない。ll_aton は POLLING でも stai_runtime_init() の中で
 * NPU0_IRQn (エラー通知用) を NVIC 有効にするので、npu_rt_init() が直後に無効へ戻す。
 */

#define NPU_RT_IN_BYTES		(6144)	/* int8 1x64x96x1 (メル64 x フレーム96、フレームが内側) */
#define NPU_RT_OUT_CLASSES	(10)	/* float32 x10 (softmax 後) */

/*
 * 推論1回の上限。ll_aton のポーリング待ち (LL_Streng_Wait) の中で DWT CYCCNT により判定する。
 * 推論は数 ms の見込み。値はタスク6 で実測して詰める
 */
#define NPU_RT_TIMEOUT_US	(100000)

/*
 * ランタイムとネットワークを初期化し、各ステップの結果を UART に出す。推論はしない。
 * 戻り値: E_OK 成功 / E_OBJ npu_hw_init() が済んでいない / E_IO ランタイムの初期化失敗
 *
 * - npu_hw_init() が E_OK を返した後に1回だけ呼ぶ。NPU にクロックが入っていないと
 *   ll_aton の初期化 (LL_ATON_Init) が NPU のバージョンを読み続けて戻らないため、
 *   npu_hw_ready() でなければ何もしない
 * - 失敗しても Error_Handler() は呼ばない。呼び出し側は結果を表示して続行してよい
 */
EXPORT ER npu_rt_init(void);

/* npu_rt_init() が成功したか */
EXPORT BOOL npu_rt_ready(void);

/*
 * 入力・出力バッファ (AXISRAM6 内。ランタイムが決める番地)。準備できていなければ NULL。
 * 入力を書いたら npu_rt_run() の前に D キャッシュを clean+invalidate する
 * (入力の領域は推論中に中間結果と出力の置き場として再利用される)
 */
EXPORT B *npu_rt_input(void);
EXPORT const float *npu_rt_output(void);

/*
 * 推論を1回実行し、終わるまで待つ (ポーリング。呼び出したタスクがその間 CPU を使い続ける)。
 * 戻り値: E_OK 完了 / E_OBJ 未初期化 / E_IO ランタイムがエラーを返した・前回タイムアウトして
 *         後始末が済んでいない / E_TMOUT NPU_RT_TIMEOUT_US を超えた
 *
 * タイムアウトしたら TRACE (EV_INF_TMO) とカウンタに残し、UART に理由を出して E_TMOUT で戻る
 * (assert では止めない)。ll_aton の内部状態は推論の途中のままなので、それ以降の呼び出しは
 * E_IO を返す。実行中 epoch の中断などの後始末はタスク6 で実装する。
 *
 * 推論の実行はタスク6 から。タスク5 では呼び出し元が無い。
 */
EXPORT ER npu_rt_run(void);

/* npu_rt_run() がタイムアウトした回数 */
EXPORT UW npu_rt_timeouts(void);

#endif	/* NPU_NPU_RT_H */
