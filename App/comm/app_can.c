/**
 ******************************************************************************
 * @file    app_can.c
 * @brief   CAN 适配层（L432 绑定，接口见 app_can.h —— 拷贝自 isotp 栈，平台无关）。
 *
 * 设计文档 v1.5 第 9.2 节：单 CAN1、500kbps（CubeMX 已配）、FIFO0 排空中断、
 * TX 邮箱等待（timeout 5ms 硬限）、NART=1 禁自动重传（死总线阻塞根治）、
 * TXOK 判定真实成败、Bus-Off 寄存器级非阻塞恢复、诊断快照。
 * Step 4 全实现（2026-08-21）；死总线适配修订（2026-09-05）。
 ******************************************************************************
 */
#include "comm/app_can.h"
#include "board.h"

#include <string.h>

static AppCanRxCallback s_rx_callback = NULL;
static volatile AppCanDiag s_diag;
static volatile uint32_t s_tx_mailbox_robin;   /* 发送邮箱轮转（0/1/2） */
static volatile uint8_t  s_busoff_recover;     /* Bus-Off 恢复请求标志（ISR 置位，主循环处理） */

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

    /* 死总线阻塞根治（2026-09-05）：禁用自动重传（NART=1）。
     * CubeMX 的 AutoRetransmission=ENABLE 使死总线上发送帧永不完成（RQCP 不来），
     * 每次发送阻塞满 5ms 硬限；心跳+状态同迭代对齐时叠成 10ms > ADS 环形 8.2ms → 跳帧。
     * NART=1 后失败帧 ~250µs 即完成（RQCP 置位、TXOK=0），阻塞归零；
     * 丢帧由协议层兜底（ISO-TP ACK 重试 3×200ms / 20Hz 心跳下一拍自愈）。
     * NART 位仅在初始化模式下可写：INRQ→等 INAK→写位→清 INRQ（带超时防挂死）。 */
    {
        uint32_t t0 = HAL_GetTick();
        hcan1.Instance->MCR |= CAN_MCR_INRQ;
        while (((hcan1.Instance->MSR & CAN_MSR_INAK) == 0u) &&
               ((HAL_GetTick() - t0) < 100u))
        {
        }
        SET_BIT(hcan1.Instance->MCR, CAN_MCR_NART);
        hcan1.Instance->MCR &= ~CAN_MCR_INRQ;
        t0 = HAL_GetTick();
        while (((hcan1.Instance->MSR & CAN_MSR_INAK) != 0u) &&
               ((HAL_GetTick() - t0) < 100u))
        {
        }
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

    /* 中断优先级调整（2026-08-21）：CAN RX0/SCE 降为优先级 2，
     * 低于 DRDY EXTI15_10（0）——无总线时 CAN 错误中断再频繁也不会饿死 ADC 链路。 */
    HAL_NVIC_SetPriority(CAN1_RX0_IRQn, 2u, 0u);
    HAL_NVIC_SetPriority(CAN1_SCE_IRQn, 2u, 0u);

    return APP_CAN_STATUS_OK;
}

AppCanStatus App_Can_Send(AppCanPort port, const AppCanFrame *frame, uint32_t timeout_ms)
{
    CAN_TxHeaderTypeDef txh = {0};
    uint32_t t0;
    uint32_t mailbox;

    if (port >= APP_CAN_PORT_COUNT || frame == NULL || timeout_ms == 0u)
    {
        return APP_CAN_STATUS_ERROR;
    }

    /* 无总线/总线关闭防护（2026-08-21 实测）：
     * 控制器 Bus-Off 时 TX 请求永不完成（RQCP 不来），主循环会被阻塞整个超时窗口，
     * 饿死 ADC 帧消费（实测 drop≈drdy/2）——Bus-Off 直接失败返回，等待时间硬限 5ms。 */
    if ((hcan1.Instance->ESR & CAN_ESR_BOFF) != 0u)
    {
        return APP_CAN_STATUS_ERROR;
    }
    if (timeout_ms > 5u)
    {
        timeout_ms = 5u;
    }

    txh.StdId = frame->can_id & 0x7FFu;
    txh.ExtId = 0u;
    txh.IDE   = CAN_ID_STD;
    txh.RTR   = CAN_RTR_DATA;
    txh.DLC   = (frame->dlc > 8u) ? 8u : frame->dlc;
    txh.TransmitGlobalTime = DISABLE;

    /* 等空闲邮箱 */
    t0 = HAL_GetTick();
    while (HAL_CAN_GetTxMailboxesFreeLevel(&hcan1) == 0u)
    {
        if ((HAL_GetTick() - t0) >= timeout_ms)
        {
            s_diag.tx_fail_count++;
            return APP_CAN_STATUS_TIMEOUT;
        }
    }

    /* 邮箱轮转，避免 HAL 自动选择时的重入问题 */
    mailbox = s_tx_mailbox_robin;
    s_tx_mailbox_robin = (s_tx_mailbox_robin + 1u) % 3u;

    /* 预清该邮箱完成标志（写1清零）：NART=1 下失败帧也置 RQCP，
     * 上次传输的残留标志会造成假完成/假成败（2026-09-05） */
    hcan1.Instance->TSR = (uint32_t)(CAN_TSR_RQCP0 | CAN_TSR_TXOK0 | CAN_TSR_TERR0) << mailbox;

    if (HAL_CAN_AddTxMessage(&hcan1, &txh, (uint8_t *)frame->data, &mailbox) != HAL_OK)
    {
        s_diag.tx_fail_count++;
        return APP_CAN_STATUS_ERROR;
    }

    /* 等该邮箱请求完成位（RQCP）；等待期间若进入 Bus-Off 立即放弃 */
    t0 = HAL_GetTick();
    while ((hcan1.Instance->TSR & (CAN_TSR_RQCP0 << mailbox)) == 0u)
    {
        if ((hcan1.Instance->ESR & CAN_ESR_BOFF) != 0u)
        {
            s_diag.tx_fail_count++;
            return APP_CAN_STATUS_ERROR;
        }
        if ((HAL_GetTick() - t0) >= timeout_ms)
        {
            s_diag.tx_fail_count++;
            return APP_CAN_STATUS_TIMEOUT;
        }
    }

    /* NART=1 下失败帧（ACK 错误/仲裁丢失）也会置 RQCP——以 TXOK 判真实成败 */
    if ((hcan1.Instance->TSR & (CAN_TSR_TXOK0 << mailbox)) == 0u)
    {
        hcan1.Instance->TSR = CAN_TSR_RQCP0 << mailbox;   /* 清完成标志 */
        s_diag.tx_fail_count++;
        return APP_CAN_STATUS_ERROR;
    }
    hcan1.Instance->TSR = CAN_TSR_RQCP0 << mailbox;
    s_diag.tx_ok_count++;
    return APP_CAN_STATUS_OK;
}

