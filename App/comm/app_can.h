#ifndef APP_CAN_H
#define APP_CAN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* CAN 端口抽象。
 * 说明：
 * 1. 对上层只暴露逻辑端口，不暴露 HAL 句柄。
 * 2. 移植到 F1/F4/L4 或其他平台时，建议只在 app_can.c 内部做端口到硬件实例的映射。
 */
typedef enum
{
    APP_CAN_PORT_1 = 0,
    APP_CAN_PORT_2 = 1,
    APP_CAN_PORT_COUNT
} AppCanPort;

/* 协议栈统一使用的 CAN 帧抽象。 */
typedef struct
{
    uint32_t can_id;
    uint8_t is_extended_id;
    uint8_t dlc;
    uint8_t data[8];
} AppCanFrame;

/* CAN 接收回调。
 * @param port   收到帧的逻辑 CAN 口。
 * @param frame  收到的 CAN 帧；若调用方要异步保存，请自行拷贝。
 */
typedef void (*AppCanRxCallback)(AppCanPort port, const AppCanFrame *frame);

/* CAN 适配层接口返回值。 */
typedef enum
{
    APP_CAN_STATUS_OK = 0,
    APP_CAN_STATUS_ERROR = -1,
    APP_CAN_STATUS_TIMEOUT = -2
} AppCanStatus;

/* CAN 健康状态快照。
 * 说明：
 * 1. 这里汇总了应用层和调试最常需要的状态。
 * 2. esr/tsr/rf0r 为原始寄存器值，便于底层问题排查。
 */
typedef struct
{
    uint8_t tec;
    uint8_t rec;
    uint8_t last_error_code;
    uint8_t error_warning;
    uint8_t error_passive;
    uint8_t bus_off;
    uint32_t hal_error;
    uint32_t esr;
    uint32_t tsr;
    uint32_t rf0r;
    uint32_t tx_ok_count;
    uint32_t tx_fail_count;
    uint32_t rx_irq_count;
    uint32_t rx_frame_count;
} AppCanDiag;

/* 初始化底层 CAN 适配层。
 * 返回值：
 * - APP_CAN_STATUS_OK      初始化成功
 * - APP_CAN_STATUS_ERROR   初始化失败
 * 移植说明：
 * - 换 MCU 时，原则上优先重写这个函数，而不是改 transport/protocol/service。
 */
AppCanStatus App_Can_Init(void);

/* 发送一帧 CAN。
 * @param port        逻辑 CAN 口。
 * @param frame       待发送帧。
 * @param timeout_ms  超时时间，单位毫秒。
 * 返回值：
 * - APP_CAN_STATUS_OK
 * - APP_CAN_STATUS_ERROR
 * - APP_CAN_STATUS_TIMEOUT
 */
AppCanStatus App_Can_Send(AppCanPort port, const AppCanFrame *frame, uint32_t timeout_ms);

/* 发送标准帧的便捷接口。 */
AppCanStatus App_Can_SendStd(AppCanPort port, uint16_t std_id, const uint8_t *data, uint8_t len, uint32_t timeout_ms);

/* 注册底层接收回调。
 * @param callback  接收回调，可为 0 表示取消注册。
 */
void App_Can_SetRxCallback(AppCanRxCallback callback);

/* 获取某个 CAN 口的诊断信息。 */
void App_Can_GetDiag(AppCanPort port, AppCanDiag *diag);

/* 将 LEC 错误码转为字符串。 */
const char *App_Can_LastErrorToString(uint8_t lec);

#ifdef __cplusplus
}
#endif

#endif
