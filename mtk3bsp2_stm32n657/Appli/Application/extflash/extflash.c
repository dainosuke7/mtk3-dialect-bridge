#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "main.h"			// HAL (HAL_XSPI, RCC, GPIO, PWR)
#include "extflash.h"
#include "mx66uw1g45g/mx66uw1g45g.h"	// ST のコンポーネントドライバ (無改変)

/*
 * 外部 NOR フラッシュ MX66UW1G45G を DTR-OPI でメモリマップする。
 *
 * 手順は STM32N6-GettingStarted-Audio v2.3.0 の Ext_Mem_Config()
 * (Projects/GS/Src/audio_bm.c:834-853) の呼び出し
 *   BSP_XSPI_NOR_Init(0, {OPI, DTR}) -> BSP_XSPI_NOR_EnableMemoryMappedMode(0)
 *   -> XSPI2->CR の NOPREF を立てる (ST の hotfix)
 * と同じ。BSP 側 (Drivers/BSP/STM32N6570-DK/stm32n6570_discovery_xspi.c の
 * BSP_XSPI_NOR_Init / MX_XSPI_NOR_Init / XSPI_NOR_MspInit /
 * XSPI_NOR_ResetMemory / XSPI_NOR_EnterDOPIMode /
 * BSP_XSPI_NOR_EnableMemoryMappedMode) は、LCD/audio 等を含む BSP 一式を
 * 持ち込まないよう、この用途に要る部分だけをここに書き起こした。
 * フラッシュへのコマンドは ST のコンポーネントドライバ (mx66uw1g45g/) を
 * そのまま使う。
 *
 * ST 版との違い:
 *   1. SCLK。ST 版は OTP ヒューズ HSLV_VDDIO3 を焼いて (fuse_vddio()) I/O を
 *      1.8V 高速モードにした上で、初期化を 50MHz で行い、最後にプリスケーラを
 *      外して (HAL_XSPI_SetClockPrescaler(0)) 200MHz で動かす。本機はヒューズを
 *      焼かないので、プリスケーラを外さず、初期化からメモリマップまで同じ
 *      SCLK (EXTFLASH_PRESCALER) で通す。fuse_vddio() は移植しない。
 *   2. XSPI2 のカーネルクロックは ST 版と同じ IC3 (EXTFLASH_KERCLK_IC3=1)。
 *      ST 版は PLL1 800MHz / 4、本機は FSBL が設定した PLL1 1200MHz / 6 で、
 *      どちらも 200MHz。FSBL と同じ HCLK (これも 200MHz) を選ぶと、命令だけの
 *      コマンドでも SR.BUSY が落ちず、全コマンドが TIMEOUT する (下の「切り分け」)。
 *      IC3 は他で使っていないので、音声用の PLL2->IC7 (SAI1) /
 *      PLL3->IC8 (MDF1) とは干渉しない。
 *   3. FSBL が XSPI2 を触った状態で起動するので、最初に RCC で XSPI2 を
 *      リセットする (ST 版も MspInit でリセットしている)。
 *   4. 待ちは HAL_Delay ではなく tk_dly_tsk。
 *   5. ST の EnterDOPIMode は CR2 書き込み後に HAL_XSPI_Init を呼び直して
 *      MemoryType を Macronix にしているが、この HAL の HAL_XSPI_Init は
 *      State が RESET のときしかレジスタを触らない (呼んでも何もしない)。
 *      最初の HAL_XSPI_Init で Macronix を指定しておき、呼び直しは省く。
 *   6. MemorySize は実容量どおり HAL_XSPI_SIZE_1GB (1Gbit = 2^(26+1) バイト)。
 *      ST の BSP は POSITION_VAL(128MB) = 27 (2Gbit 相当) を渡している。
 *   7. リセット直後に SPI モードで JEDEC ID を読み、製造者 ID が Macronix
 *      (0xC2) であることを確かめる (FSBL の失敗が ManuID=0 だったので、
 *      同じ所で躓いていないかを最初に見えるようにする)。
 *
 * 切り分け (Reset memory が全モードで TIMEOUT した件):
 *   結論: 原因は XSPI2 のカーネルクロック。HCLK では SR.BUSY が落ちず、
 *   IC3 に替えたら動いた (2026-09-21 実機)。以下は切り分けの記録。
 *   HAL_XSPI_Command はデータ相の無いコマンドでは TCF ではなく SR.BUSY が
 *   落ちるのを待つ。したがって TIMEOUT は「XSPI2 が BUSY のまま戻らない」
 *   状態で、フラッシュの応答は関係ない (コマンド送出だけなら相手が居なくても
 *   完了する)。MCU 側の XSPI2 周辺 (カーネルクロック、XSPIM の調停、
 *   XSPI PHY、電源) を疑う。
 *   - extflash_dump_regs(): PWR/RCC/XSPIM/XSPI2/GPION を初期化前・初期化後・
 *     失敗時に UART に出す
 *   - slow_spi_test(): 失敗時に XSPI2 を RCC リセットし、プリスケーラ最大
 *     (255, SCLK = カーネルクロック/256 ≒ 0.78MHz) の 1 線 SPI で JEDEC ID を
 *     読む。これも TIMEOUT なら信号品質ではなく設定の問題と確定する
 *   - ST 版 / BSP2 FSBL との比較で欠けていた設定を EXTFLASH_FIX_* で追加
 *     (OTP ヒューズを焼く処理は入れない)
 *   - EXTFLASH_PAD_TEST=1 で、失敗時に VDDIO3 監視・PN6 パッドの GPIO 折り返し・
 *     DLL 校正値とフリーランクロックの確認も行う (通常は 0)
 */

/*
 * SCLK = XSPI2 カーネルクロック / (EXTFLASH_PRESCALER + 1)
 *      = IC3 200MHz / (3 + 1) = 50MHz
 * IC3 = PLL1 1200MHz / 6。分周比は起動時に PLL1 の実周波数から 200MHz に
 * なるよう計算する (xspi2_hw_setup)。PLL1 は FSBL の SystemClock_Config() が
 * 設定した値をそのまま使っている (アプリは PLL1 を変えない)。
 * 起動ログに実際の値を出す。
 *
 * 上げるときの次の候補は 1 (100MHz)。0 (200MHz) は ST 版がヒューズ前提で
 * 使っている値なので使わない。分周比 (EXTFLASH_PRESCALER + 1) は偶数に
 * しておく (奇数分周だと CLK のデューティが 50% にならず、DTR で不利)。
 */
