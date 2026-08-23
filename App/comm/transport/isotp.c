#include "transport/isotp.h"

#include <string.h>

#define TRANSPORT_ISOTP_TX_TIMEOUT_MS 100U

enum
{
    TRANSPORT_ISOTP_TX_STATE_IDLE = 0,
    TRANSPORT_ISOTP_TX_STATE_WAIT_FC,
    TRANSPORT_ISOTP_TX_STATE_SEND_CF
};

static uint32_t Transport_Isotp_GetMs(const TransportIsotpContext *ctx)
{
    if (ctx->cfg.get_ms != 0)
    {
        return ctx->cfg.get_ms();
    }

    return 0U;
}

static uint32_t Transport_Isotp_GetTimeout(const TransportIsotpContext *ctx)
{
    if (ctx->cfg.tx_timeout_ms == 0U)
    {
        return TRANSPORT_ISOTP_TX_TIMEOUT_MS;
    }

    return ctx->cfg.tx_timeout_ms;
}

static void Transport_Isotp_NotifyTxComplete(TransportIsotpContext *ctx, TransportIsotpStatus status)
{
    if ((ctx != 0) && (ctx->cfg.on_tx_complete != 0))
    {
        ctx->cfg.on_tx_complete(ctx->cfg.user_arg, status);
    }
}

static void Transport_Isotp_ResetTx(TransportIsotpContext *ctx)
{
    ctx->tx_state = TRANSPORT_ISOTP_TX_STATE_IDLE;
    ctx->tx_length = 0U;
    ctx->tx_offset = 0U;
    ctx->tx_sn = 1U;
    ctx->tx_block_counter = 0U;
    ctx->tx_deadline_tick = 0U;
    ctx->tx_next_tick = 0U;
}

static void Transport_Isotp_LogFrame(TransportIsotpContext *ctx, const char *direction, const AppCanFrame *frame)
{
    if (ctx->cfg.on_frame != 0)
    {
        ctx->cfg.on_frame(ctx->cfg.user_arg, ctx->cfg.port, direction, frame);
    }
}

static void Transport_Isotp_SendFlowControl(TransportIsotpContext *ctx, uint8_t flow_status, uint8_t block_size, uint8_t st_min)
{
    AppCanFrame frame = {0};

    frame.can_id = ctx->cfg.tx_id;
    frame.is_extended_id = 0U;
    frame.dlc = 8U;
    frame.data[0] = (uint8_t)(0x30U | (flow_status & 0x0FU));
    frame.data[1] = block_size;
    frame.data[2] = st_min;

    if (App_Can_Send(ctx->cfg.port, &frame, Transport_Isotp_GetTimeout(ctx)) == APP_CAN_STATUS_OK)
    {
        Transport_Isotp_LogFrame(ctx, "TX", &frame);
    }
}

static TransportIsotpStatus Transport_Isotp_SendConsecutiveFrame(TransportIsotpContext *ctx)
{
    AppCanFrame frame = {0};
    uint16_t bytes_left = (uint16_t)(ctx->tx_length - ctx->tx_offset);
    uint8_t chunk = (bytes_left > 7U) ? 7U : (uint8_t)bytes_left;

    frame.can_id = ctx->cfg.tx_id;
    frame.is_extended_id = 0U;
    frame.dlc = 8U;
    frame.data[0] = (uint8_t)(0x20U | (ctx->tx_sn & 0x0FU));
    memcpy(&frame.data[1], &ctx->tx_buffer[ctx->tx_offset], chunk);

    if (App_Can_Send(ctx->cfg.port, &frame, Transport_Isotp_GetTimeout(ctx)) != APP_CAN_STATUS_OK)
    {
        ctx->last_error = TRANSPORT_ISOTP_ERROR_TX;
        Transport_Isotp_NotifyTxComplete(ctx, TRANSPORT_ISOTP_ERROR_TX);
        Transport_Isotp_ResetTx(ctx);
        return TRANSPORT_ISOTP_ERROR_TX;
    }

    Transport_Isotp_LogFrame(ctx, "TX", &frame);
    ctx->tx_offset = (uint16_t)(ctx->tx_offset + chunk);
    ctx->tx_sn = (uint8_t)((ctx->tx_sn + 1U) & 0x0FU);

    if (ctx->tx_offset >= ctx->tx_length)
    {
        Transport_Isotp_NotifyTxComplete(ctx, TRANSPORT_ISOTP_OK);
        Transport_Isotp_ResetTx(ctx);
        return TRANSPORT_ISOTP_OK;
    }

    ctx->tx_block_counter++;
    ctx->tx_deadline_tick = Transport_Isotp_GetMs(ctx) + Transport_Isotp_GetTimeout(ctx);
    ctx->tx_next_tick = Transport_Isotp_GetMs(ctx) + ctx->cfg.st_min_ms;

    if (ctx->cfg.tx_require_flow_control &&
        (ctx->cfg.block_size != 0U) &&
        (ctx->tx_block_counter >= ctx->cfg.block_size))
    {
        ctx->tx_block_counter = 0U;
        ctx->tx_state = TRANSPORT_ISOTP_TX_STATE_WAIT_FC;
    }

    return TRANSPORT_ISOTP_OK;
}

