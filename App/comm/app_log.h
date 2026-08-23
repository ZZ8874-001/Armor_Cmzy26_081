#ifndef APP_LOG_H
#define APP_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>

/* 日志适配层。
 * 说明：
 * 1. 用于隔离 printf、HAL UART、RTT、DMA 日志等平台差异。
 * 2. 若后续移植到其他板子，原则上允许只改 app_log.c。
 * 3. 当前 demo 为阻塞式串口输出；实时性更严格时建议改成 DMA 或后台任务。
 */

/* 初始化日志输出模块。 */
void App_Log_Init(void);

/* printf 风格日志输出。 */
void App_Log_Printf(const char *fmt, ...);

/* va_list 版本日志输出。 */
void App_Log_VPrintf(const char *fmt, va_list args);

#ifdef __cplusplus
}
#endif

#endif
