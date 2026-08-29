/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
ADC_HandleTypeDef hadc2;

I2C_HandleTypeDef hi2c1;

/* USER CODE BEGIN PV */

/* I2C2 is not enabled in the .ioc, so CubeMX generated no hi2c2/init code
 * for it. It is brought up manually below (MX_I2C2_Init) because the
 * WM8904 audio codec is wired to I2C2 (PD14/PD4), not I2C1. */
I2C_HandleTypeDef hi2c2;

/* SAI1 is likewise not enabled in the .ioc. g_sai1_status starts at
 * HAL_ERROR so any code that checks it before MX_SAI1_Init() runs sees a
 * defined "not ready" value rather than uninitialized memory. */
SAI_HandleTypeDef hsai1;
HAL_StatusTypeDef g_sai1_status = HAL_ERROR;

/* SAI1 Tx DMA (GPDMA1 Channel 2, circular linked-list -> continuous
 * double-buffered playback). Needs to be a plain (non-static) global
 * because stm32n6xx_it.c's GPDMA1_Channel2_IRQHandler() must reach it. */
DMA_HandleTypeDef hDmaSaiTx;

/* MDF1 is likewise not enabled in the .ioc. */
MDF_HandleTypeDef hmdf1;
HAL_StatusTypeDef g_mdf1_status = HAL_ERROR;
uint32_t g_mdf1_step = 0;

/* MDF1 filter0 Rx DMA (GPDMA1 Channel 0, circular linked-list -> continuous
 * double-buffered capture). Plain global for the same reason as hDmaSaiTx:
 * stm32n6xx_it.c's GPDMA1_Channel0_IRQHandler() must reach it. */
DMA_HandleTypeDef hDmaMdf;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_ADC2_Init(void);
static void MX_I2C1_Init(void);
/* USER CODE BEGIN PFP */

static void MX_I2C2_Init(void);
static void MPU_Config(void);
static void MX_SAI1_Init(void);
static void MX_MDF1_Init(void);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/*
 * I2C2 Initialization Function (WM8904 audio codec control interface)
 *
 * Not part of the CubeMX-generated peripherals (I2C2 is unchecked in the
 * .ioc), so there is no generated MX_I2C2_Init(). Added by hand here,
 * mirroring MX_I2C1_Init() above, so that Appli/Application/ code can talk
 * to the WM8904 over I2C2 (PD14=SCL, PD4=SDA) via HAL_I2C_Mem_Read/Write.
 */
static void MX_I2C2_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOD_CLK_ENABLE();

  GPIO_InitStruct.Pin = I2C2_SCL_Pin | I2C2_SDA_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF4_I2C2;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  __HAL_RCC_I2C2_CLK_ENABLE();

  hi2c2.Instance = I2C2;
  hi2c2.Init.Timing = 0x30C0EDFF;
  hi2c2.Init.OwnAddress1 = 0;
  hi2c2.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c2.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c2.Init.OwnAddress2 = 0;
  hi2c2.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c2.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c2.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c2) != HAL_OK)
  {
    Error_Handler();
  }

  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c2, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c2, 0) != HAL_OK)
  {
    Error_Handler();
  }
}

/*
 * MPU configuration: marks the .noncacheable linker section (already
 * present in STM32N657X0HXQ_LRUN.ld, __snoncacheable/__enoncacheable,
 * but unused until now) as Normal/Non-cacheable memory, for data that a
 * DMA engine's own bus-master reads/writes directly and that the CPU
 * cache would otherwise hide (see the cache note on SaiTxNode/SaiTxQueue
 * in MX_SAI1_Init() below for why a manual SCB_CleanDCache_by_Addr() is
 * not enough for those two structures specifically). Not CubeMX-generated
 * (no MPU_Config() existed before -- SAI1/GPDMA-for-SAI are not in the
 * .ioc); written using the standard CMSIS/HAL MPU API and the same
 * ARM_MPU_ATTR_NON_CACHEABLE encoding CubeMX itself uses for this pattern
 * on other STM32 families, not guessed from scratch. MPU_HFNMI_PRIVDEF
 * is required when enabling the MPU with only one explicit region so that
 * every other address (everything not in .noncacheable) keeps falling
 * back to the default memory map instead of faulting.
 */