#define EXTFLASH_PRESCALER	(3U)

/*
 * 比較で欠けていた設定 (1 で有効)。決め手は EXTFLASH_KERCLK_IC3。
 * 残りの 3 つが要るかは未確認 (外して動くかは試していない)。
 *
 * EXTFLASH_FIX_VDDIO3_1V8: PWR SVMCR3.VDDIO3VRSEL = 1.8V レンジ。
 *   BSP2 FSBL (main.c USER CODE Init) と ST BSP (XSPI_NOR_MspInit) の両方が
 *   行っている。VDDIO3 (Port N) は基板上 1.8V 給電。OTP ヒューズではなく
 *   PWR の揮発レジスタ。HSLV_VDDIO3 ヒューズが無いと書き込みが無視されるだけ
 *   (LL の注記) なので無害
 * EXTFLASH_FIX_XSPIM_CONFIG: HAL_XSPIM_Config で XSPI2 -> Port2 / NCS1 を明示。
 *   BSP2 FSBL の MX_XSPI2_Init と同じ。これまでは FSBL の設定が残っている
 *   前提で触っていなかった
 * EXTFLASH_FIX_XSPIPHY: XSPI PHY 補償セルのクロック (RCC MISCENR.XSPIPHYCOMPEN)
 *   を有効化し、XSPI PHY2 のリセット (MISCRSTSR.XSPIPHY2RSTS) を解除する。
 *   ST 版はスリープ中の維持設定 (set_clk_sleep_mode) でこのクロックを
 *   参照しており、リセット時の既定値が不明なので明示する。解除済みなら no-op
 * EXTFLASH_KERCLK_IC3: XSPI2 カーネルクロックを ST 版と同じ IC3 (PLL1 分周で
 *   200MHz) にする。必須。0 にすると FSBL と同じ HCLK になり、SR.BUSY が
 *   落ちずに動かない
 */
#define EXTFLASH_FIX_VDDIO3_1V8		(1)
#define EXTFLASH_FIX_XSPIM_CONFIG	(1)
#define EXTFLASH_FIX_XSPIPHY		(1)
#define EXTFLASH_KERCLK_IC3		(1)

/* 失敗時の切り分け: プリスケーラ最大の 1 線 SPI で JEDEC ID を読む */
#define EXTFLASH_SLOW_TEST		(1)
#define EXTFLASH_SLOW_PRESCALER		(255U)	/* DCR2.PRESCALER の最大値 */

/* 失敗時の追加診断 (VDDIO3 監視 / PN6 パッド生存 / DLL 校正)。通常は 0 */
#define EXTFLASH_PAD_TEST		(0)

#define EXTFLASH_KERCLK_TARGET_HZ	(200000000U)	/* IC3 選択時の目標 */

#define JEDEC_MANUF_MACRONIX	(0xC2U)
#define JEDEC_READ_ID_CMD	(0x9FU)

LOCAL XSPI_HandleTypeDef	hxspi_nor;	/* XSPI2 */
LOCAL BOOL			mapped = FALSE;

/* 1ステップの成否を UART に出す (wm8904.c と同じ書式) */
LOCAL ER extflash_step(const char *name, BOOL ok)
{
	if(ok) {
		tm_printf((UB*)"  [ OK ] %s\n", name);
		return E_OK;
	}
	tm_printf((UB*)"  [FAIL] %s (XSPI ErrorCode=0x%x State=0x%x SR=0x%08x)\n",
			name, (UW)hxspi_nor.ErrorCode, (UW)hxspi_nor.State, (UW)XSPI2->SR);
	return E_IO;
}

#define STEP(name, cond)	do { if(extflash_step((name), (cond)) < E_OK) goto fail; } while(0)

/*
 * 切り分け用のレジスタダンプ。
 * 読むだけで副作用は無い (XSPI2 の SR/CR も読み出しでフラグは変わらない)。
 * クロックが止まっている周辺 (RCC で無効) のレジスタは読むとバスフォルトに
 * なるので、XSPIM/XSPI2/GPION は RCC の有効ビットを見てから読む。
 */
