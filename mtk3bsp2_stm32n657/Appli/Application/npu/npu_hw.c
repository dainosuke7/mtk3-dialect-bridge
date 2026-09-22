#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "main.h"		// CMSIS (RCC, RAMCFG, RIFSC, RISAF, CACHEAXI, MPU, MEMSYSCTL, SCB), HAL RCC
#include "npu_hw.h"
#include "../trace/trace.h"	// NOW(), trace_cyc_per_us()

/*
 * NPU を動かす前提となるメモリと NPU 周辺の初期化。
 *
 * 手順は STM32N6-GettingStarted-Audio v2.3.0 の次の2つと同じ。
 *   Int_Mem_Config()  Projects/GS/Src/audio_bm.c:717-764 (APP_LP でない側)
 *   NPU_Config()      Projects/Common/misc_toolbox.c:229-257 (USE_NPU_CACHE=1 の側。
 *                     GS-Audio は app_config.h:72-74 で 1 にしている)
 * ST のコードは同梱せず、レジスタ操作をここに書き起こした。
 *
 * HAL_RAMCFG / HAL_RIF / HAL_CACHEAXI を使わずレジスタを直接操作する理由:
 *   - このプロジェクトの Drivers/STM32N6xx_HAL_Driver には3つとも .c も .h も無い。
 *     Drivers/ は編集しない方針で、参照リポジトリを絶対パスでリンクすることもしない
 *     (CLAUDE.md)。HAL を使うには Drivers/ にファイルを足すしかない
 *   - 中身は短い。HAL_RAMCFG_EnableAXISRAM() は CLEAR_BIT(CR, SRAMSD) の1行
 *     (stm32n6xx_hal_ramcfg.c:1028-1035)。RIF の2関数は RIFSC の3レジスタを
 *     読んで書き戻すだけ (stm32n6xx_hal_rif.c:279-283, 374-382)
 *   - HAL_CACHEAXI_Enable() の BUSYF 待ちは、タイムアウト判定が逆で実質無限待ち
 *     (stm32n6xx_hal_cacheaxi.c:370-376)。しかもカーネル起動後の HAL_GetTick() は
 *     10ms 刻みでしか進まない。ここでは DWT CYCCNT で上限を付けて待つ
 *   RCC のクロック・リセットは、既存コードと同じく HAL の __HAL_RCC_* マクロを使う。
 *
 * ST 版との違い:
 *   1. Int_Mem_Config は RCC->MEMENR |= ... と __HAL_RCC_AXISRAMx_MEM_CLK_ENABLE() で
 *      同じビットを2回立てている (後者は MEMENSR への書き込みで、対象ビットは同じ)。
 *      ここでは1回だけにする
 *   2. Int_Mem_Config の最後の MEMSYSCTL->MSCR |= DCACTIVE|ICACTIVE は移植しない。
 *      CPU のキャッシュ全体の動作を変え、音声経路にも効くため。値を表示するだけにする
 *   3. Int_Mem_Config 冒頭の SYSCFG・CRC のクロック有効化は移植しない。
 *      ここでの処理には使わない (ランタイムを入れるときに要否を見直す)
 *   4. ST は NPU のリセットを初回だけにしている (APP_LP で推論ごとに NPU_Config を
 *      呼び直すため)。本機は起動時に1回しか呼ばないので、そのままリセットする
 *   5. ST の RISAF_Config() (misc_toolbox.c:259) は ST も呼んでいないので移植しない。
 *      理由は下の「RISAF」
 *   6. IC6 / IC11 は変更しない (FSBL の設定のまま)。読んで表示するだけ
 *
 * 既に有効な場合 (FSBL・前回の実行が残した状態):
 *   - BSP2 の FSBL は NPU / CACHEAXI / RIF / RAMCFG / MEMENR に触れない (ソースを確認済み)。
 *     ただしデバッガでシステムリセットを経ずに再実行すると、前回の状態が残り得る
 *   - MEMENR / RAMCFG: 既に目的の状態ならレジスタを書かない
 *   - NPU / CACHEAXI: 変更前の状態を表示したうえで、必ずリセットして既知の状態から
 *     始める。今の時点で NPU を使うものは無いので、リセットしても害は無い
 *     (MDF1 と同じ扱い。CLAUDE.md「FSBL がペリフェラルを触った状態でアプリが起動する」)
 *   - RIF: 読んで書き戻すので、同じ設定を2回書いても結果は同じ。ロック (GLOCK / RLOCK)
 *     が立っていると書き込みはエラーにならず黙って無視されるので、読み戻して確かめる
 *
 * RISAF (NPU が外部フラッシュの重みや AXISRAM3〜6 を読み書きするのに設定が要るか):
 *   要らないと判断した。
 *   - RISAF はリージョンを1つも有効にしていないと、セキュア・特権 (ST のコメントでは
 *     CID1 も) のアクセスだけを通す (SVD の RISAF_REGx_CFGR.BREN の説明、
 *     misc_toolbox.c:57 のコメント)
 *   - RIMC で NPU を CID1・セキュア・特権にすれば、この既定の条件を満たす。
 *     RIMC_ATTR のリセット値は 0 (CID0・非セキュア・非特権) なので、RIMC の設定は必須。
 *     また RISC で NPU をセキュアにしておかないと RIMC の MSEC は無視され、NPU の
 *     アクセスは非セキュアに強制される (stm32n6xx_hal_rif.c:32-38)。だから両方設定する
 *   - GS-Audio も RISAF を一切設定せずに、同じ番地 (重み 0x70180000 を NPU キャッシュ
 *     経由、activations を 0x34350000) で動いている。GS-Audio には Development boot で
 *     動かす手順もある (README の Dev Mode)
 *   - このアプリでも、GPDMA1 (CID を設定していないので CID0。セキュア・特権) が既定の
 *     RISAF のまま AXISRAM1/2 のバッファを読み書きできている。既定の RISAF は少なくとも
 *     「セキュア・特権なら CID を問わず通す」
 *   - 残る不確かさ: Development boot でブート ROM が RISAF をどの状態で渡すか。
 *     NPU_RISAF_DUMP=1 で RISAF のリージョン有効ビットを表示して確かめる
 *
 * CPU のキャッシュと AXISRAM3〜6:
 *   main.c の MPU_Config() が決めているのは .noncacheable (region0) だけで、MPU は
 *   PRIVDEFENA 付き。ほかの番地は既定のメモリマップの属性になり、0x20000000〜
 *   0x3FFFFFFF (AXISRAM) も 0x60000000〜0x7FFFFFFF (外部フラッシュ) も
 *   Normal・ライトバックのキャッシュ対象 (ARMv8-M の既定)。
 *   したがって NPU が AXISRAM に書いた結果を CPU が読む前は SCB_InvalidateDCache_by_Addr、
 *   CPU が書いた入力を NPU が読む前は SCB_CleanInvalidateDCache_by_Addr が要る
 *   (範囲は 32B 単位)。clean だけでなく invalidate もするのは、AED モデルでは入力の領域
 *   (0x34350000〜) が推論中に中間結果・出力の置き場として再利用され、CPU キャッシュに
 *   残ったラインが NPU の書いた値を隠すため (ST も preproc_dpu.c:144 で clean+invalidate)。
 *   NPU キャッシュ (CACHEAXI) を通るのは読み出し専用の重み (0x70180000) だけで、
 *   推論ごとの NPU キャッシュ保守は要らない (ST の network.c / stm32n6.mpool の設定)。
 *   ただし ST は MSCR.DCACTIVE がブート時に 0 になっていると書いている
 *   (audio_bm.c:762)。0 なら CPU の D キャッシュは実際には効いていない。
 *   ここでは値を表示して確かめるだけにする (上の「ST 版との違い」2)。
 */