static void MPU_Config(void)
{
  MPU_Attributes_InitTypeDef attr = {0};
  MPU_Region_InitTypeDef     region = {0};

  HAL_MPU_Disable();

  attr.Number     = MPU_ATTRIBUTES_NUMBER0;
  attr.Attributes = ARM_MPU_ATTR(ARM_MPU_ATTR_NON_CACHEABLE, ARM_MPU_ATTR_NON_CACHEABLE);
  HAL_MPU_ConfigMemoryAttributes(&attr);

  region.Enable          = MPU_REGION_ENABLE;
  region.Number          = MPU_REGION_NUMBER0;
  region.AttributesIndex = MPU_ATTRIBUTES_NUMBER0;
  region.BaseAddress     = __NON_CACHEABLE_SECTION_BEGIN;
  region.LimitAddress    = __NON_CACHEABLE_SECTION_END - 1U;
  region.AccessPermission = MPU_REGION_ALL_RW;
  region.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
  region.DisablePrivExec  = MPU_PRIV_INSTRUCTION_ACCESS_DISABLE;
  region.IsShareable      = MPU_ACCESS_OUTER_SHAREABLE;
  HAL_MPU_ConfigRegion(&region);

  HAL_MPU_Enable(MPU_HFNMI_PRIVDEF);
}

/*
 * SAI1 Initialization Function (WM8904 audio data path)
 *
 * SAI1 is unchecked in the .ioc, so there is no generated MX_SAI1_Init()
 * for the Appli target (it only exists for FSBL, for a different purpose).
 * Added by hand here, following the same "clock/GPIO/HAL_*_Init only in
 * main.c" pattern as MX_I2C2_Init() above.
 *
 * Clock tree (PLL2 -> IC7 -> SAI1 kernel clock) and the SAI_InitTypeDef /
 * FrameInit / SlotInit field values below are copied from ST's official
 * STM32N6570-DK BSP (stm32n6570-dk-bsp repo, stm32n6570_discovery_audio.c:
 * MX_SAI1_ClockConfig() / MX_SAI1_Init() / SAI_MspInit(), 16kHz-group PLL2
 * settings), not derived from the datasheet. SAI1_Block_A is configured as
 * I2S-format master transmitter (FS=PB0, SCK=PB6, SD=PB7, MCLK=PG7, AF6),
 * 16kHz/16bit, with MCLK output enabled, matching the WM8904 side
 * (AUDIO_INTERFACE1 = 16bit/I2S, CLOCK_RATES1 = 16kHz) configured in
 * audio/wm8904.c.
 *
 * Unlike MX_I2C1_Init()/MX_I2C2_Init(), this function does NOT call
 * Error_Handler() on failure: tm_printf() is not safe to use before
 * knl_start_mtkernel() runs (see the g_sai1_status comment in main.h), so
 * there would be no way to report *why* Error_Handler()'s silent
 * `while(1)` was hit. Instead every failure just returns early, leaving
 * g_sai1_status at HAL_ERROR (or whatever HAL_RCC_OscConfig/
 * HAL_RCCEx_PeriphCLKConfig/HAL_SAI_Init returned) for Application/ code
 * to check and report over UART once the kernel (and libtm_init) is up.
 *
 * Also brings up SAI1's Tx DMA channel (GPDMA1 Channel 2) in circular
 * linked-list mode, so a single HAL_SAI_Transmit_DMA() call plays a
 * double buffer indefinitely (Application/audio/ refills each half from
 * HAL_SAI_TxHalfCpltCallback/HAL_SAI_TxCpltCallback). GPDMA1/HPDMA1/
 * LINKEDLIST were already enabled in the .ioc (unlike SAI1), so
 * HAL_DMA_MODULE_ENABLED and hal_dma.c/hal_dma_ex.c were already wired
 * into the build -- this part didn't need the .project-link workaround
 * SAI1 itself needed. The GPDMA node/queue/channel setup below (node
 * config fields, linked-list init sequence) is copied from the same
 * STM32N6570-DK BSP's SAI_MspInit() DMA block, not derived from the
 * reference manual.
 *
 * Cache note: SaiTxNode/SaiTxQueue below are tagged __NON_CACHEABLE (see
 * MPU_Config() further down in this file, called from main() before the
 * caches are enabled) instead of being manually cache-cleaned. A manual
 * clean does not work for this specific pair of structures: GPDMA's
 * circular linked-list re-reads the head node from RAM on every loop (not
 * just once at setup), and HAL_SAI_Transmit_DMA() itself (Drivers/, not
 * ours to edit) writes this node's LinkRegisters right before starting
 * the channel, leaving no hook to clean afterwards. The actual PCM sample
 * buffer is a different case -- Application/audio/audio_task.c fully
 * controls the write-then-read ordering there, so SCB_CleanDCache_by_Addr()
 * right after each refill is sufficient and that buffer stays in normal
 * cacheable SRAM.
 */
