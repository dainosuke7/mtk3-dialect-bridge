#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <math.h>
#include <string.h>
#include "main.h"	// SAI_HandleTypeDef, SAI1_Block_A, SCB_CleanDCache_by_Addr (DMAコールバック用)
#include "wm8904.h"
#include "sai_io.h"
#include "mdf_io.h"
#include "audio_task.h"

LOCAL void task_audio(INT stacd, void *exinf);	// task execution function
LOCAL ID	tskid_audio;			// Task ID number
LOCAL T_CTSK ctsk_audio = {			// Task creation information
	.itskpri	= 10,
	.stksz		= 1024,
	.task		= task_audio,
	.tskatr		= TA_HLNG | TA_RNG3,
};

#define WM8904_EXPECT_ID	(0x8904U)
/* TODO: 動作確認用に音量/再生時間を一時的に上げている。確認できたら
 * WM8904_TEST_VOLUME=20, DMA_PLAY_SECONDS=1 程度に戻すこと。 */
#define WM8904_TEST_VOLUME	(70U)	// 0-100
#define DMA_PLAY_SECONDS	(10)

/* 440Hz サイン波テーブル。16000Hz / 400サンプルで11周期 = ちょうど440Hz
 * (16000*11/400 = 440) になるよう選んだサンプル数なので、末尾から先頭に
 * 戻っても位相が繋がりループ再生できる。 */
#define SINE_SAMPLE_RATE	(16000)
#define SINE_TABLE_LEN		(400)
#define SINE_CYCLES		(11)
#define SINE_AMPLITUDE		(8192)	// 16bitフルスケールの約1/4(安全のため控えめ)

LOCAL H sine_table[SINE_TABLE_LEN * 2];	// L/Rインターリーブ (=1半分ぶん)

/*
 * DMAダブルバッファ。前半/後半それぞれがsine_table 1個ぶん
 * (SINE_TABLE_LEN*2サンプル)。GPDMAはこのバッファ全体を循環リンクリストで
 * 再生し続けるので、HAL_SAI_TxHalfCpltCallback(前半再生完了=後半再生中)/
 * HAL_SAI_TxCpltCallback(後半再生完了=前半再生中、折返し)で、今再生して
 * いない方の半分に次データを書き込む。
 *
 * この項目はD-Cacheが有効な通常のSRAM上にあり(MPUのnon-cacheable領域は
 * このプロジェクトには無い)、GPDMAはCPUのキャッシュを介さず直接RAMを
 * 読むので、書き込むたびにSCB_CleanDCache_by_Addr()でRAMへ書き戻す。
 */
#define DMA_HALF_SAMPLES	(SINE_TABLE_LEN * 2)		// 800
#define DMA_TOTAL_SAMPLES	(DMA_HALF_SAMPLES * 2)		// 1600
LOCAL H dma_buf[DMA_TOTAL_SAMPLES] __attribute__((aligned(32)));

LOCAL volatile UINT dma_half_count;	// HAL_SAI_TxHalfCpltCallback 呼び出し回数
LOCAL volatile UINT dma_cplt_count;	// HAL_SAI_TxCpltCallback 呼び出し回数

LOCAL void sine_table_build(void)
{
	UINT	i;
	float	theta;
	H	v;

	for(i = 0; i < SINE_TABLE_LEN; i++) {
		theta = 2.0f * 3.14159265f * SINE_CYCLES * (float)i / (float)SINE_TABLE_LEN;
		v = (H)(SINE_AMPLITUDE * sinf(theta));
		sine_table[2*i]     = v;	// L
		sine_table[2*i + 1] = v;	// R
	}
}

LOCAL void dma_fill_half(H *half)
{
	memcpy(half, sine_table, DMA_HALF_SAMPLES * sizeof(H));
	SCB_CleanDCache_by_Addr((uint32_t*)half, DMA_HALF_SAMPLES * sizeof(H));
}

/* 前半(dma_buf先頭)の再生完了 = 現在後半を再生中 -> 前半に次データを充填 */
void HAL_SAI_TxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
	if(hsai->Instance == SAI1_Block_A) {
		dma_fill_half(&dma_buf[0]);
		dma_half_count++;
	}
}

/* バッファ全体の再生完了(折返し) = 現在前半を再生中 -> 後半に次データを充填 */
void HAL_SAI_TxCpltCallback(SAI_HandleTypeDef *hsai)
{
	if(hsai->Instance == SAI1_Block_A) {
		dma_fill_half(&dma_buf[DMA_HALF_SAMPLES]);
		dma_cplt_count++;
	}
}