EXPORT void extflash_dump_regs(const char *when)
{
	UW	v;

	tm_printf((UB*)"--- XSPI2 regs (%s) ---\n", when);

	/* PWR: VDDIO3 = Port N の電源 */
	v = PWR->SVMCR3;
	tm_printf((UB*)"  PWR SVMCR3   =0x%08x  VDDIO3SV=%d VDDIO3RDY=%d VDDIO3VRSEL=%d(1=1.8V) VDDIO3VMEN=%d\n",
			v,
			(v & PWR_SVMCR3_VDDIO3SV_Msk)    ? 1 : 0,
			(v & PWR_SVMCR3_VDDIO3RDY_Msk)   ? 1 : 0,
			(v & PWR_SVMCR3_VDDIO3VRSEL_Msk) ? 1 : 0,
			(v & PWR_SVMCR3_VDDIO3VMEN_Msk)  ? 1 : 0);

	/* RCC: クロック有効・リセット状態・カーネルクロック選択 */
	v = RCC->AHB5ENR;
	tm_printf((UB*)"  RCC AHB5ENR  =0x%08x  XSPI2EN=%d XSPIMEN=%d\n", v,
			(v & RCC_AHB5ENR_XSPI2EN_Msk) ? 1 : 0,
			(v & RCC_AHB5ENR_XSPIMEN_Msk) ? 1 : 0);
	v = RCC->AHB5RSTSR;
	tm_printf((UB*)"  RCC AHB5RSTSR=0x%08x  XSPI2RST=%d XSPIMRST=%d (1=in reset)\n", v,
			(v & RCC_AHB5RSTSR_XSPI2RSTS_Msk) ? 1 : 0,
			(v & RCC_AHB5RSTSR_XSPIMRSTS_Msk) ? 1 : 0);
	v = RCC->AHB4ENR;
	tm_printf((UB*)"  RCC AHB4ENR  =0x%08x  GPIONEN=%d\n", v,
			(v & RCC_AHB4ENR_GPIONEN_Msk) ? 1 : 0);
	v = RCC->CCIPR6;
	tm_printf((UB*)"  RCC CCIPR6   =0x%08x  XSPI2SEL=%d (0=HCLK 1=CLKP 2=IC3 3=IC4)\n", v,
			(v & RCC_CCIPR6_XSPI2SEL_Msk) >> RCC_CCIPR6_XSPI2SEL_Pos);
	v = RCC->MISCENR;
	tm_printf((UB*)"  RCC MISCENR  =0x%08x  XSPIPHYCOMPEN=%d\n", v,
			(v & RCC_MISCENR_XSPIPHYCOMPEN_Msk) ? 1 : 0);
	v = RCC->MISCRSTSR;
	tm_printf((UB*)"  RCC MISCRSTSR=0x%08x  XSPIPHY2RST=%d (1=in reset)\n", v,
			(v & RCC_MISCRSTSR_XSPIPHY2RSTS_Msk) ? 1 : 0);
	tm_printf((UB*)"  RCC IC3CFGR  =0x%08x  DIVENSR=0x%08x\n", RCC->IC3CFGR, RCC->DIVENSR);
	tm_printf((UB*)"  HCLK=%u Hz  XSPI2 kernel clock=%u Hz\n",
			HAL_RCC_GetHCLKFreq(), HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_XSPI2));

	/* XSPIM (IO マネージャ) */
	if(__HAL_RCC_XSPIM_IS_CLK_ENABLED()) {
		v = XSPIM->CR;
		tm_printf((UB*)"  XSPIM CR     =0x%08x  MUXEN=%d MODE=%d CSSEL_OVR_EN=%d OVR_O2=%d REQ2ACK=%d\n", v,
				(v & XSPIM_CR_MUXEN_Msk) ? 1 : 0,
				(v & XSPIM_CR_MODE_Msk) ? 1 : 0,
				(v & XSPIM_CR_CSSEL_OVR_EN_Msk) ? 1 : 0,
				(v & XSPIM_CR_CSSEL_OVR_O2_Msk) ? 1 : 0,
				(v & XSPIM_CR_REQ2ACK_TIME_Msk) >> XSPIM_CR_REQ2ACK_TIME_Pos);
	} else {
		tm_printf((UB*)"  XSPIM CR     = (clock disabled)\n");
	}

	/* XSPI2 本体 */
	if(__HAL_RCC_XSPI2_IS_CLK_ENABLED()) {
		v = XSPI2->CR;
		tm_printf((UB*)"  XSPI2 CR     =0x%08x  EN=%d FMODE=%d CSSEL=%d MSEL=%d NOPREF=%d\n", v,
				(v & XSPI_CR_EN_Msk) ? 1 : 0,
				(v & XSPI_CR_FMODE_Msk) >> XSPI_CR_FMODE_Pos,
				(v & XSPI_CR_CSSEL_Msk) ? 1 : 0,
				(v & XSPI_CR_MSEL_Msk) >> XSPI_CR_MSEL_Pos,
				(v & XSPI_CR_NOPREF) ? 1 : 0);
		tm_printf((UB*)"  XSPI2 DCR1   =0x%08x  DCR2=0x%08x (PRESCALER=%d)  DCR3=0x%08x  DCR4=0x%08x\n",
				XSPI2->DCR1, XSPI2->DCR2,
				(XSPI2->DCR2 & XSPI_DCR2_PRESCALER_Msk) >> XSPI_DCR2_PRESCALER_Pos,
				XSPI2->DCR3, XSPI2->DCR4);
		v = XSPI2->SR;
		tm_printf((UB*)"  XSPI2 SR     =0x%08x  BUSY=%d TCF=%d TEF=%d FTF=%d FLEVEL=%d\n", v,
				(v & XSPI_SR_BUSY_Msk) ? 1 : 0,
				(v & XSPI_SR_TCF_Msk) ? 1 : 0,
				(v & XSPI_SR_TEF_Msk) ? 1 : 0,
				(v & XSPI_SR_FTF_Msk) ? 1 : 0,
				(v & XSPI_SR_FLEVEL_Msk) >> XSPI_SR_FLEVEL_Pos);
		tm_printf((UB*)"  XSPI2 CCR    =0x%08x  TCR=0x%08x  IR=0x%08x  DLR=0x%08x\n",
				XSPI2->CCR, XSPI2->TCR, XSPI2->IR, XSPI2->DLR);
		tm_printf((UB*)"  XSPI2 CALFCR =0x%08x  CALMR=0x%08x  CALSOR=0x%08x  CALSIR=0x%08x\n",
				XSPI2->CALFCR, XSPI2->CALMR, XSPI2->CALSOR, XSPI2->CALSIR);
	} else {
		tm_printf((UB*)"  XSPI2        = (clock disabled)\n");
	}

	/* GPION: 期待値 MODER=0x00aa2aaa (PN0-6,8-11 が AF) AFR0=0x09999999 AFR1=0x00009999 (AF9)
	 * OSPEEDR=0x00ff3fff (VERY_HIGH) PUPDR: PN0,PN1 のみ pull-up = 0x00000005 */
	if(__HAL_RCC_GPION_IS_CLK_ENABLED()) {
		tm_printf((UB*)"  GPION MODER  =0x%08x  OTYPER=0x%08x  OSPEEDR=0x%08x  PUPDR=0x%08x\n",
				GPION->MODER, GPION->OTYPER, GPION->OSPEEDR, GPION->PUPDR);
		tm_printf((UB*)"  GPION AFR0   =0x%08x  AFR1=0x%08x  IDR=0x%08x\n",
				GPION->AFR[0], GPION->AFR[1], GPION->IDR);
		tm_printf((UB*)"  GPION SECCFGR=0x%08x  PRIVCFGR=0x%08x  DELAYR0=0x%08x DELAYR1=0x%08x  ADVCFGR0=0x%08x ADVCFGR1=0x%08x\n",
				GPION->SECCFGR, GPION->PRIVCFGR,
				GPION->DELAYR[0], GPION->DELAYR[1], GPION->ADVCFGR[0], GPION->ADVCFGR[1]);
	} else {
		tm_printf((UB*)"  GPION        = (clock disabled)\n");
	}
	tm_printf((UB*)"--- end ---\n");
}