/* 1 で RISAF の状態を表示する (読むだけ)。表示中にフォルトしたら 0 にする */
#define NPU_RISAF_DUMP		(1)

/* メモリクロック。AXISRAM2 はアプリ自身が使っているので立っているはず (ST に合わせて含める) */
#define NPU_MEMEN	(RCC_MEMENR_AXISRAM2EN | RCC_MEMENR_AXISRAM3EN | RCC_MEMENR_AXISRAM4EN \
			 | RCC_MEMENR_AXISRAM5EN | RCC_MEMENR_AXISRAM6EN | RCC_MEMENR_CACHEAXIRAMEN)

/* RAM の読み書き確認。256B (D キャッシュのライン 8 本分) をパターン P と ~P で確かめる */
#define RAM_TEST_WORDS		(64)
#define RAM_TEST_PATTERN	(0xA5A5A5A5U)

/* CACHEAXI のリセット後に自動で走る全無効化 (SR.BUSYF=1) を待つ上限 */
#define CACHEAXI_WAIT_US	(10000)

/*
 * 待ちループの読み出し回数の上限。時間の上限は DWT CYCCNT で測るが、
 * CYCCNT が進まない場合 (trace_init が FAIL を出す場合) でも必ず抜けるよう、
 * 回数でも打ち切る。1回の周辺読み出しは数十ns 以上かかるので、
 * CYCCNT が動いていれば先に時間の上限に当たる。
 */
#define NPU_WAIT_MAX_READS	(1000000)

/*
 * RIF で NPU を指す番号 (ST の stm32n6xx_hal_rif.h より)
 *   RIF_MASTER_INDEX_NPU      = 1                              (:64)
 *   RIF_RISC_PERIPH_INDEX_NPU = RIF_PERIPH_REG3 | SEC10_Pos    (:184) → レジスタ3の bit10
 *   RIF_CID_1 = 0x2。HAL は POSITION_VAL(0x2) = 1 を MCID に書く (hal_rif.c:279-283)
 */
#define NPU_RIMC_INDEX		(1)
#define NPU_RISC_REG		(3)
#define NPU_RISC_SEC		RIFSC_RISC_SECCFGRx_SEC10_Msk
#define NPU_RISC_PRIV		RIFSC_RISC_PRIVCFGRx_PRIV10_Msk
#define NPU_RISC_RLOCK		RIFSC_RISC_RCFGLOCKRx_RLOCK10_Msk
#define NPU_MASTER_CID		(1U)
#define NPU_RIMC_MASK		(RIFSC_RIMC_ATTRx_MCID_Msk | RIFSC_RIMC_ATTRx_MSEC_Msk | RIFSC_RIMC_ATTRx_MPRIV_Msk)
#define NPU_RIMC_VALUE		((NPU_MASTER_CID << RIFSC_RIMC_ATTRx_MCID_Pos) | RIFSC_RIMC_ATTRx_MSEC_Msk \
				 | RIFSC_RIMC_ATTRx_MPRIV_Msk)	/* = 0x310 */

