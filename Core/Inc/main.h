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

/* Includes ------------------------------------------------------------------*/
#include "stm32l4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

void HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim);

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define ADC_NDRDY_Pin GPIO_PIN_14
#define ADC_NDRDY_GPIO_Port GPIOC
#define ADC_NDRDY_EXTI_IRQn EXTI15_10_IRQn
#define ADC_SYNC_Pin GPIO_PIN_15
#define ADC_SYNC_GPIO_Port GPIOC
#define PA3_NC_Pin GPIO_PIN_3
#define PA3_NC_GPIO_Port GPIOA
#define ROHT_DAC_TEST_NC_Pin GPIO_PIN_4
#define ROHT_DAC_TEST_NC_GPIO_Port GPIOA
#define TMP_REF2_NC_Pin GPIO_PIN_5
#define TMP_REF2_NC_GPIO_Port GPIOA
#define TMP_REF3_NC_Pin GPIO_PIN_6
#define TMP_REF3_NC_GPIO_Port GPIOA
#define TMP_REF4_NC_Pin GPIO_PIN_7
#define TMP_REF4_NC_GPIO_Port GPIOA
#define IND_ROHT_Pin GPIO_PIN_0
#define IND_ROHT_GPIO_Port GPIOB
#define IND_NORM_Pin GPIO_PIN_1
#define IND_NORM_GPIO_Port GPIOB
#define PA8_NC_Pin GPIO_PIN_8
#define PA8_NC_GPIO_Port GPIOA
#define ADC_CS_Pin GPIO_PIN_15
#define ADC_CS_GPIO_Port GPIOA
#define ADC_CLK_Pin GPIO_PIN_6
#define ADC_CLK_GPIO_Port GPIOB
#define PB7_NC_Pin GPIO_PIN_7
#define PB7_NC_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