AppCanStatus App_Can_SendStd(AppCanPort port, uint16_t std_id, const uint8_t *data,
                             uint8_t len, uint32_t timeout_ms)
{
    AppCanFrame f;

    f.can_id = std_id;
    f.is_extended_id = 0u;
    f.dlc = len;
    if (data != NULL)
    {
        memcpy(f.data, data, (len > 8u) ? 8u : len);
    }
    else
    {
        memset(f.data, 0, sizeof(f.data));
    }
    return App_Can_Send(port, &f, timeout_ms);
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
    switch (lec)
    {
    case 0u: return "none";
    case 1u: return "stuff";
    case 2u: return "form";
    case 3u: return "ack";
    case 4u: return "bit-recessive";
    case 5u: return "bit-dominant";
    case 6u: return "crc";
    case 7u: return "software";
    default: return "?";
    }
}

/* ==================== weak 回调桥接（it.c 已备好 CAN1_RX0/SCE handler） ==================== */

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    CAN_RxHeaderTypeDef rxh;
    uint8_t data[8];
    AppCanFrame f;

    if (hcan->Instance != CAN1)
    {
        return;
    }
    s_diag.rx_irq_count++;

    /* FIFO0 排空式接收 */
    while (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &rxh, data) == HAL_OK)
    {
        f.can_id = rxh.StdId & 0x7FFu;
        f.is_extended_id = (rxh.IDE == CAN_ID_EXT) ? 1u : 0u;
        f.dlc = (rxh.DLC > 8u) ? 8u : rxh.DLC;
        memcpy(f.data, data, sizeof(f.data));
        s_diag.rx_frame_count++;

        if (s_rx_callback != NULL)
        {
            s_rx_callback(APP_CAN_PORT_1, &f);
        }
    }
}

void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan)
{
    uint32_t err;
    uint32_t esr;

    if (hcan->Instance != CAN1)
    {
        return;
    }
    err = HAL_CAN_GetError(hcan);
    esr = hcan->Instance->ESR;

    s_diag.last_error_code = (uint8_t)(err & 0x7u);
    s_diag.hal_error       = err;
    s_diag.error_warning   = ((esr & CAN_ESR_EWGF) != 0u) ? 1u : 0u;
    s_diag.error_passive   = ((esr & CAN_ESR_EPVF) != 0u) ? 1u : 0u;
    s_diag.bus_off         = ((esr & CAN_ESR_BOFF) != 0u) ? 1u : 0u;
    s_diag.tec             = (uint8_t)((esr >> 16) & 0xFFu);
    s_diag.rec             = (uint8_t)((esr >> 24) & 0xFFu);
    s_diag.esr             = esr;
    s_diag.tsr             = hcan->Instance->TSR;
    s_diag.rf0r            = hcan->Instance->RF0R;

    /* Bus-Off 自动恢复（9.2）：ISR 只置标志，恢复动作放主循环（BoardComm_Loop），
     * 避免在错误中断风暴中反复 Stop/Start 加重系统负担（2026-08-21） */
    if (s_diag.bus_off != 0u)
    {
        s_busoff_recover = 1u;
    }
}

/* 主循环调用：Bus-Off 恢复状态机（**非阻塞**，每圈只查标志）。
 * 2026-08-21 实测：HAL_CAN_Stop/Start 在死总线上阻塞 ~2ms（等 INIT/SLEEP 确认），
 * 3s 一个 Bus-Off 循环 → 每次恢复掉 6-8 帧 ADC。改为寄存器级三步状态机：
 *   置 INRQ → 等 INAK（进入初始化，错误计数清零）→ 清 INRQ → 等退出初始化 */
static volatile uint8_t s_recover_state;   /* 0=空闲 1=等INAK 2=等退出INIT */

void App_Can_RecoverBusOff(void)
{
    switch (s_recover_state)
    {
    case 0u:
        if (s_busoff_recover != 0u)
        {
            s_busoff_recover = 0u;
            hcan1.Instance->MCR |= CAN_MCR_INRQ;
            s_recover_state = 1u;
        }
        break;
    case 1u:
        if ((hcan1.Instance->MSR & CAN_MSR_INAK) != 0u)
        {
            hcan1.Instance->MCR &= ~CAN_MCR_INRQ;
            s_recover_state = 2u;
        }
        break;
    case 2u:
    default:
        if ((hcan1.Instance->MSR & CAN_MSR_INAK) == 0u)
        {
            s_recover_state = 0u;
        }
        break;
    }
}
