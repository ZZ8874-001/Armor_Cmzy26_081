/**
 ******************************************************************************
 * @file    app_can.c
 * @brief   CAN 适配层（L432 绑定，接口见 app_can.h —— 拷贝自 isotp 栈，平台无关）。
 *
 * 设计文档 v1.5 第 9.2 节：单 CAN1、500kbps、FIFO0 排空中断、TX 邮箱等待
 * （timeout 10ms）、Bus-Off 后 Stop/Start 自动恢复。
 *
 * 骨架阶段：Init 已可用（滤波器全接收 + Start + 通知使能）；
 * TODO(Step 4)：App_Can_Send/SendStd 邮箱发送实现、RX FIFO0 排空分发、
 * ErrorCallback 恢复逻辑与诊断计数。
 ******************************************************************************
 */
#include "app_can.h"
#include "board.h"

static AppCanRxCallback s_rx_callback = NULL;
static AppCanDiag       s_diag = {0};

AppCanStatus App_Can_Init(void)
{
    CAN_FilterTypeDef filter = {0};

    /* 滤波器 bank0：全接收（协议为应用层路由，9.2） */
    filter.FilterActivation      = CAN_FILTER_ENABLE;
    filter.FilterBank            = 0u;
    filter.FilterFIFOAssignment  = CAN_FILTER_FIFO0;
    filter.FilterIdHigh          = 0x0000u;
    filter.FilterIdLow           = 0x0000u;
    filter.FilterMaskIdHigh      = 0x0000u;
    filter.FilterMaskIdLow       = 0x0000u;
    filter.FilterMode            = CAN_FILTERMODE_IDMASK;
    filter.FilterScale           = CAN_FILTERSCALE_32BIT;
    filter.SlaveStartFilterBank  = 0u;
    if (HAL_CAN_ConfigFilter(&hcan1, &filter) != HAL_OK)
    {
        return APP_CAN_STATUS_ERROR;
    }
    if (HAL_CAN_Start(&hcan1) != HAL_OK)
    {
        return APP_CAN_STATUS_ERROR;
    }
    if (HAL_CAN_ActivateNotification(&hcan1,
            CAN_IT_RX_FIFO0_MSG_PENDING | CAN_IT_ERROR_WARNING |
            CAN_IT_ERROR_PASSIVE | CAN_IT_BUSOFF | CAN_IT_LAST_ERROR_CODE) != HAL_OK)
    {
        return APP_CAN_STATUS_ERROR;
    }
    return APP_CAN_STATUS_OK;
}

AppCanStatus App_Can_Send(AppCanPort port, const AppCanFrame *frame, uint32_t timeout_ms)
{
    (void)port; (void)frame; (void)timeout_ms;
    /* TODO(Step 4)：等邮箱 → HAL_CAN_AddTxMessage → 等 TSR 完成（timeout 10ms，
     * 设计文档 9.2）。骨架阶段无发送需求。 */
    return APP_CAN_STATUS_ERROR;
}

AppCanStatus App_Can_SendStd(AppCanPort port, uint16_t std_id, const uint8_t *data,
                             uint8_t len, uint32_t timeout_ms)
{
    AppCanFrame frame;
    if (len > 8u)
    {
        return APP_CAN_STATUS_ERROR;
    }
    frame.can_id         = std_id;
    frame.is_extended_id = 0u;
    frame.dlc            = len;
    for (uint8_t i = 0u; i < len; i++)
    {
        frame.data[i] = data[i];
    }
    return App_Can_Send(port, &frame, timeout_ms);
}

void App_Can_SetRxCallback(AppCanRxCallback callback)
{
    s_rx_callback = callback;
}

void App_Can_GetDiag(AppCanPort port, AppCanDiag *diag)
{
    (void)port;
    if (diag != NULL)
    {
        *diag = s_diag;
    }
}

const char *App_Can_LastErrorToString(uint8_t lec)
{
    /* lec 为 ESR 寄存器 LEC[2:0] 原始值（CAN 规范编码），
     * 与 HAL 32 位 ErrorCode 标志（HAL_CAN_ERROR_*）不是一回事。 */
    switch (lec & 0x07u)
    {
        case 0u: return "none";
        case 1u: return "stuff";
        case 2u: return "form";
        case 3u: return "ack";
        case 4u: return "bit_rec";
        case 5u: return "bit_dom";
        case 6u: return "crc";
        default: return "unknown";
    }
}

/* ==================== weak 回调（it.c 已备好 CAN1_RX0/SCE_IRQHandler） ==================== */

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    if (hcan->Instance != CAN1)
    {
        return;
    }
    /* TODO(Step 4)：FIFO0 排空 → AppCanFrame → s_rx_callback 分发（9.2）。 */
}

void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan)
{
    if (hcan->Instance != CAN1)
    {
        return;
    }
    /* TODO(Step 4)：记录 LEC；Bus-Off 后 HAL_CAN_Stop/Start 自动恢复。 */
}
