#ifndef SERVICE_RETRY_ACK_SCHEDULER_H
#define SERVICE_RETRY_ACK_SCHEDULER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "protocol/app_frame.h"
#include "transport/isotp.h"

/* 调度器队列深度。 */
#define SERVICE_RETRY_ACK_QUEUE_DEPTH 8U

/* 调度器发送函数。 */
typedef bool (*ServiceRetryAckSendFn)(void *user_arg, const uint8_t *payload, uint16_t length);

/* 查询底层是否空闲。 */
typedef bool (*ServiceRetryAckIdleFn)(void *user_arg);

/* 获取毫秒时基。 */
typedef uint32_t (*ServiceRetryAckGetMsFn)(void);
typedef uint16_t ServiceRetryAckSlotId;

/* 调度器返回值。 */
typedef enum
{
    SERVICE_RETRY_ACK_STATUS_OK = 0,
    SERVICE_RETRY_ACK_STATUS_ARG,
    SERVICE_RETRY_ACK_STATUS_QUEUE_FULL,
    SERVICE_RETRY_ACK_STATUS_NOT_FOUND,
    SERVICE_RETRY_ACK_STATUS_SEND_FAILED
} ServiceRetryAckStatus;

/* 调度器事件类型。 */
typedef enum
{
    SERVICE_RETRY_ACK_EVENT_ENQUEUED = 0,
    SERVICE_RETRY_ACK_EVENT_SENT,
    SERVICE_RETRY_ACK_EVENT_ACK_OK,
    SERVICE_RETRY_ACK_EVENT_ACK_REJECTED,
    SERVICE_RETRY_ACK_EVENT_RETRYING,
    SERVICE_RETRY_ACK_EVENT_TIMEOUT_DROPPED,
    SERVICE_RETRY_ACK_EVENT_SEND_FAILED,
    SERVICE_RETRY_ACK_EVENT_CANCELLED
} ServiceRetryAckEvent;

/* ACK 结果摘要。 */
typedef struct
{
    uint8_t result_code;
    uint8_t reason_code;
    bool has_reason;
} ServiceRetryAckInfo;

struct ServiceRetryAckScheduler;

/* 调度器事件回调。 */
typedef void (*ServiceRetryAckEventCallback)(struct ServiceRetryAckScheduler *scheduler,
                                             ServiceRetryAckEvent event,
                                             ServiceRetryAckSlotId slot_id,
                                             const ProtocolAppFrame *frame,
                                             const ServiceRetryAckInfo *ack_info,
                                             void *user_arg);

/* 队列中单条消息的内部状态。 */
typedef struct
{
    ProtocolAppFrame frame;
    ProtocolPriority priority;
    ServiceRetryAckSlotId slot_id;
    uint16_t retry_timeout_ms;
    uint8_t retries_remaining;
    bool require_ack;
    bool in_use;
    bool waiting_ack;
    bool sent_once;
    bool tx_in_progress;
} ServiceRetryAckEntry;

/* ACK 重试调度器。
 * 当前每个实例同一时刻只维护一个 in-flight ACK 等待窗口。
 */
typedef struct ServiceRetryAckScheduler
{
    ServiceRetryAckEntry queue[SERVICE_RETRY_ACK_QUEUE_DEPTH];
    ServiceRetryAckSendFn send_fn;
    ServiceRetryAckIdleFn idle_fn;
    ServiceRetryAckGetMsFn get_ms;
    void *user_arg;
    ServiceRetryAckEventCallback event_cb;
    void *event_user_arg;
    uint8_t next_seq;
    ServiceRetryAckSlotId next_slot_id;
    int8_t tx_active_index;
    int8_t waiting_index;
    uint32_t waiting_deadline_tick;
} ServiceRetryAckScheduler;

/* 初始化调度器。 */
void Service_RetryAckScheduler_Init(ServiceRetryAckScheduler *scheduler,
                                    ServiceRetryAckSendFn send_fn,
                                    ServiceRetryAckIdleFn idle_fn,
                                    ServiceRetryAckGetMsFn get_ms,
                                    void *user_arg);

/* 设置事件回调。 */
void Service_RetryAckScheduler_SetEventCallback(ServiceRetryAckScheduler *scheduler,
                                                ServiceRetryAckEventCallback event_cb,
                                                void *event_user_arg);

/* 入队一条应用层消息。 */
ServiceRetryAckStatus Service_RetryAckScheduler_Enqueue(ServiceRetryAckScheduler *scheduler,
                                                        const ProtocolAppFrame *frame,
                                                        ProtocolPriority priority,
                                                        bool require_ack,
                                                        uint8_t retry_count,
                                                        uint16_t retry_timeout_ms,
                                                        ServiceRetryAckSlotId *out_slot_id);

/* 周期轮询调度器。 */
void Service_RetryAckScheduler_Poll(ServiceRetryAckScheduler *scheduler);

/* 将收到的应用层帧喂给调度器，用于 ACK 匹配。 */
void Service_RetryAckScheduler_OnReceivedFrame(ServiceRetryAckScheduler *scheduler, const ProtocolAppFrame *frame);

/* 通知调度器：底层一次 ISO-TP 发送已真正结束。 */
void Service_RetryAckScheduler_OnTransmitComplete(ServiceRetryAckScheduler *scheduler, TransportIsotpStatus status);

/* 按槽位取消一条消息。 */
ServiceRetryAckStatus Service_RetryAckScheduler_Cancel(ServiceRetryAckScheduler *scheduler, ServiceRetryAckSlotId slot_id);

/* 清空调度器。 */
void Service_RetryAckScheduler_Reset(ServiceRetryAckScheduler *scheduler);

/* 查询是否仍有待处理消息。 */
bool Service_RetryAckScheduler_HasPending(const ServiceRetryAckScheduler *scheduler);

/* 获取当前待处理消息数量。 */
uint8_t Service_RetryAckScheduler_PendingCount(const ServiceRetryAckScheduler *scheduler);

#ifdef __cplusplus
}
#endif

#endif