/*
 * NPU (ATON) のレジスタ。CMSIS に NPU の構造体は無いので番地で読む。
 * ATON_CLKCTRL_VERSION: 読み出し専用。オフセットと期待値は ST の
 * Middlewares/ST/AI/Npu/Devices/STM32N6xx/ATON.h:9991, 10190, 10196 より。
 * LL_ATON_Init() も初期化時に TYPE / MAJOR / MINOR を照合している (ll_aton.c:40-47)
 */
#define ATON_CLKCTRL_VERSION		(NPU_BASE + 0x4U)	/* NPU_BASE は -mcmse で NPU_BASE_S (0x580E0000) */
#define ATON_CLKCTRL_VERSION_DT		(0x01B4121FU)
#define ATON_VERSION_ID_MASK		(0x0000FFFFU)		/* MAJOR[15:12] MINOR[11:8] TYPE[7:0] */
#define NPU_VERSION_WAIT_US		(1000)			/* 0 が返る間に読み直す上限 */

LOCAL BOOL	ready = FALSE;

/* 1ステップの結果を UART に出す (extflash.c と同じ書式) */
LOCAL BOOL npu_step(const char *name, BOOL ok)
{
	tm_printf(ok ? (UB*)"  [ OK ] %s\n" : (UB*)"  [FAIL] %s\n", name);
	return ok;
}

LOCAL void npu_skip(const char *name, const char *why)
{
	tm_printf((UB*)"  [SKIP] %s (%s)\n", name, why);
}

/* ---------------------------------------------------------------- */
/* 状態表示 (読むだけ)                                                 */
/* ---------------------------------------------------------------- */

/*
 * CPU キャッシュが実際に効いているか (CCR と MSCR) と、MPU の有効リージョン。
 * main.c の MPU_Config() は region0 しか書かない (HAL_MPU_Disable はほかの
 * リージョンを消さない) ので、ブート ROM が残したリージョンが無いかもここで見る。
 * 期待値は region0 (.noncacheable) だけ。MPU を触るのはカーネル起動前の
 * MPU_Config() だけなので、ここで RNR を書き換えても競合しない。
 */
LOCAL void show_cpu_cache(void)
{
	UW	ccr  = SCB->CCR;
	UW	mscr = MEMSYSCTL->MSCR;
	UW	ctrl = MPU->CTRL;
	UW	n, i, rlar;

	tm_printf((UB*)"  CPU cache: CCR.DC=%d CCR.IC=%d  MSCR=0x%08x (DCACTIVE=%d ICACTIVE=%d FORCEWT=%d)\n",
			(ccr & SCB_CCR_DC_Msk) ? 1 : 0, (ccr & SCB_CCR_IC_Msk) ? 1 : 0, mscr,
			(mscr & MEMSYSCTL_MSCR_DCACTIVE_Msk) ? 1 : 0,
			(mscr & MEMSYSCTL_MSCR_ICACTIVE_Msk) ? 1 : 0,
			(mscr & MEMSYSCTL_MSCR_FORCEWT_Msk) ? 1 : 0);

	n = (MPU->TYPE & MPU_TYPE_DREGION_Msk) >> MPU_TYPE_DREGION_Pos;
	tm_printf((UB*)"  MPU: CTRL=0x%08x (ENABLE=%d PRIVDEFENA=%d) MAIR0=0x%08x MAIR1=0x%08x, enabled regions:\n",
			ctrl, (ctrl & MPU_CTRL_ENABLE_Msk) ? 1 : 0, (ctrl & MPU_CTRL_PRIVDEFENA_Msk) ? 1 : 0,
			MPU->MAIR0, MPU->MAIR1);
	for(i = 0; i < n; i++) {
		MPU->RNR = i;
		rlar = MPU->RLAR;
		if((rlar & MPU_RLAR_EN_Msk) == 0) continue;
		tm_printf((UB*)"    region %u: 0x%08x-0x%08x AttrIndx=%u\n", i,
				MPU->RBAR & MPU_RBAR_BASE_Msk,
				(rlar & MPU_RLAR_LIMIT_Msk) | 0x1FU,
				(rlar & MPU_RLAR_AttrIndx_Msk) >> MPU_RLAR_AttrIndx_Pos);
	}
}

/* PLL1〜4 の出力周波数。止まっていれば 0 (HAL の計算関数。タイムアウトは使わない) */
LOCAL UW pll_freq(UW sel)
{
	switch(sel) {
	case 0:  return HAL_RCCEx_GetPLL1CLKFreq();
	case 1:  return HAL_RCCEx_GetPLL2CLKFreq();
	case 2:  return HAL_RCCEx_GetPLL3CLKFreq();
	default: return HAL_RCCEx_GetPLL4CLKFreq();
	}
}

/* IC 分周器1つ分の表示。sel は ICxSEL (0〜3 = PLL1〜4)、div は ICxINT + 1 */
LOCAL BOOL show_ic(const char *name, BOOL en, UW sel, UW div)
{
	UW	pll = pll_freq(sel);

	tm_printf((UB*)"  %s: EN=%d src=PLL%u (%u Hz) / %u = %u Hz\n",
			name, en ? 1 : 0, sel + 1U, pll, div, pll / div);
	return en && (pll != 0U);
}

