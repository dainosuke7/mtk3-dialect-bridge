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

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_ADC2_Init(void);
static void MX_I2C1_Init(void);
/* USER CODE BEGIN PFP */

static void MX_I2C2_Init(void);
static void MX_SAI1_Init(void);

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
 */
static void MX_SAI1_Init(void)
{
  RCC_OscInitTypeDef       RCC_OscInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
  GPIO_InitTypeDef         GPIO_InitStruct = {0};

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
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

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
