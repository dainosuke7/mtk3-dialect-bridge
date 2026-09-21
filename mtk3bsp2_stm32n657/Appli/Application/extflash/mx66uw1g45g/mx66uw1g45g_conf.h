/**
  ******************************************************************************
  * @file    mx66uw1g45g_conf.h
  * @author  MCD Application Team
  * @brief   MX66UW1G45G OctoSPI memory configuration template file.
  *          This file should be copied to the application folder and renamed
  *          to mx66uw1g45g_conf.h
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2023 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef MX66UW1G45G_CONF_H
#define MX66UW1G45G_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 本プロジェクトでの変更点 (テンプレートからの差分はこの2点のみ):
 *   - インクルードを stm32xxxx_hal.h から stm32n6xx_hal.h に
 *   - DUMMY_CYCLES_READ_OCTAL / _OCTAL_DTR を 6 から 10 に。
 *     STM32N6-GettingStarted-Audio v2.3.0 (Projects/GS/Inc/mx66uw1g45g_conf.h)
 *     が STM32N6570-DK で使っている値に合わせた。フラッシュ側 CR2 の
 *     ダミーサイクルは extflash.c で 20 サイクルに設定する (ST の BSP と同じ)。
 */

/* Includes ------------------------------------------------------------------*/
#include "stm32n6xx_hal.h"

/** @addtogroup BSP
  * @{
  */
#define CONF_OSPI_ODS                MX66UW1G45G_CR_ODS_24   /* MX66UW1G45G Output Driver Strength */

#define DUMMY_CYCLES_READ            8U
#define DUMMY_CYCLES_READ_OCTAL      10U
#define DUMMY_CYCLES_READ_OCTAL_DTR  10U
#define DUMMY_CYCLES_REG_OCTAL       4U
#define DUMMY_CYCLES_REG_OCTAL_DTR   5U

/**
  * @}
  */

#ifdef __cplusplus
}
#endif

#endif /* MX66UW1G45G_CONF_H */