static void MX_SAI1_Init(void)
{
  RCC_OscInitTypeDef       RCC_OscInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
  GPIO_InitTypeDef         GPIO_InitStruct = {0};
  static DMA_NodeTypeDef   SaiTxNode  __NON_CACHEABLE;
  static DMA_QListTypeDef  SaiTxQueue __NON_CACHEABLE;
  DMA_NodeConfTypeDef      dmaNodeConfig = {0};

  g_sai1_status = HAL_ERROR;

  /* SAI1 kernel clock: PLL2 -> IC7, 16kHz group (ClockDivider = 1) */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_NONE;
  RCC_OscInitStruct.PLL1.PLLState = RCC_PLL_NONE;
  RCC_OscInitStruct.PLL2.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL2.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL2.PLLFractional = 0;
  RCC_OscInitStruct.PLL2.PLLM = 6;
  RCC_OscInitStruct.PLL2.PLLN = 172;
  RCC_OscInitStruct.PLL2.PLLP1 = 7;
  RCC_OscInitStruct.PLL2.PLLP2 = 4;
  RCC_OscInitStruct.PLL3.PLLState = RCC_PLL_NONE;
  RCC_OscInitStruct.PLL4.PLLState = RCC_PLL_NONE;
  g_sai1_status = HAL_RCC_OscConfig(&RCC_OscInitStruct);
  if (g_sai1_status != HAL_OK)
  {
    return;
  }

  PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_SAI1;
  PeriphClkInitStruct.Sai1ClockSelection = RCC_SAI1CLKSOURCE_IC7;
  PeriphClkInitStruct.ICSelection[RCC_IC7].ClockSelection = RCC_ICCLKSOURCE_PLL2;
  PeriphClkInitStruct.ICSelection[RCC_IC7].ClockDivider = 1;
  g_sai1_status = HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct);
  if (g_sai1_status != HAL_OK)
  {
    return;
  }

  /* SAI1_Block_A pins: FS=PB0, SCK=PB6, SD=PB7, MCLK=PG7 (AF6) */
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull      = GPIO_NOPULL;
  GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF6_SAI1;

  GPIO_InitStruct.Pin = SAI1_FS_A_Pin;
  HAL_GPIO_Init(SAI1_FS_A_GPIO_Port, &GPIO_InitStruct);
  GPIO_InitStruct.Pin = SAI1_SCK_A_Pin;
  HAL_GPIO_Init(SAI1_SCK_A_GPIO_Port, &GPIO_InitStruct);
  GPIO_InitStruct.Pin = SAI1_SD_A_Pin;
  HAL_GPIO_Init(SAI1_SD_A_GPIO_Port, &GPIO_InitStruct);
  GPIO_InitStruct.Pin = SAI1_MCLK_A_Pin;
  HAL_GPIO_Init(SAI1_MCLK_A_GPIO_Port, &GPIO_InitStruct);

  __HAL_RCC_SAI1_CLK_ENABLE();

  /* SAI1_Block_A: master transmitter, 16kHz/16bit, I2S-equivalent frame,
   * MCLK output enabled */
  hsai1.Instance = SAI1_Block_A;
  hsai1.Init.MonoStereoMode    = SAI_STEREOMODE;
  hsai1.Init.AudioFrequency    = SAI_AUDIO_FREQUENCY_16K;
  hsai1.Init.AudioMode         = SAI_MODEMASTER_TX;
  hsai1.Init.NoDivider         = SAI_MASTERDIVIDER_ENABLE;
  hsai1.Init.Protocol          = SAI_FREE_PROTOCOL;
  hsai1.Init.DataSize          = SAI_DATASIZE_16;
  hsai1.Init.FirstBit          = SAI_FIRSTBIT_MSB;
  hsai1.Init.ClockStrobing     = SAI_CLOCKSTROBING_FALLINGEDGE;
  hsai1.Init.Synchro           = SAI_ASYNCHRONOUS;
  hsai1.Init.OutputDrive       = SAI_OUTPUTDRIVE_ENABLE;
  hsai1.Init.FIFOThreshold     = SAI_FIFOTHRESHOLD_1QF;
  hsai1.Init.SynchroExt        = SAI_SYNCEXT_DISABLE;
  hsai1.Init.CompandingMode    = SAI_NOCOMPANDING;
  hsai1.Init.TriState          = SAI_OUTPUT_NOTRELEASED;
  hsai1.Init.Mckdiv            = 0U;
  hsai1.Init.MckOutput         = SAI_MCK_OUTPUT_ENABLE;
  hsai1.Init.MckOverSampling   = SAI_MCK_OVERSAMPLING_DISABLE;
  hsai1.Init.PdmInit.Activation = DISABLE;

  hsai1.FrameInit.FrameLength       = 32;
  hsai1.FrameInit.ActiveFrameLength = 16;
  hsai1.FrameInit.FSDefinition      = SAI_FS_CHANNEL_IDENTIFICATION;
  hsai1.FrameInit.FSPolarity        = SAI_FS_ACTIVE_LOW;
  hsai1.FrameInit.FSOffset          = SAI_FS_BEFOREFIRSTBIT;

  hsai1.SlotInit.FirstBitOffset = 0;
  hsai1.SlotInit.SlotSize       = SAI_SLOTSIZE_16B;
  hsai1.SlotInit.SlotNumber     = 2;
  hsai1.SlotInit.SlotActive     = SAI_SLOTACTIVE_0 | SAI_SLOTACTIVE_1;

  g_sai1_status = HAL_SAI_Init(&hsai1);
  if (g_sai1_status != HAL_OK)
  {
    return;
  }

  /* SAI1 Tx DMA: GPDMA1 Channel 2, one linear node, queue set circular so
   * the whole (double-buffer) region plays on repeat until HAL_SAI_DMAStop()
   * is called. */
  __HAL_RCC_GPDMA1_CLK_ENABLE();

  hDmaSaiTx.Instance = GPDMA1_Channel2;

  dmaNodeConfig.NodeType                        = DMA_GPDMA_LINEAR_NODE;
  dmaNodeConfig.Init.Request                    = GPDMA1_REQUEST_SAI1_A;
  dmaNodeConfig.Init.BlkHWRequest               = DMA_BREQ_SINGLE_BURST;
  dmaNodeConfig.Init.Direction                  = DMA_MEMORY_TO_PERIPH;
  dmaNodeConfig.Init.SrcInc                     = DMA_SINC_INCREMENTED;
  dmaNodeConfig.Init.DestInc                    = DMA_DINC_FIXED;
  dmaNodeConfig.Init.SrcDataWidth               = DMA_SRC_DATAWIDTH_HALFWORD;
  dmaNodeConfig.Init.DestDataWidth              = DMA_DEST_DATAWIDTH_HALFWORD;
  dmaNodeConfig.Init.SrcBurstLength             = 1;
  dmaNodeConfig.Init.DestBurstLength            = 1;
  dmaNodeConfig.Init.Priority                   = DMA_HIGH_PRIORITY;
  dmaNodeConfig.Init.TransferEventMode          = DMA_TCEM_BLOCK_TRANSFER;
  dmaNodeConfig.Init.TransferAllocatedPort      = DMA_SRC_ALLOCATED_PORT1 | DMA_DEST_ALLOCATED_PORT0;
  dmaNodeConfig.DataHandlingConfig.DataExchange  = DMA_EXCHANGE_NONE;
  dmaNodeConfig.DataHandlingConfig.DataAlignment = DMA_DATA_RIGHTALIGN_ZEROPADDED;
  dmaNodeConfig.TriggerConfig.TriggerPolarity    = DMA_TRIG_POLARITY_MASKED;
  dmaNodeConfig.SrcSecure                        = DMA_CHANNEL_SRC_SEC;
  dmaNodeConfig.DestSecure                       = DMA_CHANNEL_DEST_SEC;

  if (HAL_DMA_ConfigChannelAttributes(&hDmaSaiTx, (DMA_CHANNEL_PRIV | DMA_CHANNEL_SEC
                                                    | DMA_CHANNEL_SRC_SEC | DMA_CHANNEL_DEST_SEC)) != HAL_OK)
  {
    g_sai1_status = HAL_ERROR;
    return;
  }

  if (HAL_DMAEx_List_BuildNode(&dmaNodeConfig, &SaiTxNode) != HAL_OK)
  {
    g_sai1_status = HAL_ERROR;
    return;
  }

  if (HAL_DMAEx_List_InsertNode_Tail(&SaiTxQueue, &SaiTxNode) != HAL_OK)
  {
    g_sai1_status = HAL_ERROR;
    return;
  }

  if (HAL_DMAEx_List_SetCircularMode(&SaiTxQueue) != HAL_OK)
  {
    g_sai1_status = HAL_ERROR;
    return;
  }

  hDmaSaiTx.InitLinkedList.Priority          = DMA_HIGH_PRIORITY;
  hDmaSaiTx.InitLinkedList.LinkStepMode      = DMA_LSM_FULL_EXECUTION;
  hDmaSaiTx.InitLinkedList.LinkAllocatedPort = DMA_LINK_ALLOCATED_PORT1;
  hDmaSaiTx.InitLinkedList.TransferEventMode = DMA_TCEM_LAST_LL_ITEM_TRANSFER;
  hDmaSaiTx.InitLinkedList.LinkedListMode    = DMA_LINKEDLIST_CIRCULAR;

  if (HAL_DMAEx_List_Init(&hDmaSaiTx) != HAL_OK)
  {
    g_sai1_status = HAL_ERROR;
    return;
  }

  if (HAL_DMAEx_List_LinkQ(&hDmaSaiTx, &SaiTxQueue) != HAL_OK)
  {
    g_sai1_status = HAL_ERROR;
    return;
  }

  /* SaiTxNode/SaiTxQueue live in the .noncacheable section (MPU_Config(),
   * called from main() before the caches are enabled, marks that section
   * Normal/Non-cacheable) precisely because a manual clean here is not
   * enough: HAL_SAI_Transmit_DMA() itself later writes this node's
   * LinkRegisters (source address/size) right before enabling the GPDMA
   * channel, with no hook for us to clean afterwards, and GPDMA re-reads
   * this same node from RAM on every loop of the circular transfer (not
   * just the first one). See MPU_Config()'s comment in this file. */
  __HAL_LINKDMA(&hsai1, hdmatx, hDmaSaiTx);

  HAL_NVIC_SetPriority(GPDMA1_Channel2_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(GPDMA1_Channel2_IRQn);

  g_sai1_status = HAL_OK;
}

