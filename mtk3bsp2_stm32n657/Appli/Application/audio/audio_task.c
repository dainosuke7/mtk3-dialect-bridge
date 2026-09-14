#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <math.h>
#include <string.h>
#include "main.h"	// SAI_HandleTypeDef, SAI1_Block_A, MDF1_Filter0, SCB_*DCache_by_Addr
#include "wm8904.h"
#include "sai_io.h"
#include "mdf_io.h"
#include "pcm_fifo.h"
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
/* TODO: 動作確認用に音量を上げている。確認できたらWM8904_TEST_VOLUME=20
 * 程度に戻すこと。パススルー中はマイク→ヘッドホンの経路ができるため、
 * ヘッドホンをマイクに近づけるとハウリングする可能性がある。 */
#define WM8904_TEST_VOLUME	(70U)	// 0-100

/* ------------------------------------------------------------------ */
/* SAI 出力 (WM8904 ヘッドホン)                                        */
/* ------------------------------------------------------------------ */

/* 起動確認用の440Hzサイン波テーブル。16000Hz / 400サンプルで11周期 =
 * ちょうど440Hz (16000*11/400 = 440) なので、末尾から先頭に戻っても
 * 位相が繋がる。パススルーに切り替わるまでの間これを鳴らし、
 * 「出力経路は生きている」ことを耳で確認できるようにしている。 */
#define SINE_TABLE_LEN		(400)
#define SINE_CYCLES		(11)
#define SINE_AMPLITUDE		(8192)	// 16bitフルスケールの約1/4(安全のため控えめ)

LOCAL H sine_table[SINE_TABLE_LEN * 2];	// L/Rインターリーブ (=1半分ぶん)

/* SAI DMAダブルバッファ。前半/後半それぞれSAI_HALF_FRAMESフレーム
 * (L/RインターリーブなのでHはその2倍)。
 * このバッファは通常のキャッシュ可能SRAM上にある。write(CPU)->read(GPDMA)
 * の順序をこちらで制御できるので、書くたびにcleanすれば足りる */
#define DMA_HALF_SAMPLES	(SINE_TABLE_LEN * 2)		// 800 (L/R合わせて)
#define DMA_TOTAL_SAMPLES	(DMA_HALF_SAMPLES * 2)		// 1600
#define SAI_HALF_FRAMES		(DMA_HALF_SAMPLES / 2)		// 400 (=25ms @16kHz)
LOCAL H dma_buf[DMA_TOTAL_SAMPLES] __attribute__((aligned(32)));

LOCAL volatile UINT dma_half_count;	// HAL_SAI_TxHalfCpltCallback 呼び出し回数
LOCAL volatile UINT dma_cplt_count;	// HAL_SAI_TxCpltCallback 呼び出し回数

/* ------------------------------------------------------------------ */
/* パススルー (MDFマイク入力 -> SAIヘッドホン出力)                     */
/* ------------------------------------------------------------------ */

/* 生産側(MDF->FIFO)と消費側(FIFO->SAI)でフラグを分ける。1つで兼ねると
 * 「溜まるまで待つ/溜めるのは待ちが終わってから」の循環になる */
LOCAL volatile UB fifo_feed_active;	// 1でMDFコールバックがFIFOへ投入する
LOCAL volatile UB passthrough_active;	// 1でSAIの出力元がサイン波->FIFOになる

LOCAL volatile UW pt_underrun;	// SAI側でFIFOが足りずに無音を埋めた回数
LOCAL volatile UW pt_overrun;	// MDF側でFIFOが満杯で捨てた回数
LOCAL volatile UW mdf_cb_total;	// MDFコールバックの累計回数(リセットしない)
LOCAL volatile UW mdf_err_count;	// HAL_MDF_ErrorCallback の回数

/* SAIコールバック用の作業バッファ。ISRのスタックを消費しないようstatic。
 * SAIの2つのコールバックは同一IRQなので直列化され、共有しても安全。 */