/*
 * XSPI2 のクロック・リセット・電源・端子 (ST BSP の XSPI_NOR_MspInit 相当)。
 * Appli の stm32n6xx_hal_msp.c には HAL_XSPI_MspInit が無い (weak の空実装が
 * 呼ばれる) ので、HAL_XSPI_Init の前にここで済ませる。
 */
LOCAL BOOL xspi2_hw_setup(void)
{
	RCC_PeriphCLKInitTypeDef	clk = {0};
	GPIO_InitTypeDef		gpio = {0};

	/* カーネルクロック */
	clk.PeriphClockSelection = RCC_PERIPHCLK_XSPI2;
#if EXTFLASH_KERCLK_IC3
	{
		/* ST 版と同じ IC3 (PLL1 分周)。PLL1 は FSBL の設定のまま (1200MHz 想定) なので、
		 * 分周比は実測値から 200MHz になるよう決める */
		UW pll1 = HAL_RCCEx_GetPLL1CLKFreq();
		UW div  = (pll1 + EXTFLASH_KERCLK_TARGET_HZ / 2U) / EXTFLASH_KERCLK_TARGET_HZ;
		if(div < 1U) div = 1U;
		if(div > 256U) div = 256U;
		clk.Xspi2ClockSelection = RCC_XSPI2CLKSOURCE_IC3;
		clk.ICSelection[RCC_IC3].ClockSelection = RCC_ICCLKSOURCE_PLL1;
		clk.ICSelection[RCC_IC3].ClockDivider   = div;
		tm_printf((UB*)"  kernel clock: IC3 = PLL1 %u Hz / %u\n", pll1, div);
	}
#else
	clk.Xspi2ClockSelection = RCC_XSPI2CLKSOURCE_HCLK;	/* FSBL と同じ。これだと SR.BUSY が落ちない */
#endif
	if(HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK) return FALSE;

	/* FSBL が設定した XSPI2 のレジスタを初期値に戻す */
	__HAL_RCC_XSPI2_CLK_ENABLE();
	__HAL_RCC_XSPI2_FORCE_RESET();
	__HAL_RCC_XSPI2_RELEASE_RESET();
	__HAL_RCC_XSPIM_CLK_ENABLE();

#if EXTFLASH_FIX_XSPIPHY
	/* XSPI PHY: 補償セルのクロックを有効化し、PHY2 のリセットを解除する
	 * (解除済みなら no-op)。ヒューズ・OTP には触らない */
	__HAL_RCC_XSPIPHYCOMP_CLK_ENABLE();
	__HAL_RCC_XSPIPHY2_RELEASE_RESET();
#endif

	/* VDDIO3 (Port N) の電源を有効に (FSBL・ST BSP と同じ) */
	__HAL_RCC_PWR_CLK_ENABLE();
	HAL_PWREx_EnableVddIO3();
#if EXTFLASH_FIX_VDDIO3_1V8
	/* 電圧レンジ 1.8V (FSBL・ST BSP と同じ。PWR の揮発レジスタで OTP ではない) */
	HAL_PWREx_ConfigVddIORange(PWR_VDDIO3, PWR_VDDIO_RANGE_1V8);
#endif

	/* 端子は ST BSP (stm32n6570_discovery_xspi.h) と同じ。すべて PN, AF9 */
	__HAL_RCC_GPION_CLK_ENABLE();
	gpio.Mode      = GPIO_MODE_AF_PP;
	gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
	gpio.Alternate = GPIO_AF9_XSPIM_P2;

	gpio.Pull = GPIO_PULLUP;
	gpio.Pin  = GPIO_PIN_1			/* NCS1 */
		  | GPIO_PIN_0;			/* DQS0 */
	HAL_GPIO_Init(GPION, &gpio);

	gpio.Pull = GPIO_NOPULL;
	gpio.Pin  = GPIO_PIN_6			/* CLK */
		  | GPIO_PIN_2 | GPIO_PIN_3	/* IO0, IO1 */
		  | GPIO_PIN_4 | GPIO_PIN_5	/* IO2, IO3 */
		  | GPIO_PIN_8 | GPIO_PIN_9	/* IO4, IO5 */
		  | GPIO_PIN_10 | GPIO_PIN_11;	/* IO6, IO7 */
	HAL_GPIO_Init(GPION, &gpio);

	return TRUE;
}

