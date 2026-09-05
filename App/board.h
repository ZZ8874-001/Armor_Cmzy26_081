/**
 ******************************************************************************
 * @file    board.h
 * @brief   板级公共定义：外设句柄 extern 的唯一入口、板级常量、通道极性宏。
 *
 * 设计文档：Docs/装甲模块固件设计方案.md v1.5（第 2 章引脚映射、6.7 极性、8.1 灯数）。
 * 约定：新增对生成代码中任何外设句柄的引用，一律在此声明 extern，不各自 extern。
 ******************************************************************************
 */
#ifndef APP_BOARD_H
#define APP_BOARD_H

#include "main.h"   /* 引脚宏（ADC_CS/IND_ROHT/…）与 HAL 类型 */

/* ==================== 外设句柄（定义于 Core/Src/main.c，CubeMX 生成） ==================== */
extern ADC_HandleTypeDef     hadc1;
extern CAN_HandleTypeDef     hcan1;
extern DAC_HandleTypeDef     hdac1;
extern SPI_HandleTypeDef     hspi1;
extern TIM_HandleTypeDef     htim1;   /* ADC1 触发源（TRGO=OC1，≈1.22kHz） */
extern TIM_HandleTypeDef     htim2;   /* 1kHz 状态/灯/健康时基 */
extern TIM_HandleTypeDef     htim16;  /* ADS131M04 CLKIN（8MHz PWM CH1N） */
extern UART_HandleTypeDef    huart1;  /* 调试日志 */
extern UART_HandleTypeDef    huart2;  /* WS2812 灯带 */
extern DMA_HandleTypeDef     hdma_spi1_tx;
extern DMA_HandleTypeDef     hdma_spi1_rx;
extern DMA_HandleTypeDef     hdma_usart2_tx;

/* ==================== 板级常量 ==================== */
#define LED_COUNT          13U     /* WS2812：板载 1 + 左 6 + 右 6（8.1） */
#define ADS_FRAME_BYTES    18U     /* 6 字 × 24bit（6.4） */
#define ADS_RING_DEPTH     32U     /* K 帧环形缓冲，预触发历史 8.2ms（6.4/7.4 P21） */
#define APP_TICK_HZ        1000U   /* TIM2 1kHz（第 10 章） */

/* ==================== 通道极性宏（6.7） ==================== */
/* bit0-3 = ch0-3，1 = 该通道取反。实际极性受传感器接线影响，
 * 标定后写入 Flash；开机击打校对接口 HitDetect_CalibratePolarity() 预留。 */
#define ADC_CH_SIGN_MASK   0x00u

#endif /* APP_BOARD_H */
