/**
 ******************************************************************************
 * @file    app_log.c
 * @brief   日志适配层（USART1 921600）——非阻塞环形缓冲 + DMA 块发送（2026-08-21/10-28）。
 *
 * 文本通路（原阻塞式 printf 在流采集开启后每行阻塞 3~8ms，导致 ADS 环形缓冲
 * 溢出，故改为）：
 *   - App_Log_Printf 只格式化进 1KB 环形缓冲（溢出丢最旧，计数 s_log_drop）；
 *   - 主循环每圈调用 App_Log_FlushSmall(n) 向 USART1 刷 1~2 字符（<200µs，
 *     小于帧周期 256µs），帧消费优先、日志其次。
 * 二进制通路（2026-10-28）：
 *   - App_Log_SendDma 整帧交给 USART1 TX DMA（DMA2_Channel6，硬件搬运）；
 *   - 与文本通路互斥：DMA 在飞期间 FlushSmall 暂停（见其入口门控）；
 *   - 完成回调经 bsp/ws2812_uart.c 的 HAL_UART_TxCpltCallback 分发到
 *     App_Log_IrqTxDone（USART1 IRQ 的 TC 中断 → HAL 内部结束发送）。
 ******************************************************************************
 */
#include "comm/app_log.h"
#include "board.h"

#include <stdio.h>
#include <string.h>

#define LOG_BUF_SIZE   1024u
#define LOG_TMP_SIZE   128u

static char s_log_buf[LOG_BUF_SIZE];
static volatile uint16_t s_log_wr;      /* 写指针（任意上下文调用方保证） */
static uint16_t s_log_rd;               /* 读指针（仅主循环 FlushSmall 访问） */
static volatile uint32_t s_log_drop;    /* 缓冲溢出丢弃字节数（调试变量） */

/* DMA 块发送状态（深度 1；Ozone 标量调试变量） */
static volatile uint8_t  s_wave_tx_busy;  /* 1=DMA 在飞（FlushSmall 门控 + TxBusy 判忙） */
static volatile uint32_t s_wave_tx_cnt;   /* 累计成功启动的块发送数 */
static volatile uint32_t s_wave_drop;     /* 防御性丢弃计数（忙/启动失败；正常路径应恒 0） */
static volatile uint32_t s_wave_tx_start_ms;  /* 本次 DMA 启动时刻（看门狗用） */
static volatile uint32_t s_wave_tx_stuck;     /* 看门狗强制中止次数（正常应恒 0） */

/* DMA 块发送看门狗上限：2100B ≈22.8ms@921600，4 倍余量。
 * 一旦 HAL 完成回调不来（典型原因：CubeMX 重生成掉了 NVIC.USART1_IRQn /
 * USART1_IRQHandler —— NORMAL 模式 DMA 搬完只置 TCIE，完成回调在 USART1
 * 中断里才执行），busy 会永久为 1：波形帧不再发、FlushSmall 被门控、整个
 * 串口静默。看门狗超时强制中止，至少让文本日志活下来并留下计数。 */
#define WAVE_TX_TIMEOUT_MS   100u

void App_Log_Init(void)
{
    s_log_wr = 0u;
    s_log_rd = 0u;
    s_log_drop = 0u;
    s_wave_tx_busy = 0u;
    s_wave_tx_cnt = 0u;
    s_wave_drop = 0u;
    s_wave_tx_start_ms = 0u;
    s_wave_tx_stuck = 0u;
    /* USART1（921600 8N1 + TX DMA2_Channel6 + 全局中断）由 CubeMX 配置 */
}

void App_Log_VPrintf(const char *fmt, va_list args)
{
    char tmp[LOG_TMP_SIZE];
    int  n;
    int  i;

    n = vsnprintf(tmp, sizeof(tmp), fmt, args);
    if (n <= 0)
    {
        return;
    }
    if (n > (int)sizeof(tmp))
    {
        n = (int)sizeof(tmp);
    }

    for (i = 0; i < n; i++)
    {
        s_log_buf[s_log_wr] = tmp[i];
        s_log_wr = (uint16_t)((s_log_wr + 1u) % LOG_BUF_SIZE);
        if (s_log_wr == s_log_rd)
        {
            s_log_drop++;
            s_log_rd = (uint16_t)((s_log_rd + 1u) % LOG_BUF_SIZE);   /* 丢最旧 */
        }
    }
}

void App_Log_Printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    App_Log_VPrintf(fmt, args);
    va_end(args);
}

/* 主循环调用：从环形缓冲向 USART1 刷 n 个字符（每字符 ≈10.9µs@921600）。
 * DMA 在飞期间直接返回：轮询 HAL_UART_Transmit 会因 gState=BUSY_TX 返回
 * HAL_BUSY，而这里不检查返回值——不门控就会把未发出的字符静默越过（丢失）。 */
void App_Log_FlushSmall(uint8_t n)
{
    if (s_wave_tx_busy != 0u)
    {
        return;
    }
    while (n-- > 0u && s_log_rd != s_log_wr)
    {
        (void)HAL_UART_Transmit(&huart1, (uint8_t *)&s_log_buf[s_log_rd], 1u, 1u);
        s_log_rd = (uint16_t)((s_log_rd + 1u) % LOG_BUF_SIZE);
    }
}

bool App_Log_SendDma(const uint8_t *buf, uint16_t len)
{
    if (buf == NULL || len == 0u)
    {
        return false;
    }
    if (s_wave_tx_busy != 0u)
    {
        s_wave_drop++;
        return false;              /* 上一帧在飞：丢弃（调用方计 trig_id 跳号） */
    }

    s_wave_tx_busy = 1u;
    s_wave_tx_start_ms = HAL_GetTick();
    if (HAL_UART_Transmit_DMA(&huart1, (uint8_t *)buf, len) != HAL_OK)
    {
        s_wave_tx_busy = 0u;
        s_wave_drop++;
        return false;
    }
    s_wave_tx_cnt++;
    return true;
}

/* 主循环调用：DMA 块发送看门狗（见 WAVE_TX_TIMEOUT_MS 注释）。 */
void App_Log_Poll(void)
{
    if (s_wave_tx_busy != 0u &&
        (uint32_t)(HAL_GetTick() - s_wave_tx_start_ms) >= WAVE_TX_TIMEOUT_MS)
    {
        (void)HAL_UART_AbortTransmit(&huart1);   /* 阻塞式：停 DMA + 清 TCIE + gState=READY */
        s_wave_tx_busy = 0u;
        s_wave_tx_stuck++;
        App_Log_Printf("[WAVE] tx timeout, aborted\r\n");
    }
}

bool App_Log_TxBusy(void)
{
    return (s_wave_tx_busy != 0u) ? true : false;
}

/* USART1 TX 完成（DMA 搬完 → HAL 在 USART1 TC 中断里回调到这里）。ISR 上下文。 */
void App_Log_IrqTxDone(void)
{
    s_wave_tx_busy = 0u;
}
