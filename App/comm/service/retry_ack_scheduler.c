#include "service/retry_ack_scheduler.h"

#include <string.h>

#include "protocol/app_ack.h"
#include "protocol/tlv.h"

static uint8_t Service_RetryAckScheduler_AllocateSeq(ServiceRetryAckScheduler *scheduler)
{
    if ((scheduler == 0) || (scheduler->next_seq == 0U))
    {
        scheduler->next_seq = 1U;
    }

    return scheduler->next_seq;
}

static void Service_RetryAckScheduler_AdvanceSeq(ServiceRetryAckScheduler *scheduler)
{
    if (scheduler == 0)
    {
        return;
    }

    scheduler->next_seq++;
    if (scheduler->next_seq == 0U)
    {
        scheduler->next_seq = 1U;
    }
}

static ServiceRetryAckSlotId Service_RetryAckScheduler_AllocateSlotId(ServiceRetryAckScheduler *scheduler)
{
    scheduler->next_slot_id++;
    if (scheduler->next_slot_id == 0U)
    {
        scheduler->next_slot_id = 1U;
    }

    return scheduler->next_slot_id;
}

static void Service_RetryAckScheduler_ClearEntry(ServiceRetryAckScheduler *scheduler, ServiceRetryAckEntry *entry)
{
    if ((scheduler != 0) && (entry != 0))
    {
        if ((scheduler->waiting_index >= 0) && (entry == &scheduler->queue[scheduler->waiting_index]))
        {
            scheduler->waiting_index = -1;
        }
        if ((scheduler->tx_active_index >= 0) && (entry == &scheduler->queue[scheduler->tx_active_index]))
        {
            scheduler->tx_active_index = -1;
        }
    }

    if (entry != 0)
    {
        memset(entry, 0, sizeof(*entry));
    }
}

static void Service_RetryAckScheduler_Notify(ServiceRetryAckScheduler *scheduler,
                                             ServiceRetryAckEvent event,
                                             const ServiceRetryAckEntry *entry,
                                             const ServiceRetryAckInfo *ack_info)
{
    if ((scheduler == 0) || (scheduler->event_cb == 0) || (entry == 0))
    {
        return;
    }

    scheduler->event_cb(scheduler,
                        event,
                        entry->slot_id,
                        &entry->frame,
                        ack_info,
                        scheduler->event_user_arg);
}

static bool Service_RetryAckScheduler_ParseAckInfo(const ProtocolAppFrame *frame, ServiceRetryAckInfo *ack_info)
{
    ProtocolAppAckInfo parsed;

    if ((frame == 0) || (ack_info == 0))
    {
        return false;
    }

    if (!Protocol_AppAck_Parse(frame, &parsed))
    {
        return false;
    }

    ack_info->result_code = 0U;
    ack_info->reason_code = 0U;
    ack_info->has_reason = false;
    ack_info->result_code = parsed.result_code;
    ack_info->reason_code = parsed.reason_code;
    ack_info->has_reason = parsed.has_reason;
    return true;
}

static ServiceRetryAckStatus Service_RetryAckScheduler_SendEntry(ServiceRetryAckScheduler *scheduler, int8_t index)
{
    uint8_t encoded[PROTOCOL_APP_FRAME_MAX_ENCODED_SIZE];
    uint16_t encoded_len = 0U;
    ServiceRetryAckEntry *entry;

    if ((scheduler == 0) || (index < 0) || (index >= (int8_t)SERVICE_RETRY_ACK_QUEUE_DEPTH))
    {
        return SERVICE_RETRY_ACK_STATUS_ARG;
    }

    entry = &scheduler->queue[index];
    if (!Protocol_AppFrame_Encode(&entry->frame, encoded, &encoded_len, sizeof(encoded)))
    {
        Service_RetryAckScheduler_Notify(scheduler, SERVICE_RETRY_ACK_EVENT_SEND_FAILED, entry, 0);
        Service_RetryAckScheduler_ClearEntry(scheduler, entry);
        return SERVICE_RETRY_ACK_STATUS_SEND_FAILED;
    }

    if (!scheduler->send_fn(scheduler->user_arg, encoded, encoded_len))
    {
        Service_RetryAckScheduler_Notify(scheduler, SERVICE_RETRY_ACK_EVENT_SEND_FAILED, entry, 0);
        return SERVICE_RETRY_ACK_STATUS_SEND_FAILED;
    }

    entry->sent_once = true;
    entry->tx_in_progress = true;
    scheduler->tx_active_index = index;
    Service_RetryAckScheduler_Notify(scheduler, SERVICE_RETRY_ACK_EVENT_SENT, entry, 0);
    if (entry->require_ack)
    {
        entry->waiting_ack = false;
    }
    else
    {
        if (!entry->tx_in_progress)
        {
            Service_RetryAckScheduler_ClearEntry(scheduler, entry);
        }
    }

    return SERVICE_RETRY_ACK_STATUS_OK;
}

