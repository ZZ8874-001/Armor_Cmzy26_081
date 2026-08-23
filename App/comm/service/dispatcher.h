#ifndef SERVICE_DISPATCHER_H
#define SERVICE_DISPATCHER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "app_can.h"
#include "protocol/app_frame.h"

/* 应用层分发处理函数。 */
typedef bool (*ServiceDispatcherHandler)(void *user_arg, AppCanPort port, const ProtocolAppFrame *frame);

/* 基于 func_code 的简单分发表。 */
typedef struct
{
    ServiceDispatcherHandler handlers[256];
    void *handler_user_args[256];
    ServiceDispatcherHandler default_handler;
    void *default_user_arg;
} ServiceDispatcher;

/* 初始化分发表。 */
void Service_Dispatcher_Init(ServiceDispatcher *dispatcher);

/* 注册某个功能码的处理函数。 */
void Service_Dispatcher_Register(ServiceDispatcher *dispatcher, uint8_t func_code, ServiceDispatcherHandler handler, void *user_arg);

/* 设置默认处理函数。 */
void Service_Dispatcher_SetDefault(ServiceDispatcher *dispatcher, ServiceDispatcherHandler handler, void *user_arg);

/* 直接分发一条已解码的应用层帧。 */
bool Service_Dispatcher_DispatchFrame(ServiceDispatcher *dispatcher, AppCanPort port, const ProtocolAppFrame *frame);

/* 从原始字节流解码后再分发。 */
bool Service_Dispatcher_DispatchRaw(ServiceDispatcher *dispatcher, AppCanPort port, const uint8_t *payload, uint16_t length);

#ifdef __cplusplus
}
#endif

#endif
