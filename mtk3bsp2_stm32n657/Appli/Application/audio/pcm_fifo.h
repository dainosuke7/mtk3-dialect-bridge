#ifndef AUDIO_PCM_FIFO_H
#define AUDIO_PCM_FIFO_H

#include <tk/tkernel.h>

/*
 * モノラル16bit PCMのリングバッファ(SPSC: 単一生産者・単一消費者)。
 *
 * MDF入力(256サンプル/16ms)とSAI出力(400フレーム/25ms)はレートこそ
 * 同じ16kHzだがチャンクサイズが違うため、その差を吸収する。
 *
 * 生産者はheadのみ、消費者はtailのみ更新するので排他不要。
 */

/*
 * Phase 2 の注意: 消費者を増やす場合はこのままでは使えない。tail を
 * 更新するのが1者である前提なので、推論タスクが1秒窓を読む用途には
 * 別のリング(音声タスクが並行して push する tap)を用意すること。
 */

/* 容量。2の冪であること(マスクで剰余を取るため)。16kHzで128ms分 */
#define PCM_FIFO_CAPACITY	(2048)

EXPORT void pcm_fifo_reset(void);

/* 現在たまっているサンプル数 */
EXPORT UINT pcm_fifo_count(void);

/* src から n サンプル書き込む。実際に書けた数を返す(満杯なら n 未満) */
EXPORT UINT pcm_fifo_push(const H *src, UINT n);

/* dst へ n サンプル取り出す。実際に取れた数を返す(不足なら n 未満) */
EXPORT UINT pcm_fifo_pop(H *dst, UINT n);

#endif	/* AUDIO_PCM_FIFO_H */