static int8_t Service_RetryAckScheduler_FindNextReady(ServiceRetryAckScheduler *scheduler)
{
    int8_t found = -1;
    ProtocolPriority best_priority = PROTOCOL_PRIORITY_LOW;

    for (int8_t i = 0; i < (int8_t)SERVICE_RETRY_ACK_QUEUE_DEPTH; ++i)
    {
        const ServiceRetryAckEntry *entry = &scheduler->queue[i];
        if (!entry->in_use || entry->waiting_ack)
        {
            continue;
        }

        if ((found < 0) || (entry->priority > best_priority))
        {
            found = i;
            best_priority = entry->priority;
        }
    }

    return found;
}

void Service_RetryAckScheduler_Init(ServiceRetryAckScheduler *scheduler,
                                    ServiceRetryAckSendFn send_fn,
                                    ServiceRetryAckIdleFn idle_fn,
                                    ServiceRetryAckGetMsFn get_ms,
                                    void *user_arg)
{
    if (scheduler == 0)
    {
        return;
    }

    memset(scheduler, 0, sizeof(*scheduler));
    scheduler->send_fn = send_fn;
    scheduler->idle_fn = idle_fn;
    scheduler->get_ms = get_ms;
    scheduler->user_arg = user_arg;
    scheduler->tx_active_index = -1;
    scheduler->waiting_index = -1;
}

void Service_RetryAckScheduler_SetEventCallback(ServiceRetryAckScheduler *scheduler,
                                                ServiceRetryAckEventCallback event_cb,
                                                void *event_user_arg)
{
    if (scheduler == 0)
    {
        return;
    }

    scheduler->event_cb = event_cb;
    scheduler->event_user_arg = event_user_arg;
}

ServiceRetryAckStatus Service_RetryAckScheduler_Enqueue(ServiceRetryAckScheduler *scheduler,
                                                        const ProtocolAppFrame *frame,
                                                        ProtocolPriority priority,
                                                        bool require_ack,
                                                        uint8_t retry_count,
                                                        uint16_t retry_timeout_ms,
                                                        ServiceRetryAckSlotId *out_slot_id)
{
    if ((scheduler == 0) || (frame == 0))
    {
        return SERVICE_RETRY_ACK_STATUS_ARG;
    }

    for (uint8_t i = 0; i < SERVICE_RETRY_ACK_QUEUE_DEPTH; ++i)
    {
        ServiceRetryAckEntry *entry = &scheduler->queue[i];
        if (entry->in_use)
        {
            continue;
        }

        memset(entry, 0, sizeof(*entry));
        entry->frame = *frame;
        entry->priority = priority;
        entry->require_ack = require_ack;
        entry->retry_timeout_ms = retry_timeout_ms;
        entry->retries_remaining = retry_count;
        entry->in_use = true;
        entry->slot_id = Service_RetryAckScheduler_AllocateSlotId(scheduler);

        if (require_ack && (entry->frame.seq == 0U))
        {
            entry->frame.seq = Service_RetryAckScheduler_AllocateSeq(scheduler);
        }

        if (out_slot_id != 0)
        {
            *out_slot_id = entry->slot_id;
        }

        Service_RetryAckScheduler_Notify(scheduler, SERVICE_RETRY_ACK_EVENT_ENQUEUED, entry, 0);
        return SERVICE_RETRY_ACK_STATUS_OK;
    }

    return SERVICE_RETRY_ACK_STATUS_QUEUE_FULL;
}

void Service_RetryAckScheduler_Poll(ServiceRetryAckScheduler *scheduler)
{
    uint32_t now;
    int8_t index;
    ServiceRetryAckEntry *entry;

    if ((scheduler == 0) || (scheduler->send_fn == 0) || (scheduler->idle_fn == 0) || (scheduler->get_ms == 0))
    {
        return;
    }

    now = scheduler->get_ms();

    if (scheduler->waiting_index >= 0)
    {
        entry = &scheduler->queue[scheduler->waiting_index];
        if ((int32_t)(now - scheduler->waiting_deadline_tick) >= 0)
        {
            if (entry->retries_remaining > 0U)
            {
                entry->retries_remaining--;
                entry->waiting_ack = false;
                scheduler->waiting_index = -1;
                Service_RetryAckScheduler_Notify(scheduler, SERVICE_RETRY_ACK_EVENT_RETRYING, entry, 0);
            }
            else
            {
                Service_RetryAckScheduler_Notify(scheduler, SERVICE_RETRY_ACK_EVENT_TIMEOUT_DROPPED, entry, 0);
                Service_RetryAckScheduler_ClearEntry(scheduler, entry);
            }
        }
    }

    if ((scheduler->waiting_index >= 0) || (scheduler->tx_active_index >= 0))
    {
        return;
    }

    if (!scheduler->idle_fn(scheduler->user_arg))
    {
        return;
    }

    index = Service_RetryAckScheduler_FindNextReady(scheduler);
    if (index >= 0)
    {
        (void)Service_RetryAckScheduler_SendEntry(scheduler, index);
    }
}