/*
 * NPU のクロック (IC6 = sysc_ck) と NPU RAM のクロック (IC11 = sysd_ck。
 * AXISRAM3〜6・NPU キャッシュ RAM・NPU の NIC) が FSBL で有効になっているか。
 * RCC には書かない。
 *
 * FSBL (FSBL/Core/Src/main.c:211-246) は PLL1 = HSI 64MHz / 4 x 75 = 1200MHz、
 * IC6 = PLL1 / 4 = 300MHz、IC11 = PLL1 / 3 = 400MHz にし、システムクロックを
 * IC2/IC6/IC11 に切り替えている。IC6CFGR のリセット値 (PLL1/4) は FSBL の設定と
 * 同じなので、有効かどうかは DIVENR と CFGR1.SYSSWS で判断する
 * (HAL_RCC_GetNPUClockFreq() は DIVENR を見ないので、これだけでは判断できない)。
 */
LOCAL BOOL check_npu_clocks(void)
{
	UW	cfgr1 = RCC->CFGR1;
	UW	diven = RCC->DIVENR;
	UW	ic6   = RCC->IC6CFGR;
	UW	ic11  = RCC->IC11CFGR;
	UW	sysws = (cfgr1 & RCC_CFGR1_SYSSWS_Msk) >> RCC_CFGR1_SYSSWS_Pos;
	BOOL	ok6, ok11;

	tm_printf((UB*)"  RCC CFGR1=0x%08x (SYSSWS=%u, 3=IC2/IC6/IC11)  DIVENR=0x%08x  IC6CFGR=0x%08x  IC11CFGR=0x%08x\n",
			cfgr1, sysws, diven, ic6, ic11);
	ok6  = show_ic("IC6  (NPU)    ", (diven & RCC_DIVENR_IC6EN_Msk) != 0,
			(ic6 & RCC_IC6CFGR_IC6SEL_Msk) >> RCC_IC6CFGR_IC6SEL_Pos,
			((ic6 & RCC_IC6CFGR_IC6INT_Msk) >> RCC_IC6CFGR_IC6INT_Pos) + 1U);
	ok11 = show_ic("IC11 (NPU RAM)", (diven & RCC_DIVENR_IC11EN_Msk) != 0,
			(ic11 & RCC_IC11CFGR_IC11SEL_Msk) >> RCC_IC11CFGR_IC11SEL_Pos,
			((ic11 & RCC_IC11CFGR_IC11INT_Msk) >> RCC_IC11CFGR_IC11INT_Pos) + 1U);

	return (sysws == 3U) && ok6 && ok11;
}

/* ---------------------------------------------------------------- */
/* 1. 内部メモリ                                                      */
/* ---------------------------------------------------------------- */

LOCAL const struct {
	RAMCFG_TypeDef	*cfg;
	const char	*name;
} axisram[] = {
	{ RAMCFG_SRAM2_AXI, "AXISRAM2" },	/* アプリの RAM の後半。電源は入っているはず */
	{ RAMCFG_SRAM3_AXI, "AXISRAM3" },
	{ RAMCFG_SRAM4_AXI, "AXISRAM4" },
	{ RAMCFG_SRAM5_AXI, "AXISRAM5" },
	{ RAMCFG_SRAM6_AXI, "AXISRAM6" },
};

/*
 * AXISRAM3〜6 と NPU キャッシュ RAM のクロックを入れ、AXISRAM2〜6 の電源を入れる
 * (Int_Mem_Config と同じ)。SVD 上はリセット直後から AXISRAM3〜6 のクロックも電源も
 * 入っているが、ST はブート後に切れている前提で書いているので、読んでから必要な分だけ書く。
 */
LOCAL BOOL enable_internal_ram(void)
{
	UW	before, after, cr;
	INT	i;
	BOOL	ok;

	/* RAMCFG のレジスタのクロック (リセット直後から有効。AHB2ENSR への書き込みなので冪等) */
	__HAL_RCC_RAMCFG_CLK_ENABLE();

	/* メモリのクロック (RCC MEMENR。MEMENSR に立てたいビットだけを書く) */
	before = RCC->MEMENR;
	if((before & NPU_MEMEN) != NPU_MEMEN) {
		__HAL_RCC_AXISRAM2_MEM_CLK_ENABLE();
		__HAL_RCC_AXISRAM3_MEM_CLK_ENABLE();
		__HAL_RCC_AXISRAM4_MEM_CLK_ENABLE();
		__HAL_RCC_AXISRAM5_MEM_CLK_ENABLE();
		__HAL_RCC_AXISRAM6_MEM_CLK_ENABLE();
		__HAL_RCC_CACHEAXIRAM_MEM_CLK_ENABLE();
	}
	after = RCC->MEMENR;
	tm_printf((UB*)"  RCC MEMENR 0x%08x -> 0x%08x (AXISRAM2-6EN=%d%d%d%d%d CACHEAXIRAMEN=%d)%s\n",
			before, after,
			(after & RCC_MEMENR_AXISRAM2EN) ? 1 : 0, (after & RCC_MEMENR_AXISRAM3EN) ? 1 : 0,
			(after & RCC_MEMENR_AXISRAM4EN) ? 1 : 0, (after & RCC_MEMENR_AXISRAM5EN) ? 1 : 0,
			(after & RCC_MEMENR_AXISRAM6EN) ? 1 : 0, (after & RCC_MEMENR_CACHEAXIRAMEN) ? 1 : 0,
			((before & NPU_MEMEN) == NPU_MEMEN) ? " already enabled" : "");
	ok = npu_step("memory clocks (AXISRAM2-6, CACHEAXIRAM)", (after & NPU_MEMEN) == NPU_MEMEN);

	/*
	 * 電源 (シャットダウン解除)。HAL_RAMCFG_EnableAXISRAM() と同じく SRAMSD を落とす。
	 * 立っているときだけ書き、SRAMSD を立てる方向には決して書かない
	 * (AXISRAM2 はアプリの RAM とスタック。落とすと内容が消えて即座に止まる)
	 */
	for(i = 0; i < (INT)(sizeof(axisram) / sizeof(axisram[0])); i++) {
		cr = axisram[i].cfg->CR;
		if(cr & RAMCFG_CR_SRAMSD) {
			axisram[i].cfg->CR = cr & ~RAMCFG_CR_SRAMSD;
		}
		tm_printf((UB*)"  RAMCFG %s CR 0x%08x -> 0x%08x%s\n", axisram[i].name,
				cr, axisram[i].cfg->CR,
				(cr & RAMCFG_CR_SRAMSD) ? " (was shut down)" : " (already powered)");
		if(axisram[i].cfg->CR & RAMCFG_CR_SRAMSD) ok = FALSE;
	}
	__DSB();
	/* 電源投入後に待ちが要るかは資料が無く不明 (HAL は待たない)。念のため 1ms 置く */
	tk_dly_tsk(1);

	return npu_step("AXISRAM2-6 power on (RAMCFG SRAMSD=0)", ok);
}