void Transport_Isotp_Init(TransportIsotpContext *ctx, const TransportIsotpConfig *config)
{
    if ((ctx == 0) || (config == 0))
    {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg = *config;
    ctx->tx_state = TRANSPORT_ISOTP_TX_STATE_IDLE;
}

void Transport_Isotp_Poll(TransportIsotpContext *ctx)
{
    if (ctx == 0)
    {
        return;
    }

    uint32_t now = Transport_Isotp_GetMs(ctx);

    if ((ctx->tx_state == TRANSPORT_ISOTP_TX_STATE_WAIT_FC) &&
        ((int32_t)(now - ctx->tx_deadline_tick) >= 0))
    {
        ctx->last_error = TRANSPORT_ISOTP_ERROR_TIMEOUT;
        Transport_Isotp_NotifyTxComplete(ctx, TRANSPORT_ISOTP_ERROR_TIMEOUT);
        Transport_Isotp_ResetTx(ctx);
        return;
    }

    if ((ctx->tx_state == TRANSPORT_ISOTP_TX_STATE_SEND_CF) &&
        ((int32_t)(now - ctx->tx_next_tick) >= 0))
    {
        (void)Transport_Isotp_SendConsecutiveFrame(ctx);
    }
}

bool Transport_Isotp_IsIdle(const TransportIsotpContext *ctx)
{
    if (ctx == 0)
    {
        return true;
    }

    return (ctx->tx_state == TRANSPORT_ISOTP_TX_STATE_IDLE);
}

TransportIsotpStatus Transport_Isotp_Send(TransportIsotpContext *ctx, const uint8_t *payload, uint16_t length)
{
    AppCanFrame frame = {0};

    if ((ctx == 0) || (payload == 0) || (length == 0U) || (length > TRANSPORT_ISOTP_MAX_MESSAGE_SIZE))
    {
        return TRANSPORT_ISOTP_ERROR_ARG;
    }

    if (!Transport_Isotp_IsIdle(ctx))
    {
        return TRANSPORT_ISOTP_BUSY;
    }

    frame.can_id = ctx->cfg.tx_id;
    frame.is_extended_id = 0U;
    frame.dlc = 8U;

    if (length <= 7U)
    {
        frame.data[0] = (uint8_t)(length & 0x0FU);
        memcpy(&frame.data[1], payload, length);

        if (App_Can_Send(ctx->cfg.port, &frame, Transport_Isotp_GetTimeout(ctx)) != APP_CAN_STATUS_OK)
        {
            Transport_Isotp_NotifyTxComplete(ctx, TRANSPORT_ISOTP_ERROR_TX);
            return TRANSPORT_ISOTP_ERROR_TX;
        }

        Transport_Isotp_LogFrame(ctx, "TX", &frame);
        Transport_Isotp_NotifyTxComplete(ctx, TRANSPORT_ISOTP_OK);
        return TRANSPORT_ISOTP_OK;
    }

    memcpy(ctx->tx_buffer, payload, length);
    ctx->tx_length = length;
    ctx->tx_offset = 6U;
    ctx->tx_sn = 1U;
    ctx->tx_block_counter = 0U;

    frame.data[0] = (uint8_t)(0x10U | ((length >> 8) & 0x0FU));
    frame.data[1] = (uint8_t)(length & 0xFFU);
    memcpy(&frame.data[2], payload, 6U);

    ctx->tx_deadline_tick = Transport_Isotp_GetMs(ctx) + Transport_Isotp_GetTimeout(ctx);
    if (ctx->cfg.tx_require_flow_control)
    {
        ctx->tx_state = TRANSPORT_ISOTP_TX_STATE_WAIT_FC;
    }
    else
    {
        ctx->tx_state = TRANSPORT_ISOTP_TX_STATE_SEND_CF;
        ctx->tx_next_tick = Transport_Isotp_GetMs(ctx) + ctx->cfg.st_min_ms;
    }

    if (App_Can_Send(ctx->cfg.port, &frame, Transport_Isotp_GetTimeout(ctx)) != APP_CAN_STATUS_OK)
    {
        Transport_Isotp_NotifyTxComplete(ctx, TRANSPORT_ISOTP_ERROR_TX);
        Transport_Isotp_ResetTx(ctx);
        return TRANSPORT_ISOTP_ERROR_TX;
    }

    Transport_Isotp_LogFrame(ctx, "TX", &frame);

    return TRANSPORT_ISOTP_OK;
}

void Transport_Isotp_OnCanFrame(TransportIsotpContext *ctx, AppCanPort port, const AppCanFrame *frame)
{
    uint8_t pci_type;

    if ((ctx == 0) ||
        (frame == 0) ||
        (port != ctx->cfg.port) ||
        frame->is_extended_id ||
        (frame->can_id != ctx->cfg.rx_id) ||
        (frame->dlc == 0U))
    {
        return;
    }

    Transport_Isotp_LogFrame(ctx, "RX", frame);
    pci_type = (uint8_t)(frame->data[0] >> 4);

    switch (pci_type)
    {
    case 0x0U:
    {
        uint8_t sf_length = (uint8_t)(frame->data[0] & 0x0FU);
        if ((ctx->cfg.on_message != 0) && (sf_length <= 7U))
        {
            ctx->cfg.on_message(ctx->cfg.user_arg, &frame->data[1], sf_length);
        }
        break;
    }

    case 0x1U:
    {
        uint16_t total_length = (uint16_t)(((frame->data[0] & 0x0FU) << 8) | frame->data[1]);
        uint8_t first_chunk = (total_length > 6U) ? 6U : (uint8_t)total_length;

        if (total_length > TRANSPORT_ISOTP_MAX_MESSAGE_SIZE)
        {
            Transport_Isotp_SendFlowControl(ctx, 0x02U, 0U, 0U);
            return;
        }

        memset(ctx->rx_buffer, 0, sizeof(ctx->rx_buffer));
        memcpy(ctx->rx_buffer, &frame->data[2], first_chunk);
        ctx->rx_length = total_length;
        ctx->rx_offset = first_chunk;
        ctx->rx_next_sn = 1U;

        Transport_Isotp_SendFlowControl(ctx, 0x00U, ctx->cfg.block_size, ctx->cfg.st_min_ms);
        if ((ctx->rx_offset >= ctx->rx_length) && (ctx->cfg.on_message != 0))
        {
            ctx->cfg.on_message(ctx->cfg.user_arg, ctx->rx_buffer, ctx->rx_length);
        }
        break;
    }

    case 0x2U:
        if ((frame->data[0] & 0x0FU) != ctx->rx_next_sn)
        {
            ctx->rx_offset = 0U;
            ctx->rx_length = 0U;
            break;
        }

        if (ctx->rx_offset < ctx->rx_length)
        {
            uint16_t remaining = (uint16_t)(ctx->rx_length - ctx->rx_offset);
            uint8_t chunk = (remaining > 7U) ? 7U : (uint8_t)remaining;
            memcpy(&ctx->rx_buffer[ctx->rx_offset], &frame->data[1], chunk);
            ctx->rx_offset = (uint16_t)(ctx->rx_offset + chunk);
            ctx->rx_next_sn = (uint8_t)((ctx->rx_next_sn + 1U) & 0x0FU);
        }

        if ((ctx->rx_offset >= ctx->rx_length) && (ctx->cfg.on_message != 0))
        {
            ctx->cfg.on_message(ctx->cfg.user_arg, ctx->rx_buffer, ctx->rx_length);
        }
        break;

    case 0x3U:
        if (!ctx->cfg.tx_require_flow_control || (ctx->tx_state != TRANSPORT_ISOTP_TX_STATE_WAIT_FC))
        {
            break;
        }

        if ((frame->data[0] & 0x0FU) == 0x00U)
        {
            ctx->cfg.block_size = frame->data[1];
            ctx->cfg.st_min_ms = frame->data[2];
            ctx->tx_block_counter = 0U;
            ctx->tx_state = TRANSPORT_ISOTP_TX_STATE_SEND_CF;
            ctx->tx_next_tick = Transport_Isotp_GetMs(ctx);
            ctx->tx_deadline_tick = Transport_Isotp_GetMs(ctx) + Transport_Isotp_GetTimeout(ctx);
        }
        else
        {
            ctx->last_error = TRANSPORT_ISOTP_ERROR_TIMEOUT;
            Transport_Isotp_NotifyTxComplete(ctx, TRANSPORT_ISOTP_ERROR_TIMEOUT);
            Transport_Isotp_ResetTx(ctx);
        }
        break;

    default:
        break;
    }
}
