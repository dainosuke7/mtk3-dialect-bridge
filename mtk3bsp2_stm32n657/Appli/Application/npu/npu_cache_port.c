#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "main.h"		// CMSIS (CACHEAXI)
#include "npu_cache.h"		// st/device/ (ST の宣言をそのまま使う)
#include "../trace/trace.h"	// NOW(), trace_cyc_per_us()

/*
 * NPU キャッシュ (CACHEAXI) の保守関数。ST の npu_cache.h の宣言どおりの関数をここで用意する。
 *
 * ST 版の npu_cache.c (Middlewares/ST/AI/Npu/Devices/STM32N6xx/) は HAL_CACHEAXI を使うが、
 * このプロジェクトの Drivers/ には HAL_CACHEAXI が無い (npu_hw.c と同じ事情)。
 * そこで npu_cache.c は同梱せず、同じ手順をレジスタ操作で書き起こした。
 * 手順は ST の HAL (STM32N6-GettingStarted-Audio v2.3.0, stm32n6xx_hal_cacheaxi.c) と同じ:
 *   npu_cache_invalidate            HAL_CACHEAXI_Invalidate()      :475-537
 *   npu_cache_clean_range 等        CACHEAXI_CommandByAddr()       :1370-1446
 *   npu_cache_disable               HAL_CACHEAXI_Disable()         :404-456
 * ST 版との違い:
 *   - 完了待ちの上限は HAL_GetTick() でなく DWT CYCCNT で測る (カーネル起動後の HAL ティックは
 *     10ms 刻み)。上限は HAL と同じ (コマンド 200ms、無効化 1ms)
 *   - npu_cache_enable はクロック・リセットから始めない。それは npu_hw_init() が済ませている
 *     (npu_hw.c の npu_cache_enable() と同じく BUSYF の解除を待ってから CR1.EN を立てる)
 *   - 宣言が void なので失敗は返せない。UART に出すだけにする
 *
 * 呼ばれる場面: NPU キャッシュを通るバッファを CPU が書き換えたとき (ll_aton_caches_interface.h)。
 * AED の network.c は呼ばない (NPU キャッシュを通るのは読み出し専用の重みだけ)。
 * ll_aton_stai_internal.c の stai_ext_cache_npu_*() から参照されている。
 */

#define CACHEAXI_CMD_CLEAN		(CACHEAXI_CR2_CACHECMD_0)				/* HAL: CACHEAXI_COMMAND_CLEAN */
#define CACHEAXI_CMD_CLEAN_INVALIDATE	(CACHEAXI_CR2_CACHECMD_0 | CACHEAXI_CR2_CACHECMD_1)	/* HAL: ..._CLEAN_INVALIDATE */

#define CACHEAXI_CMD_WAIT_US		(200000)	/* HAL: CACHEAXI_COMMAND_TIMEOUT_VALUE 200ms */
#define CACHEAXI_DISABLE_WAIT_US	(1000)		/* HAL: CACHEAXI_DISABLE_TIMEOUT_VALUE 1ms */

/* CYCCNT が進まない場合でも抜けるための読み出し回数の上限 (npu_hw.c と同じ考え方) */
#define CACHEAXI_WAIT_MAX_READS		(1000000)

/* SR の mask のビットが want (0 か mask) になるまで待つ。なれば TRUE */
LOCAL BOOL wait_sr(UW mask, UW want, UW us)
{
	UW	t0  = NOW();
	UW	lim = trace_cyc_per_us() * us;
	INT	reads = 0;

	while((CACHEAXI->SR & mask) != want) {
		if((UW)(NOW() - t0) > lim || ++reads >= CACHEAXI_WAIT_MAX_READS) {
			return (CACHEAXI->SR & mask) == want;
		}
	}
	return TRUE;
}

LOCAL BOOL busy(void)
{
	return (CACHEAXI->SR & (CACHEAXI_SR_BUSYF | CACHEAXI_SR_BUSYCMDF)) != 0;
}

/* 範囲指定のコマンド (clean / clean+invalidate)。end は範囲の直後の番地 */
LOCAL void command_by_addr(const char *name, UW cmd, uint32_t start, uint32_t end)
{
	if(start >= end) return;	/* ST 版と同じ */

	if(busy()) {
		tm_printf((UB*)"[npu_cache] %s 0x%08x-0x%08x: busy, skipped\n", name, start, end);
		return;
	}
	CACHEAXI->FCR = CACHEAXI_FCR_CBSYENDF | CACHEAXI_FCR_CCMDENDF;
	CACHEAXI->CMDRSADDRR = start;
	CACHEAXI->CMDREADDRR = end - 1U;
	CACHEAXI->CR2 = (CACHEAXI->CR2 & ~CACHEAXI_CR2_CACHECMD) | cmd;
	CACHEAXI->IER &= ~CACHEAXI_IER_CMDENDIE;
	CACHEAXI->CR2 |= CACHEAXI_CR2_STARTCMD;

	if(!wait_sr(CACHEAXI_SR_CMDENDF, CACHEAXI_SR_CMDENDF, CACHEAXI_CMD_WAIT_US)) {
		tm_printf((UB*)"[npu_cache] %s 0x%08x-0x%08x: timeout (SR=0x%08x)\n",
				name, start, end, CACHEAXI->SR);
	}
}

void npu_cache_enable(void)
{
	if(CACHEAXI->CR1 & CACHEAXI_CR1_EN) return;

	/* リセット直後の全無効化が終わってから有効にする */
	if(!wait_sr(CACHEAXI_SR_BUSYF, 0, CACHEAXI_CMD_WAIT_US)) {
		tm_printf((UB*)"[npu_cache] enable: busy (SR=0x%08x)\n", CACHEAXI->SR);
		return;
	}
	CACHEAXI->CR1 |= CACHEAXI_CR1_EN;
}

void npu_cache_disable(void)
{
	if((CACHEAXI->CR1 & CACHEAXI_CR1_EN) == 0) return;

	CACHEAXI->CR1 &= ~CACHEAXI_CR1_EN;
	if(!wait_sr(CACHEAXI_SR_BUSYF | CACHEAXI_SR_BUSYCMDF, 0, CACHEAXI_DISABLE_WAIT_US)) {
		tm_printf((UB*)"[npu_cache] disable: timeout (SR=0x%08x)\n", CACHEAXI->SR);
	}
}

void npu_cache_invalidate(void)
{
	if(busy()) {
		tm_printf((UB*)"[npu_cache] invalidate: busy, skipped\n");
		return;
	}
	CACHEAXI->FCR = CACHEAXI_FCR_CBSYENDF | CACHEAXI_FCR_CCMDENDF;
	CACHEAXI->CR2 &= ~CACHEAXI_CR2_CACHECMD;
	CACHEAXI->CR1 |= CACHEAXI_CR1_CACHEINV;

	if(!wait_sr(CACHEAXI_SR_BUSYF, 0, CACHEAXI_CMD_WAIT_US)) {
		tm_printf((UB*)"[npu_cache] invalidate: timeout (SR=0x%08x)\n", CACHEAXI->SR);
	}
}

void npu_cache_clean_range(uint32_t start_addr, uint32_t end_addr)
{
	command_by_addr("clean", CACHEAXI_CMD_CLEAN, start_addr, end_addr);
}

void npu_cache_clean_invalidate_range(uint32_t start_addr, uint32_t end_addr)
{
	command_by_addr("clean+invalidate", CACHEAXI_CMD_CLEAN_INVALIDATE, start_addr, end_addr);
}