/* XSPI2 本体の設定 (ST BSP の MX_XSPI_NOR_Init 相当。違いは冒頭 5, 6) */
LOCAL BOOL xspi2_init(UW prescaler)
{
	hxspi_nor.Instance = XSPI2;
	hxspi_nor.State    = HAL_XSPI_STATE_RESET;	/* 再試行でもレジスタを設定させる */

	hxspi_nor.Init.FifoThresholdByte       = 1;
	hxspi_nor.Init.MemoryMode              = HAL_XSPI_SINGLE_MEM;
	hxspi_nor.Init.MemoryType              = HAL_XSPI_MEMTYPE_MACRONIX;
	hxspi_nor.Init.MemorySize              = HAL_XSPI_SIZE_1GB;
	hxspi_nor.Init.ChipSelectHighTimeCycle = 2;
	hxspi_nor.Init.FreeRunningClock        = HAL_XSPI_FREERUNCLK_DISABLE;
	hxspi_nor.Init.ClockMode               = HAL_XSPI_CLOCK_MODE_0;
	hxspi_nor.Init.WrapSize                = HAL_XSPI_WRAP_NOT_SUPPORTED;
	hxspi_nor.Init.ClockPrescaler          = prescaler;
	hxspi_nor.Init.SampleShifting          = HAL_XSPI_SAMPLE_SHIFT_NONE;
	hxspi_nor.Init.ChipSelectBoundary      = HAL_XSPI_BONDARYOF_NONE;
	hxspi_nor.Init.MaxTran                 = 0;
	hxspi_nor.Init.Refresh                 = 0;
	hxspi_nor.Init.MemorySelect            = HAL_XSPI_CSSEL_NCS1;
	hxspi_nor.Init.MemoryExtended          = HAL_XSPI_CSSEL_SW;
	/* DelayHoldQuarterCycle は STM32N6 では未使用 (HAL のコメントより) */

	if(HAL_XSPI_Init(&hxspi_nor) != HAL_OK) return FALSE;

#if EXTFLASH_FIX_XSPIM_CONFIG
	{
		/* IO マネージャ: XSPI2 -> Port2、チップセレクトは NCS1 に固定
		 * (BSP2 FSBL の MX_XSPI2_Init と同じ)。HAL_XSPI_Init の後に呼ぶ
		 * (HAL は XSPI を一旦止めて XSPIM->CR を書き直し、再度有効にする) */
		XSPIM_CfgTypeDef iom = {0};
		iom.nCSOverride = HAL_XSPI_CSSEL_OVR_NCS1;
		iom.IOPort      = HAL_XSPIM_IOPORT_2;
		iom.Req2AckTime = 1;
		if(HAL_XSPIM_Config(&hxspi_nor, &iom, HAL_XSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) return FALSE;
	}
#endif
	return TRUE;
}

/*
 * フラッシュのソフトウェアリセット (ST BSP の XSPI_NOR_ResetMemory と同じ)。
 * フラッシュが SPI / STR-OPI / DTR-OPI のどのモードに居ても戻せるよう、
 * 3 モードすべてで Reset Enable + Reset を送る。リセット後は SPI モード。
 * (CR2 は揮発性だが、電源を切らないリセットでは前に誰か (外部ローダ、
 *  前回のこのアプリ) が設定した OPI モードが残り得る)
 */
LOCAL BOOL flash_reset(void)
{
	static const struct {
		MX66UW1G45G_Interface_t	mode;
		MX66UW1G45G_Transfer_t	rate;
		const char		*name;
	} seq[] = {
		{ MX66UW1G45G_SPI_MODE, MX66UW1G45G_STR_TRANSFER, "SPI"     },
		{ MX66UW1G45G_OPI_MODE, MX66UW1G45G_STR_TRANSFER, "STR-OPI" },
		{ MX66UW1G45G_OPI_MODE, MX66UW1G45G_DTR_TRANSFER, "DTR-OPI" },
	};
	INT	i;
	UW	t0;

	for(i = 0; i < (INT)(sizeof(seq) / sizeof(seq[0])); i++) {
		t0 = HAL_GetTick();
		if(MX66UW1G45G_ResetEnable(&hxspi_nor, seq[i].mode, seq[i].rate) != MX66UW1G45G_OK) {
			tm_printf((UB*)"  reset enable (%s) failed after %u ms\n", seq[i].name, HAL_GetTick() - t0);
			return FALSE;
		}
		if(MX66UW1G45G_ResetMemory(&hxspi_nor, seq[i].mode, seq[i].rate) != MX66UW1G45G_OK) {
			tm_printf((UB*)"  reset memory (%s) failed after %u ms\n", seq[i].name, HAL_GetTick() - t0);
			return FALSE;
		}
	}
	return TRUE;
}

/*
 * SPI モードから DTR-OPI モードへ (ST BSP の XSPI_NOR_EnterDOPIMode の前半)。
 * CR2 のダミーサイクルを 20 にしてから、CR2 で DTR-OPI を有効にする。
 * 最後の書き込みの直後からフラッシュは DTR-OPI でしか応答しない。
 */
LOCAL BOOL flash_enter_dopi(void)
{
	const MX66UW1G45G_Interface_t	mode = MX66UW1G45G_SPI_MODE;
	const MX66UW1G45G_Transfer_t	rate = MX66UW1G45G_STR_TRANSFER;

	if(MX66UW1G45G_WriteEnable(&hxspi_nor, mode, rate) != MX66UW1G45G_OK) return FALSE;
	if(MX66UW1G45G_WriteCfg2Register(&hxspi_nor, mode, rate,
			MX66UW1G45G_CR2_REG3_ADDR, MX66UW1G45G_CR2_DC_20_CYCLES) != MX66UW1G45G_OK) return FALSE;
	if(MX66UW1G45G_WriteEnable(&hxspi_nor, mode, rate) != MX66UW1G45G_OK) return FALSE;
	if(MX66UW1G45G_WriteCfg2Register(&hxspi_nor, mode, rate,
			MX66UW1G45G_CR2_REG1_ADDR, MX66UW1G45G_CR2_DOPI) != MX66UW1G45G_OK) return FALSE;
	return TRUE;
}

/* DTR-OPI で CR2 を読み戻して DTR-OPI になっているか (EnterDOPIMode の後半) */
LOCAL BOOL flash_check_dopi(void)
{
	UB	reg[2] = {0, 0};	/* DTR では 2 バイト単位で読む */

	if(MX66UW1G45G_ReadCfg2Register(&hxspi_nor, MX66UW1G45G_OPI_MODE, MX66UW1G45G_DTR_TRANSFER,
			MX66UW1G45G_CR2_REG1_ADDR, reg) != MX66UW1G45G_OK) return FALSE;
	return (reg[0] == MX66UW1G45G_CR2_DOPI);
}

#if EXTFLASH_SLOW_TEST
/*
 * 切り分け: XSPI2 を RCC でリセットし直し、プリスケーラ最大の 1 線 SPI で
 * JEDEC ID (0x9F) を読む。コンポーネントドライバを通さず HAL を直接呼び、
 * 命令 1 線 / アドレス無し / データ 1 線 3 バイト / DQS 無しを明示する。
 * ここでも TIMEOUT なら、信号品質 (速度) ではなく MCU 側設定の問題と確定する。
 */
LOCAL void slow_spi_test(void)
{
	XSPI_RegularCmdTypeDef	cmd = {0};
	UB			id[3] = {0, 0, 0};
	UW			t0, kerclk;
	HAL_StatusTypeDef	st;

	tm_printf((UB*)"--- slow SPI test (prescaler %u, 1-line JEDEC ID) ---\n", EXTFLASH_SLOW_PRESCALER);

	/* BUSY のまま固まった XSPI2 を RCC リセットで戻してから設定し直す */
	if(!xspi2_hw_setup()) { tm_printf((UB*)"  hw setup failed\n"); return; }
	if(!xspi2_init(EXTFLASH_SLOW_PRESCALER)) {
		tm_printf((UB*)"  HAL_XSPI_Init failed (ErrorCode=0x%x)\n", (UW)hxspi_nor.ErrorCode);
		extflash_dump_regs("slow test: after init failure");
		return;
	}
	kerclk = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_XSPI2);
	tm_printf((UB*)"  SCLK = %u Hz / %u = %u Hz\n", kerclk, EXTFLASH_SLOW_PRESCALER + 1U,
			kerclk / (EXTFLASH_SLOW_PRESCALER + 1U));

	cmd.OperationType      = HAL_XSPI_OPTYPE_COMMON_CFG;
	cmd.IOSelect           = HAL_XSPI_SELECT_IO_3_0;
	cmd.InstructionMode    = HAL_XSPI_INSTRUCTION_1_LINE;
	cmd.InstructionDTRMode = HAL_XSPI_INSTRUCTION_DTR_DISABLE;
	cmd.InstructionWidth   = HAL_XSPI_INSTRUCTION_8_BITS;
	cmd.Instruction        = JEDEC_READ_ID_CMD;
	cmd.AddressMode        = HAL_XSPI_ADDRESS_NONE;
	cmd.AlternateBytesMode = HAL_XSPI_ALT_BYTES_NONE;
	cmd.DataMode           = HAL_XSPI_DATA_1_LINE;
	cmd.DataDTRMode        = HAL_XSPI_DATA_DTR_DISABLE;
	cmd.DataLength         = 3U;
	cmd.DummyCycles        = 0U;
	cmd.DQSMode            = HAL_XSPI_DQS_DISABLE;

	t0 = HAL_GetTick();
	st = HAL_XSPI_Command(&hxspi_nor, &cmd, HAL_XSPI_TIMEOUT_DEFAULT_VALUE);
	if(st != HAL_OK) {
		tm_printf((UB*)"  [FAIL] HAL_XSPI_Command st=%d ErrorCode=0x%x after %u ms SR=0x%08x\n",
				(INT)st, (UW)hxspi_nor.ErrorCode, HAL_GetTick() - t0, (UW)XSPI2->SR);
		extflash_dump_regs("slow test: after command timeout");
		return;
	}
	st = HAL_XSPI_Receive(&hxspi_nor, id, HAL_XSPI_TIMEOUT_DEFAULT_VALUE);
	if(st != HAL_OK) {
		tm_printf((UB*)"  [FAIL] HAL_XSPI_Receive st=%d ErrorCode=0x%x after %u ms SR=0x%08x\n",
				(INT)st, (UW)hxspi_nor.ErrorCode, HAL_GetTick() - t0, (UW)XSPI2->SR);
		extflash_dump_regs("slow test: after receive timeout");
		return;
	}
	tm_printf((UB*)"  [ OK ] JEDEC ID: %02x %02x %02x (%s) in %u ms\n", id[0], id[1], id[2],
			(id[0] == JEDEC_MANUF_MACRONIX) ? "Macronix" : "NOT Macronix",
			HAL_GetTick() - t0);
	extflash_dump_regs("slow test: after success");
}
#endif	/* EXTFLASH_SLOW_TEST */