/* ------------------------------------------------------------------ */
/* MDF1 (PDMマイク) 入力の振幅計測                                     */
/* ------------------------------------------------------------------ */

/*
 * MDFの出力は32bit(上位24bitに有効データ)。ST公式BSPの
 * HAL_MDF_AcqCpltCallback/AcqHalfCpltCallback と同じく /256 して
 * 16bitに落とし、飽和させる。
 */
#define MDF_SAT16(v)	(((v) > 32767) ? 32767 : (((v) < -32768) ? -32768 : (v)))

typedef struct {
	W	min;		// 区間内の最小値(16bit換算)
	W	max;		// 区間内の最大値(16bit換算)
	UW	sum_abs;	// 絶対値の合計(平均振幅の算出用)
	UW	samples;	// 加算したサンプル数
	UW	callbacks;	// コールバック呼び出し回数(取りこぼし検出用)
} mic_stat_t;

LOCAL volatile mic_stat_t mic_stat;

LOCAL void mic_stat_reset(volatile mic_stat_t *st)
{
	st->min       = 32767;
	st->max       = -32768;
	st->sum_abs   = 0;
	st->samples   = 0;
	st->callbacks = 0;
}

/* DMAが書き終えた半分を解析して統計に加算する(コールバックから呼ぶ) */
LOCAL void mic_analyze_half(UINT half)
{
	W	*buf;
	UINT	i;
	W	v;

	buf = mdf_in_buf_half(half);

	/* DMAがRAMに書いた内容をCPUのキャッシュ越しに読むため、
	 * 読む直前に該当範囲を無効化する(このバッファにCPUから書き込む
	 * ことは無いので、invalidateでデータを失う心配はない) */
	SCB_InvalidateDCache_by_Addr((uint32_t*)buf, MDF_IN_HALF_SAMPLES * sizeof(W));

	for(i = 0; i < MDF_IN_HALF_SAMPLES; i++) {
		v = buf[i] / 256;
		v = MDF_SAT16(v);
		if(v < mic_stat.min) mic_stat.min = v;
		if(v > mic_stat.max) mic_stat.max = v;
		mic_stat.sum_abs += (UW)((v < 0) ? -v : v);
		mic_stat.samples++;
	}
	mic_stat.callbacks++;
}

/* 前半の取り込み完了 = 現在後半に書き込み中 -> 前半を解析 */
void HAL_MDF_AcqHalfCpltCallback(MDF_HandleTypeDef *hmdf)
{
	if(hmdf->Instance == MDF1_Filter0) {
		mic_analyze_half(0);
	}
}

/* バッファ全体の取り込み完了(折返し) = 現在前半に書き込み中 -> 後半を解析 */
void HAL_MDF_AcqCpltCallback(MDF_HandleTypeDef *hmdf)
{
	if(hmdf->Instance == MDF1_Filter0) {
		mic_analyze_half(1);
	}
}

/*
 * 統計をスナップショットしてリセットする。コールバック(割り込み)と
 * 競合するのでDI/EIで割り込みを止めた状態で行う。
 */
LOCAL void mic_stat_take(mic_stat_t *out)
{
	UINT	imask;

	DI(imask);
	*out = *(mic_stat_t*)&mic_stat;
	mic_stat_reset(&mic_stat);
	EI(imask);
}

#define MIC_REPORT_INTERVAL_MS	(500)
#define MIC_REPORT_COUNT	(20)	// 500ms x 20 = 約10秒

LOCAL void mic_capture_test(void)
{
	ER		err;
	UINT		n;
	mic_stat_t	st;
	UW		avg_abs;

	mic_stat_reset(&mic_stat);

	err = mdf_in_start_dma();
	if(err < E_OK) {
		tm_printf((UB*)"MDF1 acquisition start FAIL (err=%d)\n", err);
		return;
	}
	tm_printf((UB*)"MDF1 acquisition start OK (16kHz, double buffer %d+%d samples)\n",
			MDF_IN_HALF_SAMPLES, MDF_IN_HALF_SAMPLES);
	tm_printf((UB*)"Speak into the onboard mics; amplitude should rise.\n");

	for(n = 0; n < MIC_REPORT_COUNT; n++) {
		tk_dly_tsk(MIC_REPORT_INTERVAL_MS);
		mic_stat_take(&st);
		if(st.samples == 0) {
			tm_printf((UB*)"  mic[%d]: no data (callbacks=0)\n", n);
			continue;
		}
		avg_abs = st.sum_abs / st.samples;
		tm_printf((UB*)"  mic[%d]: min=%d max=%d avg_abs=%u (n=%u, cb=%u)\n",
				n, st.min, st.max, avg_abs, st.samples, st.callbacks);
	}

	err = mdf_in_stop_dma();
	if(err < E_OK) {
		tm_printf((UB*)"MDF1 acquisition stop FAIL (err=%d)\n", err);
	} else {
		tm_printf((UB*)"MDF1 acquisition stopped\n");
	}
}

