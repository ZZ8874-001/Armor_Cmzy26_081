/**
 ******************************************************************************
 * @file    app_log.c
 * @brief   日志适配层（USART1 115200）——非阻塞环形缓冲实现（Step 4 修订，2026-08-21）。
 *
 * 背景：原阻塞式 printf 在流采集开启后每行阻塞 3~8ms，导致 ADS 环形缓冲溢出
 * （实测 drop≈drdy/2 与击打时丢帧）。现改为：
 *   - App_Log_Printf 只格式化进 1KB 环形缓冲（溢出丢最旧，计数 s_log_drop）；
 *   - 主循环每圈调用 App_Log_FlushSmall(n) 向 USART1 刷 1~2 字符（<200µs，
 *     小于帧周期 256µs），帧消费优先、日志其次。
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

void App_Log_Init(void)
{
    s_log_wr = 0u;
    s_log_rd = 0u;
    s_log_drop = 0u;
    /* USART1（115200，阻塞发送由 FlushSmall 使用）由 CubeMX 配置 */
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

/* 主循环调用：从环形缓冲向 USART1 刷 n 个字符（每字符 87µs@115200） */
void App_Log_FlushSmall(uint8_t n)
{
    while (n-- > 0u && s_log_rd != s_log_wr)
    {
        (void)HAL_UART_Transmit(&huart1, (uint8_t *)&s_log_buf[s_log_rd], 1u, 1u);
        s_log_rd = (uint16_t)((s_log_rd + 1u) % LOG_BUF_SIZE);
    }
}