/*
 * MDF1 Initialization Function (onboard PDM MEMS mic, U13/U14)
 *
 * MDF1 is unchecked in the .ioc for the Appli target (it exists only for
 * FSBL, for a different purpose), so there is no generated MX_MDF1_Init().
 * Added by hand here, same pattern as MX_I2C2_Init()/MX_SAI1_Init() above.
 *
 * Scope for this pass: clock + GPIO (CCK0/DATIN0) + HAL_MDF_Init() only
 * (serial interface + common clock parameters). No DMA, no filter/channel
 * config, no acquisition start yet -- that is a later step. The values
 * below (PLL3/IC8 clock tree for the 16kHz group, MDF_SITF_NORMAL_SPI_MODE,
 * MDF_SITF_CCK0_SOURCE, Threshold=31, MDF_BITSTREAM0_FALLING,
 * ProcClockDivider=2, OutputClockDivider=12) are copied from ST's official
 * STM32N6570-DK BSP (stm32n6570-dk-bsp repo, stm32n6570_discovery_audio.c:
 * MX_MDF1_ClockConfig() / MX_MDF1_Init() / MDF_MspInit(), and the
 * MDF_PROC_CLOCK_DIVIDER(16K)/MDF_OUTPUT_CLOCK_DIVIDER(16K) macros there),
 * not derived from the reference manual. That same reference does not
 * configure a CKI pin either (see main.h's MDF1_CCK0_Pin comment) since
 * MDF_SITF_CCK0_SOURCE uses MDF1's own generated clock, not an external one.
 *
 * Same "no Error_Handler(), no tm_printf()" reasoning as MX_SAI1_Init()
 * applies (see that function's comment): every failure just returns early,
 * leaving g_mdf1_status at HAL_ERROR (or whatever HAL_RCC_OscConfig/
 * HAL_RCCEx_PeriphCLKConfig/HAL_MDF_Init returned) for Application/ code to
 * check and report over UART once the kernel is up.
 */