#if EXTFLASH_PAD_TEST
/*
 * 診断 1: VDDIO3 監視。SVMCR3.VDDIO3VMEN を立てて最大 10ms 待ち、VDDIO3RDY を
 * 表示する。RDY が立たなければ VDDIO3 (Port N の給電) が監視回路から見て
 * 無効ということ。終了後 VMEN は元の値に戻す。
 */
LOCAL void diag_vddio3_monitor(void)
{
	UW	orig = PWR->SVMCR3;
	INT	i;
	BOOL	rdy = FALSE;

	tm_printf((UB*)"--- diag 1: VDDIO3 monitor ---\n");
	SET_BIT(PWR->SVMCR3, PWR_SVMCR3_VDDIO3VMEN);
	for(i = 0; i < 10; i++) {		/* 1ms x 10 */
		if(PWR->SVMCR3 & PWR_SVMCR3_VDDIO3RDY_Msk) { rdy = TRUE; break; }
		tk_dly_tsk(1);
	}
	tm_printf((UB*)"  SVMCR3=0x%08x VDDIO3VMEN=%d VDDIO3SV=%d VDDIO3RDY=%d after %d ms\n",
			PWR->SVMCR3,
			(PWR->SVMCR3 & PWR_SVMCR3_VDDIO3VMEN_Msk) ? 1 : 0,
			(PWR->SVMCR3 & PWR_SVMCR3_VDDIO3SV_Msk) ? 1 : 0,
			rdy ? 1 : 0, i);
	if(rdy) {
		tm_printf((UB*)"  [ OK ] VDDIO3 ready (supply present)\n");
	} else {
		tm_printf((UB*)"  [FAIL] VDDIO3RDY did not become 1 within 10 ms -> VDDIO3 not seen as valid\n");
	}
	/* VMEN を元に戻す */
	MODIFY_REG(PWR->SVMCR3, PWR_SVMCR3_VDDIO3VMEN, orig & PWR_SVMCR3_VDDIO3VMEN);
}

/*
 * 診断 2: パッドの生存確認。XSPI2 を RCC リセットで止めた (EN=0) 状態で
 * PN6 (CLK) を GPIO 出力にし、ODR 0/1 それぞれで IDR を読む。
 * 追従しなければパッドが機能していない (給電/隔離)。終了後 AF9 に戻す。
 */