/* RAM の読み書き確認の結果 (割り込み禁止中は表示できないので、ここに残して後で出す) */
typedef struct {
	UW	base;
	UW	bad;		/* 一致しなかった語数 */
	UW	bad_addr;	/* 最初に一致しなかった番地 */
	UW	bad_read;
	UW	bad_exp;
} RAM_TEST;

/*
 * base から RAM_TEST_WORDS 語を P と ~P の2通りで書いて読み戻す。
 * P は番地で変える (番地線の取り違えも拾う)。2通り書くのは、前回の実行で
 * 書いた値が RAM に残っていて偶然一致するのを避けるため。
 * 最初に1語読むのは、RAM が使えない場合のバスエラーを precise にするため
 * (最初のアクセスが書き込みだと、エラーがライトバッファ経由で imprecise になり得る)。
 * D キャッシュを止め、割り込みを禁止した状態で呼ぶ (ram_check)。表示はしない。
 */
LOCAL void ram_pattern_test(RAM_TEST *t)
{
	volatile UW	*p = (volatile UW *)t->base;
	UW		exp, v;
	INT		pass, i;

	t->bad = 0;
	(void)p[0];
	__DSB();

	for(pass = 0; pass < 2; pass++) {
		for(i = 0; i < RAM_TEST_WORDS; i++) {
			exp = RAM_TEST_PATTERN ^ (UW)&p[i];
			p[i] = pass ? ~exp : exp;
		}
		__DSB();
		for(i = 0; i < RAM_TEST_WORDS; i++) {
			exp = RAM_TEST_PATTERN ^ (UW)&p[i];
			if(pass) exp = ~exp;
			v = p[i];
			if(v != exp) {
				if(t->bad == 0) {
					t->bad_addr = (UW)&p[i];
					t->bad_read = v;
					t->bad_exp  = exp;
				}
				t->bad++;
			}
		}
	}
}

LOCAL BOOL ram_show(const RAM_TEST *t, const char *name)
{
	if(t->bad != 0) {
		tm_printf((UB*)"  [ram] %s first mismatch @ 0x%08x: read 0x%08x expect 0x%08x\n",
				name, t->bad_addr, t->bad_read, t->bad_exp);
	}
	tm_printf((UB*)"  [ram] %s @ 0x%08x: %s (%u mismatches in %d words x 2 patterns)\n",
			name, t->base, (t->bad == 0) ? "MATCH" : "MISMATCH", t->bad, RAM_TEST_WORDS);
	return (t->bad == 0);
}

/*
 * D キャッシュを止めて RAM に直接読み書きする (キャッシュから読み戻すだけに
 * ならないように。flash_probe と同じ理由)。
 * SCB_DisableDCache() は CCR.DC を先に落としてから全体を clean+invalidate する
 * (armv7m_cachel1.h)。その間に割り込み (HAL ティックの周期ハンドラ等) が走ると、
 * 古い値を RAM から読んだり、書いた値を後の clean で上書きされたりする。
 * カーネルもキャッシュ操作は割り込み禁止で行う前提 (armv8m/cache.c)。
 * そこで止めてから戻すまでを DI/EI で囲む。中では表示しない (数十 µs で終わる)。
 * テスト後は SCB_EnableDCache() が全体を invalidate するので、テストした番地
 * (AXISRAM6 は NPU の activations) に古いラインは残らない。
 */
LOCAL BOOL ram_check(void)
{
	/* 各バンクの先頭 (0x34200000 と 0x34350000 が今回の確認対象。4・5 も同じ手間で見る) */
	static const struct {
		UW		base;
		const char	*name;
	} bank[] = {
		{ NPU_AXISRAM3_BASE, "AXISRAM3" },
		{ 0x34270000U,       "AXISRAM4" },
		{ 0x342E0000U,       "AXISRAM5" },
		{ NPU_AXISRAM6_BASE, "AXISRAM6" },
	};
	RAM_TEST	t[sizeof(bank) / sizeof(bank[0])];
	BOOL		dcache_on, ok = TRUE;
	UINT		imask;
	INT		i, n = (INT)(sizeof(bank) / sizeof(bank[0]));

	/* フォルトした場合に UART に残る最後の行がこれになる */
	tm_printf((UB*)"  [ram] testing AXISRAM3-6 heads 0x%08x..0x%08x (D-cache off, interrupts masked) ...\n",
			bank[0].base, bank[n - 1].base);

	for(i = 0; i < n; i++) t[i].base = bank[i].base;

	DI(imask);
	dcache_on = ((SCB->CCR & SCB_CCR_DC_Msk) != 0);
	if(dcache_on) SCB_DisableDCache();

	for(i = 0; i < n; i++) ram_pattern_test(&t[i]);

	if(dcache_on) SCB_EnableDCache();
	EI(imask);

	for(i = 0; i < n; i++) {
		if(!ram_show(&t[i], bank[i].name)) ok = FALSE;
	}
	return npu_step("AXISRAM3-6 pattern write/read", ok);
}