LOCAL void task_audio(INT stacd, void *exinf)
{
	ER	err;
	UH	devid;

	do {
		err = wm8904_read_device_id(&devid);
		if(err < E_OK) {
			tm_printf((UB*)"WM8904 I2C2 read error = %d\n", err);
			break;
		}
		tm_printf((UB*)"WM8904 Device ID = 0x%04X\n", devid);
		if(devid != WM8904_EXPECT_ID) {
			tm_printf((UB*)"WM8904 unexpected Device ID (expect 0x%04X)\n", WM8904_EXPECT_ID);
			break;
		}

		err = wm8904_init_headphone_16k(WM8904_TEST_VOLUME);
		if(err < E_OK) {
			tm_printf((UB*)"WM8904 init failed (err=%d)\n", err);
			break;
		}

		/* SAI1本体・Tx DMAチャネル(クロック/GPIO/HAL_SAI_Init/GPDMA)は
		 * main.cのMX_SAI1_Init()で初期化済み。ここでは結果を確認するだけ。 */
		err = sai_out_init_check();
		if(err < E_OK) {
			tm_printf((UB*)"SAI1 peripheral init FAIL (see main.c MX_SAI1_Init)\n");
			break;
		}
		tm_printf((UB*)"SAI1 peripheral init OK\n");

		sine_table_build();
		tm_printf((UB*)"Sine table build OK (440Hz, 16kHz, amp=%d/32768)\n", SINE_AMPLITUDE);

		/* DMA開始前に両バッファを充填しておく */
		dma_fill_half(&dma_buf[0]);
		dma_fill_half(&dma_buf[DMA_HALF_SAMPLES]);
		dma_half_count = 0;
		dma_cplt_count = 0;

		err = wm8904_dac_unmute();
		if(err < E_OK) {
			tm_printf((UB*)"WM8904 DAC unmute FAIL (err=%d)\n", err);
			break;
		}
		tm_printf((UB*)"WM8904 DAC unmute OK\n");

		tm_printf((UB*)"Sine wave DMA playback start (440Hz, double buffer, ~%ds)\n", DMA_PLAY_SECONDS);
		err = sai_out_transmit_dma(dma_buf, DMA_TOTAL_SAMPLES);
		if(err < E_OK) {
			tm_printf((UB*)"Sine wave DMA playback start FAIL (err=%d)\n", err);
			break;
		}

		tk_dly_tsk(DMA_PLAY_SECONDS * 1000);

		err = sai_out_stop_dma();
		if(err < E_OK) {
			tm_printf((UB*)"Sine wave DMA playback stop FAIL (err=%d)\n", err);
		}
		/* 期待値の目安: half-complete ≈ DMA_PLAY_SECONDS*1000/25,
		 * complete ≈ DMA_PLAY_SECONDS*1000/50 (どちらも大きく下回って
		 * いなければ、再生中に途切れ(アンダーラン)は起きていない) */
		tm_printf((UB*)"Sine wave DMA playback stopped (half-complete=%u, complete=%u)\n",
				dma_half_count, dma_cplt_count);

		/* MDF1(オンボードPDMマイク)は今回、初期化(main.cのMX_MDF1_Init())
		 * がエラーなく通ったかどうかの確認まで。フィルタ設定・DMA・
		 * データ取得は次のステップ。 */
		err = mdf_in_init_check();
		if(err < E_OK) {
			/* step: 1=RCC_OscConfig(PLL3), 2=RCCEx_PeriphCLKConfig(IC8),
			 * 3=GPIO設定, 4=HAL_MDF_Init, 5=Rx DMA設定.
			 * hal_status: HAL_OK=0, HAL_ERROR=1, HAL_BUSY=2, HAL_TIMEOUT=3 */
			tm_printf((UB*)"MDF1 (PDM mic) init FAIL at step=%u hal_status=%u\n",
					mdf_in_init_step(), mdf_in_init_hal_status());
			break;
		}
		tm_printf((UB*)"MDF1 (PDM mic) init OK (step=%u)\n", mdf_in_init_step());

		mic_capture_test();
	} while(0);

	tk_ext_tsk();
}

EXPORT void audio_task_start(void)
{
	tskid_audio = tk_cre_tsk(&ctsk_audio);
	tk_sta_tsk(tskid_audio, 0);
}
