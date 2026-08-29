/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined ( __ICCARM__ )
#  define CMSE_NS_CALL  __cmse_nonsecure_call
#  define CMSE_NS_ENTRY __cmse_nonsecure_entry
#else
#  define CMSE_NS_CALL  __attribute((cmse_nonsecure_call))
#  define CMSE_NS_ENTRY __attribute((cmse_nonsecure_entry))
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32n6xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* Function pointer declaration in non-secure*/
#if defined ( __ICCARM__ )
typedef void (CMSE_NS_CALL *funcptr)(void);
#else
typedef void CMSE_NS_CALL (*funcptr)(void);
#endif

/* typedef for non-secure callback functions */
typedef funcptr funcptr_NS;

/* USER CODE BEGIN ET */

/* I2C2 is not enabled in the CubeMX (.ioc) configuration, so it has no
 * generated init code. WM8904 access code lives in Appli/Application/
 * (per project convention only that folder should be touched), but the
 * peripheral clock/GPIO/HAL_I2C_Init bring-up must happen here in
 * Core/main.c since Application/ has no access to RCC/GPIO. This handle
 * is exposed so Application/ code can call HAL_I2C_Mem_Read() on it. */
extern I2C_HandleTypeDef hi2c2;

/* SAI1 is likewise not enabled in the .ioc (no MX_SAI1_Init generated for
 * the Appli target). hsai1 drives SAI1_Block_A (master TX, 16kHz/16bit,
 * MCLK out) for the WM8904. g_sai1_status records whether MX_SAI1_Init()
 * (clock/GPIO/HAL_SAI_Init, see main.c) succeeded.
 *
 * IMPORTANT: tm_printf()/tm_putstring() are NOT safe to call before
 * knl_start_mtkernel() runs, because libtm_init() (which the UART TX/RX
 * routines used by tm_printf depend on) is only called from kernel sysinit
 * (mtkernel/kernel/sysinit/sysinit.c), i.e. *after* knl_start_mtkernel().
 * So MX_SAI1_Init() must not print and must not block main() on failure
 * (no Error_Handler()); it only records the result here. Application/
 * code (running inside a task, after libtm_init() has run) checks
 * g_sai1_status and reports it over UART. */
extern SAI_HandleTypeDef hsai1;
extern HAL_StatusTypeDef g_sai1_status;

/* SAI1 Tx DMA channel (GPDMA1 Channel 2), set up in MX_SAI1_Init() for
 * circular double-buffered playback (see that function's comment in
 * main.c). Exposed so stm32n6xx_it.c's GPDMA1_Channel2_IRQHandler() can
 * call HAL_DMA_IRQHandler() on it; Application/ code does not need to
 * touch it directly (it goes through HAL_SAI_Transmit_DMA(&hsai1, ...)
 * via audio/sai_io.c instead). */
extern DMA_HandleTypeDef hDmaSaiTx;

/* MDF1 (onboard PDM MEMS mic, U13/U14) is likewise not enabled in the
 * .ioc (no MX_MDF1_Init generated for the Appli target; MDF1 is FSBL-only,
 * same as SAI1 was). hmdf1 drives MDF1_Filter0 (serial interface + common
 * clock config only, at this stage -- no DMA/filter/acquisition setup
 * yet). g_mdf1_status records whether MX_MDF1_Init() (clock/GPIO/
 * HAL_MDF_Init, see main.c) succeeded; same tm_printf-unsafe-before-
 * kernel-start reasoning as g_sai1_status above applies here too. */
extern MDF_HandleTypeDef hmdf1;
extern HAL_StatusTypeDef g_mdf1_status;

/* g_mdf1_step: which step MX_MDF1_Init() was on when it stopped (0=not
 * started yet, 1=attempting RCC_OscConfig(PLL3), 2=attempting
 * RCCEx_PeriphCLKConfig(IC8), 3=attempting GPIO config, 4=attempting
 * HAL_MDF_Init, 5=all steps completed). g_mdf1_status holds the
 * HAL_StatusTypeDef of whichever call failed at that step (HAL_OK if it
 * got all the way to step 5). Recorded here for the same reason
 * g_mdf1_status itself is -- tm_printf() is unsafe before
 * knl_start_mtkernel() runs, so Application/ code reports these two
 * values over UART once the kernel is up. */
extern uint32_t g_mdf1_step;

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define OSC_IN_Pin GPIO_PIN_0
#define OSC_IN_GPIO_Port GPIOH
#define I2C1_SDA_Pin GPIO_PIN_1
#define I2C1_SDA_GPIO_Port GPIOC
#define OSC_OUT_Pin GPIO_PIN_1
#define OSC_OUT_GPIO_Port GPIOH
#define I2C1_SCL_Pin GPIO_PIN_9
#define I2C1_SCL_GPIO_Port GPIOH
#define LED_GREEN_Pin GPIO_PIN_1
#define LED_GREEN_GPIO_Port GPIOO
#define UCPD1_VSENSE_Pin GPIO_PIN_11
#define UCPD1_VSENSE_GPIO_Port GPIOA
#define LED_RED_Pin GPIO_PIN_10
#define LED_RED_GPIO_Port GPIOG

/* USER CODE BEGIN Private defines */

/* I2C2 (WM8904 control interface, see MX_I2C2_Init() in main.c) */
#define I2C2_SCL_Pin GPIO_PIN_14
#define I2C2_SCL_GPIO_Port GPIOD
#define I2C2_SDA_Pin GPIO_PIN_4
#define I2C2_SDA_GPIO_Port GPIOD

/* SAI1_Block_A (WM8904 audio data path, see MX_SAI1_Init() in main.c) */
#define SAI1_FS_A_Pin GPIO_PIN_0
#define SAI1_FS_A_GPIO_Port GPIOB
#define SAI1_SCK_A_Pin GPIO_PIN_6
#define SAI1_SCK_A_GPIO_Port GPIOB
#define SAI1_SD_A_Pin GPIO_PIN_7
#define SAI1_SD_A_GPIO_Port GPIOB
#define SAI1_MCLK_A_Pin GPIO_PIN_7
#define SAI1_MCLK_A_GPIO_Port GPIOG

/* MDF1 (PDM mic CCK0/DATIN0, see MX_MDF1_Init() in main.c). CKI0 (PE7,
 * MIC_CK echo-back per the schematic) is not configured: MX_MDF1_Init()
 * uses MDF_SITF_CCK0_SOURCE (MDF's own generated clock), for which the
 * ST reference driver (stm32n6570-dk-bsp) does not configure a CKI pin
 * either -- it is only needed for the external-clock-source mode. */
#define MDF1_CCK0_Pin GPIO_PIN_2
#define MDF1_CCK0_GPIO_Port GPIOE
#define MDF1_DATIN0_Pin GPIO_PIN_8
#define MDF1_DATIN0_GPIO_Port GPIOE

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