/* ---------------------------------------------------------------- */
/* 2. NPU と NPU キャッシュ                                            */
/* ---------------------------------------------------------------- */

/* NPU のバスクロックを入れてリセットする (NPU_Config の前半) */
LOCAL BOOL npu_clock_reset(void)
{
	UW	en  = RCC->AHB5ENR;
	UW	rst = RCC->AHB5RSTR;

	tm_printf((UB*)"  before: AHB5ENR NPUEN=%d CACHEAXIEN=%d  AHB5RSTR NPURST=%d CACHEAXIRST=%d%s\n",
			(en  & RCC_AHB5ENR_NPUEN_Msk) ? 1 : 0, (en  & RCC_AHB5ENR_CACHEAXIEN_Msk) ? 1 : 0,
			(rst & RCC_AHB5RSTR_NPURST_Msk) ? 1 : 0, (rst & RCC_AHB5RSTR_CACHEAXIRST_Msk) ? 1 : 0,
			(en & (RCC_AHB5ENR_NPUEN_Msk | RCC_AHB5ENR_CACHEAXIEN_Msk))
				? " (left enabled by an earlier run, reset anyway)" : "");

	__HAL_RCC_NPU_CLK_ENABLE();
	__HAL_RCC_NPU_FORCE_RESET();
	__HAL_RCC_NPU_RELEASE_RESET();
	(void)RCC->AHB5RSTR;
	__DSB();

	return npu_step("NPU clock on, reset released",
			__HAL_RCC_NPU_IS_CLK_ENABLED() && ((RCC->AHB5RSTR & RCC_AHB5RSTR_NPURST_Msk) == 0));
}

/*
 * NPU キャッシュ (CACHEAXI) のクロックを入れてリセットし、有効にする
 * (NPU_Config の CACHEAXI 部分と npu_cache_enable())。
 * リセット直後は全無効化が自動で走っている (SR のリセット値 BUSYF=1、SVD) ので、
 * 終わるのを待ってから CR1.EN を立てる。明示的な無効化 (CACHEINV) は要らない。
 * ST の HAL_CACHEAXI_Init / Enable も無効化はしていない。
 */
LOCAL BOOL npu_cache_enable(void)
{
	UW	t0, dt, lim;
	INT	reads = 0;
	BOOL	busy;

	__HAL_RCC_CACHEAXI_CLK_ENABLE();
	__HAL_RCC_CACHEAXI_FORCE_RESET();
	__HAL_RCC_CACHEAXI_RELEASE_RESET();
	(void)RCC->AHB5RSTR;
	__DSB();

	lim = trace_cyc_per_us() * CACHEAXI_WAIT_US;
	t0  = NOW();
	for(;;) {
		busy = ((CACHEAXI->SR & CACHEAXI_SR_BUSYF_Msk) != 0);
		dt   = (UW)(NOW() - t0);
		if(!busy || dt > lim || ++reads >= NPU_WAIT_MAX_READS) break;
	}
	tm_printf((UB*)"  CACHEAXI after reset: SR=0x%08x (BUSYF=%d) after %u us\n",
			CACHEAXI->SR, busy ? 1 : 0, trace_cyc_to_us(dt));
	if(busy) {
		return npu_step("NPU cache: auto invalidate after reset finished", FALSE);
	}

	CACHEAXI->CR1 |= CACHEAXI_CR1_EN_Msk;
	__DSB();
	tm_printf((UB*)"  CACHEAXI CR1=0x%08x SR=0x%08x\n", CACHEAXI->CR1, CACHEAXI->SR);

	return npu_step("NPU cache (CACHEAXI) enabled",
			((CACHEAXI->CR1 & CACHEAXI_CR1_EN_Msk) != 0) && ((CACHEAXI->SR & CACHEAXI_SR_ERRF_Msk) == 0));
}

/* ---------------------------------------------------------------- */
/* 3. RIF                                                             */
/* ---------------------------------------------------------------- */

/*
 * RIMC: NPU をバスマスタとして CID1・セキュア・特権にする
 *       (HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_NPU, {RIF_CID_1, SEC|PRIV}) と同じ)
 * RISC: NPU のレジスタをセキュア・特権からだけ触れるようにする
 *       (HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_NPU, PRIV|SEC) と同じ)
 * このアプリはセキュア・特権で動いている (-mcmse、μT-Kernel は CONTROL.nPRIV を立てない)
 * ので、RISC 設定後も CPU から NPU のレジスタを触れる。
 * CACHEAXI の設定レジスタは NPU とは別の RIF 番号 (CACHECONFIG) で、ここでは
 * 保護しない (ST も設定していない)。
 */