static void MX_MDF1_Init(void)
{
  RCC_OscInitTypeDef       RCC_OscInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
  GPIO_InitTypeDef         GPIO_InitStruct = {0};
  static DMA_NodeTypeDef   MdfRxNode  __NON_CACHEABLE;
  static DMA_QListTypeDef  MdfRxQueue __NON_CACHEABLE;
  DMA_NodeConfTypeDef      dmaNodeConfig = {0};

  g_mdf1_status = HAL_ERROR;
  g_mdf1_step = 1;	// RCC_OscConfig(PLL3)

  /* MDF1 kernel clock: PLL3 -> IC8, 16kHz group (ClockDivider = 1).
   * Separate PLL from SAI1's PLL2, so this does not disturb the audio-out
   * clock already configured by MX_SAI1_Init(). */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_NONE;
  RCC_OscInitStruct.PLL1.PLLState = RCC_PLL_NONE;
  RCC_OscInitStruct.PLL2.PLLState = RCC_PLL_NONE;
  RCC_OscInitStruct.PLL3.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL3.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL3.PLLFractional = 0;
  RCC_OscInitStruct.PLL3.PLLM = 6;
  RCC_OscInitStruct.PLL3.PLLN = 172;
  RCC_OscInitStruct.PLL3.PLLP1 = 7;
  RCC_OscInitStruct.PLL3.PLLP2 = 4;
  RCC_OscInitStruct.PLL4.PLLState = RCC_PLL_NONE;
  g_mdf1_status = HAL_RCC_OscConfig(&RCC_OscInitStruct);
  if (g_mdf1_status != HAL_OK)
  {
    return;
  }

  g_mdf1_step = 2;	// RCCEx_PeriphCLKConfig(IC8)
  PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_MDF1;
  PeriphClkInitStruct.Mdf1ClockSelection = RCC_MDF1CLKSOURCE_IC8;
  PeriphClkInitStruct.ICSelection[RCC_IC8].ClockSelection = RCC_ICCLKSOURCE_PLL3;
  PeriphClkInitStruct.ICSelection[RCC_IC8].ClockDivider = 1;
  g_mdf1_status = HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct);
  if (g_mdf1_status != HAL_OK)
  {
    return;
  }

  g_mdf1_step = 3;	// GPIO config (CCK0/DATIN0)

  /* MDF1 pins: CCK0=PE2 (output, drives the mics' clock), DATIN0=PE8
   * (input, PDM bitstream from the mics) */
  __HAL_RCC_GPIOE_CLK_ENABLE();

  GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull      = GPIO_NOPULL;
  GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF4_MDF1;

  GPIO_InitStruct.Pin = MDF1_CCK0_Pin;
  HAL_GPIO_Init(MDF1_CCK0_GPIO_Port, &GPIO_InitStruct);
  GPIO_InitStruct.Pin = MDF1_DATIN0_Pin;
  HAL_GPIO_Init(MDF1_DATIN0_GPIO_Port, &GPIO_InitStruct);

  __HAL_RCC_MDF1_CLK_ENABLE();

  /* FSBL.IPs includes MDF1 too (see the .ioc), so FSBL likely already
   * activated MDF1's clock generator/serial interface for its own purpose
   * before jumping to Appli, and nothing resets the peripheral in between.
   * HAL_MDF_Init() explicitly checks CKGCR.CCKACTIVE and SITFCR.SITFACTIVE
   * and returns HAL_ERROR if either is already set (stm32n6xx_hal_mdf.c) --
   * force-reset the peripheral via RCC first so it starts from the
   * power-on-default register state regardless of what FSBL left behind. */
  __HAL_RCC_MDF1_FORCE_RESET();
  __HAL_RCC_MDF1_RELEASE_RESET();

  /* MDF1_Filter0: common clock params + serial interface only (no filter/
   * channel/acquisition config yet) */
  hmdf1.Instance = MDF1_Filter0;
  hmdf1.Init.CommonParam.ProcClockDivider          = 2U;
  hmdf1.Init.CommonParam.OutputClock.Activation    = ENABLE;
  hmdf1.Init.CommonParam.OutputClock.Pins          = MDF_OUTPUT_CLOCK_0;
  hmdf1.Init.CommonParam.OutputClock.Divider       = 12U;
  hmdf1.Init.CommonParam.OutputClock.Trigger.Activation = DISABLE;
  hmdf1.Init.SerialInterface.Activation = ENABLE;
  hmdf1.Init.SerialInterface.Mode        = MDF_SITF_NORMAL_SPI_MODE;
  hmdf1.Init.SerialInterface.ClockSource = MDF_SITF_CCK0_SOURCE;
  hmdf1.Init.SerialInterface.Threshold   = 31U;
  hmdf1.Init.FilterBistream = MDF_BITSTREAM0_FALLING;

  g_mdf1_step = 4;	// HAL_MDF_Init
  g_mdf1_status = HAL_MDF_Init(&hmdf1);
  if (g_mdf1_status != HAL_OK)
  {
    return;
  }

  g_mdf1_step = 5;	// MDF1 filter0 Rx DMA (GPDMA1 Channel 0)

  /* MDF1_Filter0 Rx DMA: GPDMA1 Channel 0, one linear node, queue circular
   * so the double buffer is filled on repeat until HAL_MDF_AcqStop_DMA().
   * Node config copied from the same STM32N6570-DK BSP's MDF_MspInit() DMA
   * block (peripheral-to-memory, fixed source = DFLTDR, incrementing
   * 32-bit destination).
   *
   * MdfRxNode/MdfRxQueue are __NON_CACHEABLE for exactly the same reason as
   * SAI1's SaiTxNode/SaiTxQueue (see MX_SAI1_Init()'s cache note):
   * HAL_MDF_AcqStart_DMA() itself writes this node's LinkRegisters (source/
   * destination address, block size) right before starting the channel, and
   * GPDMA re-reads the node from RAM on every loop of the circular transfer,
   * so a one-shot manual cache clean here would not cover it. */
  __HAL_RCC_GPDMA1_CLK_ENABLE();

  hDmaMdf.Instance = GPDMA1_Channel0;

  dmaNodeConfig.NodeType                         = DMA_GPDMA_LINEAR_NODE;
  dmaNodeConfig.Init.Request                     = GPDMA1_REQUEST_MDF1_FLT0;
  dmaNodeConfig.Init.BlkHWRequest                = DMA_BREQ_SINGLE_BURST;
  dmaNodeConfig.Init.Direction                   = DMA_PERIPH_TO_MEMORY;
  dmaNodeConfig.Init.SrcInc                      = DMA_SINC_FIXED;
  dmaNodeConfig.Init.DestInc                     = DMA_DINC_INCREMENTED;
  dmaNodeConfig.Init.SrcDataWidth                = DMA_SRC_DATAWIDTH_WORD;
  dmaNodeConfig.Init.DestDataWidth               = DMA_DEST_DATAWIDTH_WORD;
  dmaNodeConfig.Init.SrcBurstLength              = 1;
  dmaNodeConfig.Init.DestBurstLength             = 1;
  dmaNodeConfig.Init.Priority                    = DMA_HIGH_PRIORITY;
  dmaNodeConfig.Init.TransferEventMode           = DMA_TCEM_BLOCK_TRANSFER;
  dmaNodeConfig.Init.TransferAllocatedPort       = DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT1;
  dmaNodeConfig.DataHandlingConfig.DataExchange  = DMA_EXCHANGE_NONE;
  dmaNodeConfig.DataHandlingConfig.DataAlignment = DMA_DATA_RIGHTALIGN_ZEROPADDED;
  dmaNodeConfig.TriggerConfig.TriggerPolarity    = DMA_TRIG_POLARITY_MASKED;
  dmaNodeConfig.SrcSecure                        = DMA_CHANNEL_SRC_SEC;
  dmaNodeConfig.DestSecure                       = DMA_CHANNEL_DEST_SEC;

  if (HAL_DMA_ConfigChannelAttributes(&hDmaMdf, (DMA_CHANNEL_PRIV | DMA_CHANNEL_SEC
                                                  | DMA_CHANNEL_SRC_SEC | DMA_CHANNEL_DEST_SEC)) != HAL_OK)
  {
    g_mdf1_status = HAL_ERROR;
    return;
  }

  if (HAL_DMAEx_List_BuildNode(&dmaNodeConfig, &MdfRxNode) != HAL_OK)
  {
    g_mdf1_status = HAL_ERROR;
    return;
  }

  if (HAL_DMAEx_List_InsertNode_Tail(&MdfRxQueue, &MdfRxNode) != HAL_OK)
  {
    g_mdf1_status = HAL_ERROR;
    return;
  }

  if (HAL_DMAEx_List_SetCircularMode(&MdfRxQueue) != HAL_OK)
  {
    g_mdf1_status = HAL_ERROR;
    return;
  }

  hDmaMdf.InitLinkedList.Priority          = DMA_HIGH_PRIORITY;
  hDmaMdf.InitLinkedList.LinkStepMode      = DMA_LSM_FULL_EXECUTION;
  hDmaMdf.InitLinkedList.LinkAllocatedPort = DMA_LINK_ALLOCATED_PORT1;
  hDmaMdf.InitLinkedList.TransferEventMode = DMA_TCEM_LAST_LL_ITEM_TRANSFER;
  hDmaMdf.InitLinkedList.LinkedListMode    = DMA_LINKEDLIST_CIRCULAR;

  if (HAL_DMAEx_List_Init(&hDmaMdf) != HAL_OK)
  {
    g_mdf1_status = HAL_ERROR;
    return;
  }

  if (HAL_DMAEx_List_LinkQ(&hDmaMdf, &MdfRxQueue) != HAL_OK)
  {
    g_mdf1_status = HAL_ERROR;
    return;
  }

  __HAL_LINKDMA(&hmdf1, hdma, hDmaMdf);

  HAL_NVIC_SetPriority(GPDMA1_Channel0_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(GPDMA1_Channel0_IRQn);

  g_mdf1_step = 6;	// all steps completed
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  MPU_Config();	// .noncacheable region for SAI1 Tx DMA node/queue; must run before the caches are enabled below

  /* USER CODE END 1 */

  /* Enable the CPU Cache */

  /* Enable I-Cache---------------------------------------------------------*/
  SCB_EnableICache();

  /* Enable D-Cache---------------------------------------------------------*/
  SCB_EnableDCache();

  /* MCU Configuration--------------------------------------------------------*/
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_ADC1_Init();
  MX_ADC2_Init();
  MX_I2C1_Init();
  /* USER CODE BEGIN 2 */
  MX_I2C2_Init();	// WM8904 (I2C2) bring-up; see MX_I2C2_Init() comment above
  MX_SAI1_Init();	// WM8904 audio data path (SAI1); see MX_SAI1_Init() comment above
  MX_MDF1_Init();	// onboard PDM mic (MDF1); see MX_MDF1_Init() comment above

  void knl_start_mtkernel(void);
  knl_start_mtkernel();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief Peripherals Common Clock Configuration
  * @retval None
  */
void PeriphCommonClock_Config(void)
{
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

  /** Initializes the peripherals clock
  */
  PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_CKPER;
  PeriphClkInitStruct.CkperClockSelection = RCC_CLKPCLKSOURCE_HSI;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_MultiModeTypeDef multimode = {0};
  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */
  __HAL_RCC_RIFSC_CLK_ENABLE();
    RIFSC->RISC_SECCFGRx[2] |= 0x1;
  /* USER CODE END ADC1_Init 1 */

  /** Common config
  */
  hadc1.Instance = ADC1;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.GainCompensation = 0;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
  hadc1.Init.Overrun = ADC_OVR_DATA_PRESERVED;
  hadc1.Init.LeftBitShift = ADC_LEFTBITSHIFT_NONE;
  hadc1.Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure the ADC multi-mode
  */
  multimode.Mode = ADC_MODE_INDEPENDENT;
  if (HAL_ADCEx_MultiModeConfigChannel(&hadc1, &multimode) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_10;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_1CYCLE_5;
  sConfig.SingleDiff = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief ADC2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC2_Init(void)
{

  /* USER CODE BEGIN ADC2_Init 0 */

  /* USER CODE END ADC2_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC2_Init 1 */

  /* USER CODE END ADC2_Init 1 */

  /** Common config
  */
  hadc2.Instance = ADC2;
  hadc2.Init.Resolution = ADC_RESOLUTION_12B;
  hadc2.Init.GainCompensation = 0;
  hadc2.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc2.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc2.Init.LowPowerAutoWait = DISABLE;
  hadc2.Init.ContinuousConvMode = DISABLE;
  hadc2.Init.NbrOfConversion = 1;
  hadc2.Init.DiscontinuousConvMode = DISABLE;
  hadc2.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc2.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc2.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
  hadc2.Init.Overrun = ADC_OVR_DATA_PRESERVED;
  hadc2.Init.LeftBitShift = ADC_LEFTBITSHIFT_NONE;
  hadc2.Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(&hadc2) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_18;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_1CYCLE_5;
  sConfig.SingleDiff = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  if (HAL_ADC_ConfigChannel(&hadc2, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC2_Init 2 */

  /* USER CODE END ADC2_Init 2 */

}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.Timing = 0x30C0EDFF;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Analogue filter
  */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Digital filter
  */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOO_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LED_GREEN_GPIO_Port, LED_GREEN_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LED_RED_GPIO_Port, LED_RED_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : LED_GREEN_Pin */
  GPIO_InitStruct.Pin = LED_GREEN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_GREEN_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : LED_RED_Pin */
  GPIO_InitStruct.Pin = LED_RED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_RED_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