LOCAL H pt_mono[SAI_HALF_FRAMES];

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

/* DMAが再生し終えた半分に次のデータを書き込む */
LOCAL void sai_fill_half(H *half)
{
	UINT	i, got;

	if(passthrough_active) {
		/* マイク入力をFIFOから取り出し、モノラル->L/R複製で書き込む */
		got = pcm_fifo_pop(pt_mono, SAI_HALF_FRAMES);
		if(got < SAI_HALF_FRAMES) {
			for(i = got; i < SAI_HALF_FRAMES; i++) {
				pt_mono[i] = 0;		// 不足分は無音で埋める
			}
			pt_underrun++;
		}
		for(i = 0; i < SAI_HALF_FRAMES; i++) {
			half[2*i]     = pt_mono[i];	// L
			half[2*i + 1] = pt_mono[i];	// R
		}
	} else {
		memcpy(half, sine_table, DMA_HALF_SAMPLES * sizeof(H));
	}

	SCB_CleanDCache_by_Addr((uint32_t*)half, DMA_HALF_SAMPLES * sizeof(H));
}

/* 前半(dma_buf先頭)の再生完了 = 現在後半を再生中 -> 前半に次データを充填 */
void HAL_SAI_TxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
	if(hsai->Instance == SAI1_Block_A) {
		sai_fill_half(&dma_buf[0]);
		dma_half_count++;
	}
}

/* バッファ全体の再生完了(折返し) = 現在前半を再生中 -> 後半に次データを充填 */
void HAL_SAI_TxCpltCallback(SAI_HandleTypeDef *hsai)
{
	if(hsai->Instance == SAI1_Block_A) {
		sai_fill_half(&dma_buf[DMA_HALF_SAMPLES]);
		dma_cplt_count++;
	}
}

/* ------------------------------------------------------------------ */
/* MDF1 (PDMマイク) 入力                                               */
/* ------------------------------------------------------------------ */

/* MDFの出力は32bit(上位24bitに有効データ)。ST公式BSPと同じく/256して
 * 16bitに落とし、飽和させる */
#define MDF_SAT16(v)	(((v) > 32767) ? 32767 : (((v) < -32768) ? -32768 : (v)))

typedef struct {
	W	min;		// 区間内の最小値(16bit換算)
	W	max;		// 区間内の最大値(16bit換算)
	UW	sum_abs;	// 絶対値の合計(平均振幅の算出用)
	UW	samples;	// 加算したサンプル数
	UW	callbacks;	// コールバック呼び出し回数(取りこぼし検出用)
} mic_stat_t;

LOCAL volatile mic_stat_t mic_stat;

/* MDFコールバック用の作業バッファ。SAI側のpt_monoと同じ理由でstatic */
LOCAL H mic_mono[MDF_IN_HALF_SAMPLES];

LOCAL void mic_stat_reset(volatile mic_stat_t *st)
{
	st->min       = 32767;
	st->max       = -32768;
	st->sum_abs   = 0;
	st->samples   = 0;
	st->callbacks = 0;
}

/* DMAが書き終えた半分を16bitに変換し、統計に加算してFIFOへ流す */
LOCAL void mic_process_half(UINT half)
{
	W	*buf;
	UINT	i, pushed;
	W	v;

	buf = mdf_in_buf_half(half);

	/* DMAがRAMに書いた内容をCPUのキャッシュ越しに読むため、読む直前に
	 * 該当範囲を無効化する(このバッファにCPUから書き込むことは無いので、
	 * invalidateでデータを失う心配はない) */
	SCB_InvalidateDCache_by_Addr((uint32_t*)buf, MDF_IN_HALF_SAMPLES * sizeof(W));

	for(i = 0; i < MDF_IN_HALF_SAMPLES; i++) {
		v = buf[i] / 256;
		v = MDF_SAT16(v);
		mic_mono[i] = (H)v;

		if(v < mic_stat.min) mic_stat.min = v;
		if(v > mic_stat.max) mic_stat.max = v;
		mic_stat.sum_abs += (UW)((v < 0) ? -v : v);
		mic_stat.samples++;
	}
	mic_stat.callbacks++;
	mdf_cb_total++;

	if(fifo_feed_active) {
		pushed = pcm_fifo_push(mic_mono, MDF_IN_HALF_SAMPLES);
		if(pushed < MDF_IN_HALF_SAMPLES) pt_overrun++;
	}
}