LOCAL void diag_pad_pn6(void)
{
	GPIO_InitTypeDef	gpio = {0};
	UW			idr_af, idr_lo, idr_hi;
	volatile INT		w;

	tm_printf((UB*)"--- diag 2: PN6 (CLK) pad test ---\n");

	/* XSPI2 を止める (BUSY のまま固まっていてもリセットで戻る) */
	__HAL_RCC_XSPI2_CLK_ENABLE();
	__HAL_RCC_XSPI2_FORCE_RESET();
	__HAL_RCC_XSPI2_RELEASE_RESET();
	__HAL_RCC_GPION_CLK_ENABLE();
	idr_af = GPION->IDR;

	gpio.Pin       = GPIO_PIN_6;
	gpio.Mode      = GPIO_MODE_OUTPUT_PP;
	gpio.Pull      = GPIO_NOPULL;
	gpio.Speed     = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(GPION, &gpio);

	HAL_GPIO_WritePin(GPION, GPIO_PIN_6, GPIO_PIN_RESET);
	for(w = 0; w < 1000; w++) ;		/* 数 µs */
	idr_lo = GPION->IDR;
	HAL_GPIO_WritePin(GPION, GPIO_PIN_6, GPIO_PIN_SET);
	for(w = 0; w < 1000; w++) ;
	idr_hi = GPION->IDR;

	tm_printf((UB*)"  IDR(AF)=0x%08x  ODR=0 -> IDR=0x%08x (PN6=%d)  ODR=1 -> IDR=0x%08x (PN6=%d)\n",
			idr_af, idr_lo, (idr_lo >> 6) & 1U, idr_hi, (idr_hi >> 6) & 1U);
	if(((idr_lo >> 6) & 1U) == 0U && ((idr_hi >> 6) & 1U) == 1U) {
		tm_printf((UB*)"  [ OK ] PN6 follows ODR -> pad alive\n");
	} else {
		tm_printf((UB*)"  [FAIL] PN6 does not follow ODR -> pad not functional (supply/isolation)\n");
	}

	/* AF9 に戻す (xspi2_hw_setup と同じ設定) */
	gpio.Pin       = GPIO_PIN_6;
	gpio.Mode      = GPIO_MODE_AF_PP;
	gpio.Pull      = GPIO_NOPULL;
	gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
	gpio.Alternate = GPIO_AF9_XSPIM_P2;
	HAL_GPIO_Init(GPION, &gpio);
}

/*
 * 診断 3: フィードバッククロック。
 * STM32N6 の XSPI には遅延ブロックのバイパス (DLYBYP) が無い (DCR1 に
 * ビットが無く、HAL にも DelayBlockBypass が無い)。代わりに DLL 校正
 * レジスタ CALFCR (全周期) / CALMR (フィードバッククロック遅延) /
 * CALSOR / CALSIR があるので、
 *   3a. プリスケーラ 255 で初期化した直後の校正値と CALMAX を表示
 *   3b. フリーランクロック (DCR1.FRCK) を出し、PN6 の IDR をサンプルして
 *       出力クロックがパッドまで届いているか (遷移回数) を見る
 *   3c. CALMR (フィードバッククロック遅延) を fine=0/coarse=0 に上書きして
 *       1 線 SPI で JEDEC ID を再試行。BUSY が落ちるか、3 バイトを表示
 * の 3 つに置き換える。
 */
LOCAL void diag_feedback_clock(void)
{
	XSPI_RegularCmdTypeDef	cmd = {0};
	XSPI_HSCalTypeDef	cal = {0};
	UB			id[3] = {0, 0, 0};
	UW			t0, i, v, prev, trans = 0, ones = 0;
	HAL_StatusTypeDef	st;

	tm_printf((UB*)"--- diag 3: feedback clock / DLL calibration (no DLYBYP on N6) ---\n");

	if(!xspi2_hw_setup()) { tm_printf((UB*)"  hw setup failed\n"); return; }
	if(!xspi2_init(EXTFLASH_SLOW_PRESCALER)) {
		tm_printf((UB*)"  HAL_XSPI_Init failed (ErrorCode=0x%x)\n", (UW)hxspi_nor.ErrorCode);
		return;
	}

	/* 3a: 校正値 */
	tm_printf((UB*)"  3a: CALFCR=0x%08x (CALMAX=%d fine=%d coarse=%d)  CALMR=0x%08x  CALSOR=0x%08x  CALSIR=0x%08x  SR=0x%08x\n",
			XSPI2->CALFCR,
			(XSPI2->CALFCR & XSPI_CALFCR_CALMAX_Msk) ? 1 : 0,
			XSPI2->CALFCR & XSPI_CALFCR_FINE_Msk,
			(XSPI2->CALFCR & XSPI_CALFCR_COARSE_Msk) >> XSPI_CALFCR_COARSE_Pos,
			XSPI2->CALMR, XSPI2->CALSOR, XSPI2->CALSIR, XSPI2->SR);

	/* 3b: フリーランクロック → PN6 の IDR を 256 回サンプル */
	SET_BIT(XSPI2->DCR1, XSPI_DCR1_FRCK);
	prev = (GPION->IDR >> 6) & 1U;
	for(i = 0; i < 256; i++) {
		v = (GPION->IDR >> 6) & 1U;
		if(v != prev) trans++;
		if(v) ones++;
		prev = v;
	}
	CLEAR_BIT(XSPI2->DCR1, XSPI_DCR1_FRCK);
	tm_printf((UB*)"  3b: FRCK on: PN6 IDR samples=256 high=%u transitions=%u SR=0x%08x -> %s\n",
			ones, trans, XSPI2->SR,
			(trans > 0) ? "clock reaches the pad" : "NO clock seen on PN6");

	/* 3c: CALMR を最小に上書きして 1 線 JEDEC ID */
	cal.DelayValueType        = HAL_XSPI_CAL_FEEDBACK_CLK_DELAY;
	cal.FineCalibrationUnit   = 0;
	cal.CoarseCalibrationUnit = 0;
	st = HAL_XSPI_SetDelayValue(&hxspi_nor, &cal);
	tm_printf((UB*)"  3c: CALMR forced to fine=0 coarse=0 (st=%d) CALMR=0x%08x\n", (INT)st, XSPI2->CALMR);

	cmd.OperationType      = HAL_XSPI_OPTYPE_COMMON_CFG;
	cmd.IOSelect           = HAL_XSPI_SELECT_IO_3_0;
	cmd.InstructionMode    = HAL_XSPI_INSTRUCTION_1_LINE;
	cmd.InstructionDTRMode = HAL_XSPI_INSTRUCTION_DTR_DISABLE;
	cmd.InstructionWidth   = HAL_XSPI_INSTRUCTION_8_BITS;
	cmd.Instruction        = JEDEC_READ_ID_CMD;
	cmd.AddressMode        = HAL_XSPI_ADDRESS_NONE;
	cmd.AlternateBytesMode = HAL_XSPI_ALT_BYTES_NONE;
	cmd.DataMode           = HAL_XSPI_DATA_1_LINE;
	cmd.DataDTRMode        = HAL_XSPI_DATA_DTR_DISABLE;
	cmd.DataLength         = 3U;
	cmd.DummyCycles        = 0U;
	cmd.DQSMode            = HAL_XSPI_DQS_DISABLE;

	t0 = HAL_GetTick();
	st = HAL_XSPI_Command(&hxspi_nor, &cmd, HAL_XSPI_TIMEOUT_DEFAULT_VALUE);
	if(st == HAL_OK) st = HAL_XSPI_Receive(&hxspi_nor, id, HAL_XSPI_TIMEOUT_DEFAULT_VALUE);
	v = XSPI2->SR;
	if(st != HAL_OK) {
		tm_printf((UB*)"  3c: [FAIL] st=%d ErrorCode=0x%x after %u ms SR=0x%08x BUSY=%d -> state machine still stuck\n",
				(INT)st, (UW)hxspi_nor.ErrorCode, HAL_GetTick() - t0, v, (v & XSPI_SR_BUSY_Msk) ? 1 : 0);
	} else {
		tm_printf((UB*)"  3c: [ OK ] BUSY cleared, JEDEC ID: %02x %02x %02x in %u ms -> %s\n",
				id[0], id[1], id[2], HAL_GetTick() - t0,
				(id[0] == JEDEC_MANUF_MACRONIX) ? "Macronix answered" :
				((id[0] == 0x00U && id[1] == 0x00U && id[2] == 0x00U) ||
				 (id[0] == 0xFFU && id[1] == 0xFFU && id[2] == 0xFFU))
					? "TX ran but nothing came back through the pads" : "unexpected ID");
	}
	extflash_dump_regs("diag 3: after");
}
#endif	/* EXTFLASH_PAD_TEST */