LOCAL BOOL npu_rif_config(void)
{
	UW	rimc_cr, risc_cr, rlock, attr0, sec0, priv0;
	UW	attr, sec, priv;

	/* RIFSC のクロック。リセット直後から有効で、main.c の MX_ADC1_Init も有効化しているが、
	 * それに頼らない (AHB3ENSR への書き込みなので冪等) */
	__HAL_RCC_RIFSC_CLK_ENABLE();

	rimc_cr = RIFSC->RIMC_CR;
	risc_cr = RIFSC->RISC_CR;
	rlock   = RIFSC->RISC_RCFGLOCKRx[NPU_RISC_REG];
	attr0   = RIFSC->RIMC_ATTRx[NPU_RIMC_INDEX];
	sec0    = RIFSC->RISC_SECCFGRx[NPU_RISC_REG];
	priv0   = RIFSC->RISC_PRIVCFGRx[NPU_RISC_REG];

	tm_printf((UB*)"  locks: RIMC_CR.GLOCK=%d RISC_CR.GLOCK=%d RISC RLOCK(NPU)=%d\n",
			(rimc_cr & RIFSC_RIMC_CR_GLOCK_Msk) ? 1 : 0,
			(risc_cr & RIFSC_RISC_CR_GLOCK_Msk) ? 1 : 0,
			(rlock & NPU_RISC_RLOCK) ? 1 : 0);

	/* RIMC: MCID / MSEC / MPRIV だけを書き換える */
	RIFSC->RIMC_ATTRx[NPU_RIMC_INDEX] = (attr0 & ~NPU_RIMC_MASK) | NPU_RIMC_VALUE;

	/* RISC: HAL と同じくセキュア → 特権の順 */
	RIFSC->RISC_SECCFGRx[NPU_RISC_REG]  |= NPU_RISC_SEC;
	RIFSC->RISC_PRIVCFGRx[NPU_RISC_REG] |= NPU_RISC_PRIV;
	__DSB();

	attr = RIFSC->RIMC_ATTRx[NPU_RIMC_INDEX];
	sec  = RIFSC->RISC_SECCFGRx[NPU_RISC_REG];
	priv = RIFSC->RISC_PRIVCFGRx[NPU_RISC_REG];
	tm_printf((UB*)"  RIMC_ATTR%d 0x%08x -> 0x%08x (MCID=%u MSEC=%d MPRIV=%d)\n", NPU_RIMC_INDEX, attr0, attr,
			(attr & RIFSC_RIMC_ATTRx_MCID_Msk) >> RIFSC_RIMC_ATTRx_MCID_Pos,
			(attr & RIFSC_RIMC_ATTRx_MSEC_Msk) ? 1 : 0, (attr & RIFSC_RIMC_ATTRx_MPRIV_Msk) ? 1 : 0);
	tm_printf((UB*)"  RISC_SECCFGR%d 0x%08x -> 0x%08x  RISC_PRIVCFGR%d 0x%08x -> 0x%08x (NPU = bit10)\n",
			NPU_RISC_REG, sec0, sec, NPU_RISC_REG, priv0, priv);

	(void)npu_step("RIMC: NPU master = CID1, secure, privileged", (attr & NPU_RIMC_MASK) == NPU_RIMC_VALUE);
	(void)npu_step("RISC: NPU registers = secure, privileged only", (sec & NPU_RISC_SEC) && (priv & NPU_RISC_PRIV));

	return ((attr & NPU_RIMC_MASK) == NPU_RIMC_VALUE) && (sec & NPU_RISC_SEC) && (priv & NPU_RISC_PRIV);
}

/* ---------------------------------------------------------------- */
/* 確認: NPU のレジスタを1つ読む                                        */
/* ---------------------------------------------------------------- */

/*
 * ATON CLKCTRL VERSION を読む。ATON の内部クロック (CLKCTRL_CTRL) は ll_aton の
 * LL_ATON_Init() が入れるもので、ここでは書かない。ll_aton はクロックを入れた後
 * (ll_aton.c:179-181) でも「0 の間は読み直す」ので (ll_aton.c:40-47, CLKCTRL は :244)、
 * ここでも 0 の間は上限付きで読み直す。
 * 判定は ll_aton と同じく TYPE / MAJOR / MINOR。上位 (AGATES/BGATES/CLKDIV) は
 * 構成で変わり得るので参考表示だけにする。
 */
LOCAL BOOL npu_read_id(void)
{
	UW	v, t0, lim;
	INT	reads = 0;
	BOOL	id_match;

	lim = trace_cyc_per_us() * NPU_VERSION_WAIT_US;

	/* フォルトした場合に UART に残る最後の行がこれになる */
	tm_printf((UB*)"  [npu] reading ATON CLKCTRL VERSION @ 0x%08x ...\n", (UW)ATON_CLKCTRL_VERSION);
	t0 = NOW();
	do {
		v = *(const volatile UW *)ATON_CLKCTRL_VERSION;
		reads++;
	} while(v == 0U && (UW)(NOW() - t0) <= lim && reads < NPU_WAIT_MAX_READS);
	__DSB();

	/* ここに来たということはフォルトしていない */
	id_match = ((v & ATON_VERSION_ID_MASK) == (ATON_CLKCTRL_VERSION_DT & ATON_VERSION_ID_MASK));
	tm_printf((UB*)"  [npu] no fault. VERSION=0x%08x after %d reads (TYPE=0x%x MAJOR=%u MINOR=%u AGATES=%u BGATES=0x%x)"
			" expect 0x%08x -> %s\n",
			v, reads, v & 0xFFU, (v >> 12) & 0xFU, (v >> 8) & 0xFU, (v >> 16) & 0xFU, (v >> 20) & 0xFFU,
			ATON_CLKCTRL_VERSION_DT,
			(v == ATON_CLKCTRL_VERSION_DT) ? "MATCH" :
			id_match ? "TYPE/MAJOR/MINOR match" :
			(v == 0U) ? "ZERO (not decided: ATON clocks not enabled until LL_ATON_Init?)" : "MISMATCH");

	return npu_step("NPU register read (TYPE/MAJOR/MINOR as ll_aton expects)", id_match);
}