void Service_RetryAckScheduler_OnReceivedFrame(ServiceRetryAckScheduler *scheduler, const ProtocolAppFrame *frame)
{
    ServiceRetryAckEntry *entry;
    ServiceRetryAckInfo ack_info;
    bool ack_ok;

    if ((scheduler == 0) || (frame == 0) || (scheduler->waiting_index < 0))
    {
        return;
    }

    entry = &scheduler->queue[scheduler->waiting_index];
    if (!Protocol_AppAck_MatchesRequest(&entry->frame, frame, 0))
    {
        return;
    }

    if (!Service_RetryAckScheduler_ParseAckInfo(frame, &ack_info))
    {
        ack_info.result_code = 0U;
        ack_info.reason_code = 0U;
        ack_info.has_reason = false;
    }

    ack_ok = (ack_info.result_code == 0U);
    Service_RetryAckScheduler_Notify(scheduler,
                                     ack_ok ? SERVICE_RETRY_ACK_EVENT_ACK_OK : SERVICE_RETRY_ACK_EVENT_ACK_REJECTED,
                                     entry,
                                     &ack_info);
    Service_RetryAckScheduler_AdvanceSeq(scheduler);
    Service_RetryAckScheduler_ClearEntry(scheduler, entry);
}

void Service_RetryAckScheduler_OnTransmitComplete(ServiceRetryAckScheduler *scheduler, TransportIsotpStatus status)
{
    ServiceRetryAckEntry *entry;

    if ((scheduler == 0) || (scheduler->tx_active_index < 0))
    {
        return;
    }

    entry = &scheduler->queue[scheduler->tx_active_index];
    entry->tx_in_progress = false;

    if (status != TRANSPORT_ISOTP_OK)
    {
        Service_RetryAckScheduler_Notify(scheduler, SERVICE_RETRY_ACK_EVENT_SEND_FAILED, entry, 0);
        Service_RetryAckScheduler_ClearEntry(scheduler, entry);
        return;
    }

    if (entry->require_ack)
    {
        entry->waiting_ack = true;
        scheduler->waiting_index = scheduler->tx_active_index;
        scheduler->waiting_deadline_tick = scheduler->get_ms() + entry->retry_timeout_ms;
        scheduler->tx_active_index = -1;
        return;
    }

    Service_RetryAckScheduler_ClearEntry(scheduler, entry);
}

ServiceRetryAckStatus Service_RetryAckScheduler_Cancel(ServiceRetryAckScheduler *scheduler, ServiceRetryAckSlotId slot_id)
{
    if ((scheduler == 0) || (slot_id == 0U))
    {
        return SERVICE_RETRY_ACK_STATUS_ARG;
    }

    for (uint8_t i = 0; i < SERVICE_RETRY_ACK_QUEUE_DEPTH; ++i)
    {
        ServiceRetryAckEntry *entry = &scheduler->queue[i];
        if (!entry->in_use || (entry->slot_id != slot_id))
        {
            continue;
        }

        Service_RetryAckScheduler_Notify(scheduler, SERVICE_RETRY_ACK_EVENT_CANCELLED, entry, 0);
        Service_RetryAckScheduler_ClearEntry(scheduler, entry);
        return SERVICE_RETRY_ACK_STATUS_OK;
    }

    return SERVICE_RETRY_ACK_STATUS_NOT_FOUND;
}

void Service_RetryAckScheduler_Reset(ServiceRetryAckScheduler *scheduler)
{
    if (scheduler == 0)
    {
        return;
    }

    memset(scheduler->queue, 0, sizeof(scheduler->queue));
    scheduler->tx_active_index = -1;
    scheduler->waiting_index = -1;
}

bool Service_RetryAckScheduler_HasPending(const ServiceRetryAckScheduler *scheduler)
{
    return Service_RetryAckScheduler_PendingCount(scheduler) > 0U;
}

uint8_t Service_RetryAckScheduler_PendingCount(const ServiceRetryAckScheduler *scheduler)
{
    uint8_t count = 0U;

    if (scheduler == 0)
    {
        return 0U;
    }

    for (uint8_t i = 0; i < SERVICE_RETRY_ACK_QUEUE_DEPTH; ++i)
    {
        if (scheduler->queue[i].in_use)
        {
            count++;
        }
    }

    return count;
}
