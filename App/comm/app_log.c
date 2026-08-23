/**
 ******************************************************************************
 * @file    app_log.c
 * @brief   日志适配层（USART1 115200 阻塞输出，骨架/调试用）。
 *
 * 设计文档 v1.5：第 9.2 节（app_log 可选）。骨架阶段为阻塞实现；
 * TODO(Step 4)：如联调期影响实时性，改为环形缓冲 + 后台发送。
 ******************************************************************************
 */
#include "app_log.h"
#include "board.h"

#include <stdio.h>
#include <string.h>

#define APP_LOG_BUF_SIZE  128u

static char s_buf[APP_LOG_BUF_SIZE];

void App_Log_Init(void)
{
    /* huart1（115200 8N1）已由 CubeMX 初始化，无需额外动作。 */
}

void App_Log_VPrintf(const char *fmt, va_list args)
{
    int n = vsnprintf(s_buf, sizeof(s_buf), fmt, args);
    if (n <= 0)
    {
        return;
    }
    if ((size_t)n >= sizeof(s_buf))
    {
        n = (int)sizeof(s_buf) - 1;
    }
    (void)HAL_UART_Transmit(&huart1, (uint8_t *)s_buf, (uint16_t)n, 100u);
}

void App_Log_Printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    App_Log_VPrintf(fmt, args);
    va_end(args);
}
