/**
 ******************************************************************************
 * @file    ws2812_uart.h
 * @brief   WS2812 灯带驱动接口（USART2 2.6667MHz + TX DMA，设计文档 v1.5 第 8.1 节）。
 *
 * 编码方式：每 LED 24bit → 8 字节（3 个 UART 位编码 1 个 WS2812 位：110=1 码、100=0 码）。
 * 灯数上限 13（板载 1 → 左 6 → 右 6），帧缓冲 13×8+14=118B，帧刷新 20Hz。
 ******************************************************************************
 */
#ifndef APP_WS2812_UART_H
#define APP_WS2812_UART_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"

#define WS2812_FRAME_BYTES  ((LED_COUNT) * 12u)          /* 156B（12B/LED；复位由 TXINV 空闲低电平实现） */

/* 初始化（USART2 3.75MBd+TX 反相 与 TX DMA 由 CubeMX 配置；PA2 已为推挽 AF_PP）。 */
void Ws2812_Init(void);

/* 单灯 RGB → 12 字节编码（GRB 序，MSB 先行，每字节 2 个 WS 位）。 */
void Ws2812_EncodeLed(uint8_t dst[12], uint8_t r, uint8_t g, uint8_t b);

/* 发送一帧（DMA 后台传输，≈404µs）；返回 0=启动成功，-1=忙。 */
int  Ws2812_Send(const uint8_t *frame);

/* 当前是否正在发送。 */
bool Ws2812_IsBusy(void);

/* DMA 完成中断回调（HAL_UART_TxCpltCallback 桥接）。 */
void Ws2812_IrqTxDone(void);

#endif /* APP_WS2812_UART_H */