EXPORT ER extflash_init(void)
{
	UB	id[3] = {0, 0, 0};
	UW	kerclk;

	if(mapped) return E_OK;

	tm_printf((UB*)"extflash init (MX66UW1G45G, XSPI2, DTR-OPI) fixes: VDDIO3_1V8=%d XSPIM=%d XSPIPHY=%d IC3=%d\n",
			EXTFLASH_FIX_VDDIO3_1V8, EXTFLASH_FIX_XSPIM_CONFIG, EXTFLASH_FIX_XSPIPHY, EXTFLASH_KERCLK_IC3);

	/* 何も触る前の状態 (FSBL が残した設定) */
	extflash_dump_regs("before init");

	STEP("XSPI2 clock/reset/GPIO", xspi2_hw_setup());
	STEP("HAL_XSPI_Init", xspi2_init(EXTFLASH_PRESCALER));

	kerclk = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_XSPI2);
	tm_printf((UB*)"  XSPI2 kernel clock %u Hz / (prescaler %u + 1) = SCLK %u Hz\n",
			kerclk, EXTFLASH_PRESCALER, kerclk / (EXTFLASH_PRESCALER + 1U));

	extflash_dump_regs("after HAL_XSPI_Init");

	STEP("Reset memory (SPI, STR-OPI, DTR-OPI)", flash_reset());
	tk_dly_tsk(MX66UW1G45G_RESET_MAX_TIME);	/* リセットが消去中に入った場合の待ち */

	STEP("Ready (SPI)", MX66UW1G45G_AutoPollingMemReady(&hxspi_nor,
			MX66UW1G45G_SPI_MODE, MX66UW1G45G_STR_TRANSFER) == MX66UW1G45G_OK);

	STEP("Read ID (SPI)", MX66UW1G45G_ReadID(&hxspi_nor,
			MX66UW1G45G_SPI_MODE, MX66UW1G45G_STR_TRANSFER, id) == MX66UW1G45G_OK);
	tm_printf((UB*)"  JEDEC ID: %x %x %x\n", id[0], id[1], id[2]);
	STEP("Manufacturer = Macronix (0xC2)", id[0] == JEDEC_MANUF_MACRONIX);

	STEP("Enter DTR-OPI (CR2: 20 dummy cycles, DOPI)", flash_enter_dopi());
	tk_dly_tsk(MX66UW1G45G_WRITE_REG_MAX_TIME);	/* CR2 の書き込み完了待ち */

	STEP("Ready (DTR-OPI)", MX66UW1G45G_AutoPollingMemReady(&hxspi_nor,
			MX66UW1G45G_OPI_MODE, MX66UW1G45G_DTR_TRANSFER) == MX66UW1G45G_OK);
	STEP("Verify CR2 = DOPI", flash_check_dopi());

	/* ST 版はここでプリスケーラを外して 200MHz にするが、しない (冒頭 1) */
	STEP("Memory-mapped mode", MX66UW1G45G_EnableMemoryMappedModeDTR(&hxspi_nor,
			MX66UW1G45G_OPI_MODE) == MX66UW1G45G_OK);

	/*
	 * ST の hotfix: XSPI の自動プリフェッチを止める (audio_bm.c:845 と同じ)。
	 * EnableMemoryMappedModeDTR の中の HAL_XSPI_MemoryMapped がプリフェッチ
	 * 有効で CR を書くので、その後で NOPREF を立てる。まだ 0x70000000 を
	 * 一度も読んでいない (XSPI が BUSY でない) 今のうちに行う。
	 */
	MODIFY_REG(XSPI2->CR, XSPI_CR_NOPREF, HAL_XSPI_AUTOMATIC_PREFETCH_DISABLE);

	mapped = TRUE;
	tm_printf((UB*)"extflash mapped at 0x%x\n", EXTFLASH_BASE);
	return E_OK;

fail:
	/* 失敗した時点の状態。SR.BUSY が立ったままなら XSPI2 の状態機械が
	 * 完了していない (カーネルクロック・XSPIM・PHY を疑う) */
	extflash_dump_regs("after failure");
#if EXTFLASH_PAD_TEST
	diag_vddio3_monitor();	/* 1: VDDIO3 の監視回路から見た給電 */
	diag_pad_pn6();		/* 2: XSPI2 停止状態でパッドが GPIO として動くか */
#endif
#if EXTFLASH_SLOW_TEST
	slow_spi_test();	/* プリスケーラ最大で BUSY が落ちるか */
#endif
#if EXTFLASH_PAD_TEST
	diag_feedback_clock();	/* 3: 校正値・FRCK・CALMR 最小化での再試行 */
#endif
	return E_IO;
}

EXPORT BOOL extflash_mapped(void)
{
	return mapped;
}