/* 前半の取り込み完了 = 現在後半に書き込み中 -> 前半を処理 */
void HAL_MDF_AcqHalfCpltCallback(MDF_HandleTypeDef *hmdf)
{
	if(hmdf->Instance == MDF1_Filter0) {
		mic_process_half(0);
	}
}

/* バッファ全体の取り込み完了(折返し) = 現在前半に書き込み中 -> 後半を処理 */
void HAL_MDF_AcqCpltCallback(MDF_HandleTypeDef *hmdf)
{
	if(hmdf->Instance == MDF1_Filter0) {
		mic_process_half(1);
	}
}

/* MDFのエラー(飽和/リシェイプフィルタoverrun等)。通常はmdf_io.cで該当
 * 割り込みをマスクしているので呼ばれないが、weakのまま放置すると
 * Default_Handler(無限ループ)行きになる系統の問題を見落とすので、
 * ハンドラを実体化した上で回数を数えて可視化しておく。 */
void HAL_MDF_ErrorCallback(MDF_HandleTypeDef *hmdf)
{
	if(hmdf->Instance == MDF1_Filter0) {
		mdf_err_count++;
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

/* ------------------------------------------------------------------ */

/* 切り替え前にサイン波を鳴らす時間。出力経路の生存確認を兼ねており、
 * ビープが鳴ってパススルーが無音なら入力側の問題だと切り分けられる。
 * 不要なら0でよい */
#define PT_BEEP_MS		(1000)
#define PT_PREFILL_SAMPLES	(800)	// 50ms分たまってから切り替える
#define PT_REPORT_INTERVAL_MS	(500)
#define PT_REPORT_COUNT		(30)	// 500ms x 30 = 約15秒

LOCAL ER passthrough_test(void)
{
	ER		err;
	UINT		n, waited;
	mic_stat_t	st;
	UW		avg_abs;

	sine_table_build();
	fifo_feed_active   = 0;
	passthrough_active = 0;
	pt_underrun = 0;
	pt_overrun  = 0;
	mdf_cb_total = 0;
	mdf_err_count = 0;
	dma_half_count = 0;
	dma_cplt_count = 0;
	pcm_fifo_reset();
	mic_stat_reset(&mic_stat);

	/* DMA開始前に両バッファをサイン波で埋めておく */
	sai_fill_half(&dma_buf[0]);
	sai_fill_half(&dma_buf[DMA_HALF_SAMPLES]);

	err = wm8904_dac_unmute();
	if(err < E_OK) {
		tm_printf((UB*)"WM8904 DAC unmute FAIL (err=%d)\n", err);
		return err;
	}
	tm_printf((UB*)"WM8904 DAC unmute OK\n");

	/* 出力経路の確認を兼ねて、まずサイン波を鳴らす */
	err = sai_out_transmit_dma(dma_buf, DMA_TOTAL_SAMPLES);
	if(err < E_OK) {
		tm_printf((UB*)"SAI1 DMA start FAIL (err=%d)\n", err);
		return err;
	}
	tm_printf((UB*)"SAI1 DMA start OK (440Hz sine, output path check)\n");
	tk_dly_tsk(PT_BEEP_MS);

	/* マイク取り込み開始。FIFOへの投入はここから有効にしておく
	 * (出力元の切り替えはFIFOが十分たまってから) */
	fifo_feed_active = 1;
	err = mdf_in_start_dma();
	if(err < E_OK) {
		tm_printf((UB*)"MDF1 acquisition start FAIL (err=%d)\n", err);
		fifo_feed_active = 0;
		(void)sai_out_stop_dma();
		return err;
	}
	tm_printf((UB*)"MDF1 acquisition start OK\n");

	for(waited = 0; waited < 100; waited++) {	// 最大1秒待つ
		if(pcm_fifo_count() >= PT_PREFILL_SAMPLES) break;
		tk_dly_tsk(10);
	}
	if(pcm_fifo_count() < PT_PREFILL_SAMPLES) {
		/* mdf_cb=0ならMDFのDMA/割り込みが動いていない。
		 * mdf_cb>0なのにfifoが増えないならFIFO投入側の問題。 */
		tm_printf((UB*)"Passthrough prefill TIMEOUT (fifo=%u mdf_cb=%u over=%u)\n",
				pcm_fifo_count(), mdf_cb_total, pt_overrun);
		fifo_feed_active = 0;
		(void)mdf_in_stop_dma();
		(void)sai_out_stop_dma();
		return E_TMOUT;
	}

	passthrough_active = 1;
	tm_printf((UB*)"Passthrough ACTIVE (mic -> headphone, prefill=%u samples, mdf_cb=%u)\n",
			pcm_fifo_count(), mdf_cb_total);
	tm_printf((UB*)"Speak into the onboard mics; you should hear it and avg_abs should rise.\n");

	for(n = 0; n < PT_REPORT_COUNT; n++) {
		tk_dly_tsk(PT_REPORT_INTERVAL_MS);
		mic_stat_take(&st);
		if(st.samples == 0) {
			tm_printf((UB*)"  pt[%d]: no mic data (mdf_cb=%u)\n", n, mdf_cb_total);
			continue;
		}
		avg_abs = st.sum_abs / st.samples;
		tm_printf((UB*)"  pt[%d]: min=%d max=%d avg_abs=%u | fifo=%u under=%u over=%u mdf_cb=%u\n",
				n, st.min, st.max, avg_abs,
				pcm_fifo_count(), pt_underrun, pt_overrun, mdf_cb_total, mdf_err_count);
	}

	passthrough_active = 0;
	fifo_feed_active   = 0;

	err = mdf_in_stop_dma();
	if(err < E_OK) tm_printf((UB*)"MDF1 acquisition stop FAIL (err=%d)\n", err);

	err = sai_out_stop_dma();
	if(err < E_OK) tm_printf((UB*)"SAI1 DMA stop FAIL (err=%d)\n", err);

	tm_printf((UB*)"Passthrough stopped (sai_cb half=%u cplt=%u, mdf_cb=%u, under=%u over=%u)\n",
			dma_half_count, dma_cplt_count, mdf_cb_total, mdf_err_count, pt_underrun, pt_overrun);
	return E_OK;
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

		/* SAI1本体・Tx DMAチャネルはmain.cのMX_SAI1_Init()で初期化済み */
		err = sai_out_init_check();
		if(err < E_OK) {
			tm_printf((UB*)"SAI1 peripheral init FAIL (see main.c MX_SAI1_Init)\n");
			break;
		}
		tm_printf((UB*)"SAI1 peripheral init OK (kerclk=%u mckdiv=%u -> Fs=%uHz)\n",
				sai_out_kernel_clock(), sai_out_mckdiv(),
				(sai_out_mckdiv() != 0)
					? (sai_out_kernel_clock() / (sai_out_mckdiv() * 256))
					: 0);

		/* MDF1本体・Rx DMAチャネルはmain.cのMX_MDF1_Init()で初期化済み */
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

		(void)passthrough_test();
	} while(0);

	tk_ext_tsk();
}

EXPORT void audio_task_start(void)
{
	tskid_audio = tk_cre_tsk(&ctsk_audio);
	tk_sta_tsk(tskid_audio, 0);
}