#if NPU_RISAF_DUMP
/* ---------------------------------------------------------------- */
/* RISAF の状態 (読むだけ)                                             */
/* ---------------------------------------------------------------- */

/*
 * CR (GLOCK)、IASR (不正アクセスのフラグ)、有効なリージョン (REG[i].CFGR.BREN)。
 * リージョン数は ST の IS_RISAF_MAX_REGION (stm32n6xx_hal_rif.h:1337-) より。
 * 期待値は全部 0 (= 既定の「セキュア・特権なら通す」のまま)。
 */
LOCAL void show_risaf(const char *name, RISAF_TypeDef *r, INT nreg)
{
	UW	bren = 0;
	INT	i;

	for(i = 0; i < nreg; i++) {
		if(r->REG[i].CFGR & RISAF_REGx_CFGR_BREN_Msk) bren |= (1U << i);
	}
	tm_printf((UB*)"  %s: CR=0x%08x IASR=0x%08x BREN(region bits)=0x%03x\n", name, r->CR, r->IASR, bren);
}

LOCAL void show_risaf_all(void)
{
	tm_printf((UB*)"  RISAF (expect all 0 = default filtering, secure+privileged pass):\n");
	show_risaf("RISAF2  AXISRAM1   ", RISAF2_S, 7);
	show_risaf("RISAF3  AXISRAM2   ", RISAF3_S, 7);
	show_risaf("RISAF4  NPU MST0   ", RISAF4_S, 11);
	show_risaf("RISAF5  NPU MST1   ", RISAF5_S, 11);
	show_risaf("RISAF6  AXISRAM3-6 ", RISAF6_S, 11);
	/* XSPI2 のクロックは extflash_init が入れる。入っていなければ読まない */
	if(__HAL_RCC_XSPI2_IS_CLK_ENABLED()) {
		show_risaf("RISAF12 XSPI2 flash", RISAF12_S, 7);
	} else {
		tm_printf((UB*)"  RISAF12 XSPI2 flash: (XSPI2 clock off, not read)\n");
	}
}
#endif	/* NPU_RISAF_DUMP */

/* ---------------------------------------------------------------- */

EXPORT ER npu_hw_init(void)
{
	BOOL	clk_ok, mem_ok, ram_ok = FALSE, npu_ok = FALSE, cache_ok = FALSE, rif_ok, id_ok = FALSE;

	tm_printf((UB*)"npu hw init (AXISRAM3-6, NPU, CACHEAXI, RIF)\n");

	/* 状態 (読むだけ) */
	show_cpu_cache();
	clk_ok = npu_step("NPU clocks enabled by FSBL (SYSSWS=IC2/IC6/IC11, IC6EN, IC11EN)", check_npu_clocks());

	/* 1. 内部メモリ */
	mem_ok = enable_internal_ram();
	if(!clk_ok) {
		/* IC11 が止まっていると AXISRAM3〜6 へのアクセスはハングし得る */
		npu_skip("AXISRAM3-6 pattern write/read", "NPU RAM clock IC11 not confirmed");
	} else if(!mem_ok) {
		npu_skip("AXISRAM3-6 pattern write/read", "memory not enabled");
	} else {
		ram_ok = ram_check();
	}

	/* 2. NPU と NPU キャッシュ (IC6 / IC11 が止まっているなら触らない) */
	if(clk_ok) {
		npu_ok = npu_clock_reset();
		if(!npu_ok) {
			npu_skip("NPU cache (CACHEAXI) enable", "NPU clock/reset failed");
		} else if((RCC->MEMENR & RCC_MEMENR_CACHEAXIRAMEN) == 0) {
			npu_skip("NPU cache (CACHEAXI) enable", "CACHEAXIRAM clock off");
		} else {
			cache_ok = npu_cache_enable();
		}
	} else {
		npu_skip("NPU clock/reset, NPU cache", "NPU clocks IC6/IC11 not confirmed");
	}

	/* 3. RIF (RIFSC のレジスタだけなので NPU のクロックとは関係なく書ける) */
	rif_ok = npu_rif_config();

	/* 確認: RISC 設定後に NPU のレジスタを読む */
	if(clk_ok && npu_ok) {
		id_ok = npu_read_id();
	} else {
		npu_skip("NPU register read", "NPU not clocked");
	}

#if NPU_RISAF_DUMP
	/* RISAF4/5 は NPU のマスタポート側にあるので、NPU にクロックが入ってから読む */
	if(clk_ok && npu_ok) {
		show_risaf_all();
	} else {
		npu_skip("RISAF dump", "NPU not clocked");
	}
#endif

	ready = clk_ok && mem_ok && ram_ok && npu_ok && cache_ok && rif_ok && id_ok;
	tm_printf((UB*)"npu hw %s (clk=%d mem=%d ram=%d npu=%d cache=%d rif=%d id=%d)\n",
			ready ? "ready" : "NOT ready",
			clk_ok, mem_ok, ram_ok, npu_ok, cache_ok, rif_ok, id_ok);
	return ready ? E_OK : E_IO;
}

EXPORT BOOL npu_hw_ready(void)
{
	return ready;
}
