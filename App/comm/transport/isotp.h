#ifndef TRANSPORT_ISOTP_H
#define TRANSPORT_ISOTP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "app_can.h"

/* ISO-TP 缓冲区上限。
 * 当前应用层完整帧最大 263 字节，因此 512 字节足够覆盖《通信协议v2》的现有需求。
 */
#define TRANSPORT_ISOTP_MAX_MESSAGE_SIZE 512U

/* ISO-TP 接口返回值。 */
typedef enum
{
    TRANSPORT_ISOTP_OK = 0,
    TRANSPORT_ISOTP_BUSY,
    TRANSPORT_ISOTP_ERROR_ARG,
    TRANSPORT_ISOTP_ERROR_TX,
    TRANSPORT_ISOTP_ERROR_TIMEOUT,
    TRANSPORT_ISOTP_ERROR_OVERFLOW
} TransportIsotpStatus;

/* 获取毫秒时基，要求单调递增。 */
typedef uint32_t (*TransportIsotpGetMsFn)(void);

/* 收到完整 ISO-TP 消息后的回调。 */
typedef void (*TransportIsotpMessageCallback)(void *user_arg, const uint8_t *payload, uint16_t length);

/* 原始分帧日志回调，仅用于调试。 */
typedef void (*TransportIsotpLogFrameCallback)(void *user_arg, AppCanPort port, const char *direction, const AppCanFrame *frame);

/* 一条 ISO-TP 发送完全结束后的回调。
 * 单帧发送成功、多帧全部发送成功、发送失败、流控超时都会走这里。
 */
typedef void (*TransportIsotpTxCompleteCallback)(void *user_arg, TransportIsotpStatus status);

/* ISO-TP 通道配置。
 * 移植说明：
 * - 本结构与 HAL 解耦，可直接复用于 F1/F4/L4 或其他 MCU。
 */
typedef struct
{
    AppCanPort port;
    uint16_t tx_id;
    uint16_t rx_id;
    uint8_t block_size;
    uint8_t st_min_ms;
    uint16_t tx_timeout_ms;
    bool tx_require_flow_control;
    TransportIsotpGetMsFn get_ms;
    TransportIsotpMessageCallback on_message;
    TransportIsotpLogFrameCallback on_frame;
    TransportIsotpTxCompleteCallback on_tx_complete;
    void *user_arg;
} TransportIsotpConfig;

/* ISO-TP 上下文。
 * 每个独立逻辑通道都应持有一个独立 context。
 */
typedef struct
{
    TransportIsotpConfig cfg;
    uint8_t tx_buffer[TRANSPORT_ISOTP_MAX_MESSAGE_SIZE];
    uint16_t tx_length;
    uint16_t tx_offset;
    uint8_t tx_sn;
    uint8_t tx_block_counter;
    uint8_t rx_buffer[TRANSPORT_ISOTP_MAX_MESSAGE_SIZE];
    uint16_t rx_length;
    uint16_t rx_offset;
    uint8_t rx_next_sn;
    uint32_t tx_deadline_tick;
    uint32_t tx_next_tick;
    uint8_t tx_state;
    TransportIsotpStatus last_error;
} TransportIsotpContext;

/* 初始化 ISO-TP 上下文。 */
void Transport_Isotp_Init(TransportIsotpContext *ctx, const TransportIsotpConfig *config);

/* 周期轮询 ISO-TP 状态机。 */
void Transport_Isotp_Poll(TransportIsotpContext *ctx);

/* 查询当前发送通道是否空闲。 */
bool Transport_Isotp_IsIdle(const TransportIsotpContext *ctx);

/* 发送一条完整上层消息，内部自动拆帧。 */
TransportIsotpStatus Transport_Isotp_Send(TransportIsotpContext *ctx, const uint8_t *payload, uint16_t length);

/* 将原始 CAN 帧喂给 ISO-TP 接收状态机。 */
void Transport_Isotp_OnCanFrame(TransportIsotpContext *ctx, AppCanPort port, const AppCanFrame *frame);

#ifdef __cplusplus
}
#endif

#endif
