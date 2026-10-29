#ifndef APP_LOG_H
#define APP_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

/* 日志适配层（USART1 @ 921600 8N1）。
 * 说明：
 * 1. 用于隔离 printf、HAL UART、RTT、DMA 日志等平台差异。
 * 2. 若后续移植到其他板子，原则上允许只改 app_log.c。
 * 3. 两条 TX 通路，互斥见 App_Log_SendDma 注释：
 *    - 文本：App_Log_Printf → 1KB 环形缓冲 → FlushSmall 轮询逐字符（低带宽，不丢主循环）；
 *    - 二进制块：App_Log_SendDma → TX DMA 一整帧（波形帧，硬件搬运、零字符开销）。
 */

/* 初始化日志输出模块。 */
void App_Log_Init(void);

/* printf 风格日志输出（非阻塞：写入环形缓冲，由 FlushSmall 刷出）。 */
void App_Log_Printf(const char *fmt, ...);

/* 主循环调用：向 USART1 刷 n 个字符（每字符 ≈10.9µs@921600，帧消费后调用）。 */
void App_Log_FlushSmall(uint8_t n);

/* 二进制块发送（USART1 TX DMA，非阻塞）：buf 在发送完成前不得覆写。
 * 忙（上一帧在飞）或启动失败返回 false——调用方负责丢弃计数；len ≤ 65535。
 * DMA 在飞期间 FlushSmall 自动暂停（轮询 Transmit 会撞 HAL_BUSY 丢字符）。
 * 完成由 HAL_UART_TxCpltCallback(USART1) → App_Log_IrqTxDone 释放。
 * 组帧前建议先查 App_Log_TxBusy()，忙则本趟不组帧（调用方下一趟重试），
 * 避免覆写在飞缓冲。 */
bool App_Log_SendDma(const uint8_t *buf, uint16_t len);

/* 是否有 DMA 块发送在飞（供调用方在写发送缓冲前判忙）。 */
bool App_Log_TxBusy(void);

/* 主循环调用：DMA 块发送看门狗。超时（100ms）强制中止并清忙，使文本通路
 * 恢复。正常路径永不触发；触发即说明 USART1 完成中断没工作（典型原因：
 * CubeMX 重生成把 NVIC.USART1_IRQn / USART1_IRQHandler 弄丢了）。 */
void App_Log_Poll(void);

/* USART1 TX 完成钩子（ISR 上下文，由 bsp/ws2812_uart.c 的 TxCpltCallback 分发）。 */
void App_Log_IrqTxDone(void);

/* va_list 版本日志输出。 */
void App_Log_VPrintf(const char *fmt, va_list args);

#ifdef __cplusplus
}
#endif

#endif
