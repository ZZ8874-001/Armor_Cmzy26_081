/**
 ******************************************************************************
 * @file    board_comm.c
 * @brief   本板消息集实现（车内总线协议，设计文档 v1.5 第 9 章，Step 4 全实现）。
 *
 * - ISOTP 传输 + 调度器（ACK 重试）+ 分发器三实例接线；
 * - 上行：心跳 50ms（20Hz）、STATUS_REPORT（默认 1Hz，P24 可调）、MSG_HIT_EVENT；
 * - 下行：0x51 控制命令分派（LED/参数/ID/自检/零偏）+ ACK；
 * - 心跳超时跟踪（P14=200ms）→ 状态机 COMM_LOST 联动。
 * CAN ID（0x1A0/0x2A0）、src_id、新 TLV 码均为建议值（风险 R15，待协议方确认）。
 ******************************************************************************
 */
#include "comm/board_comm.h"

#include "board.h"
#include "comm/app_can.h"
#include "comm/app_log.h"
#include "comm/protocol/app_ack.h"
#include "comm/protocol/app_frame.h"
#include "comm/protocol/tlv.h"
#include "comm/service/dispatcher.h"
#include "comm/service/retry_ack_scheduler.h"
#include "comm/transport/isotp.h"
#include "app/led_status.h"
#include "app/state_machine.h"
#include "detect/calibration.h"
#include "detect/hit_detect.h"

#include <string.h>

/* ==================== 实例 ==================== */
static TransportIsotpContext  g_isotp;
static ServiceRetryAckScheduler g_sched;
static ServiceDispatcher      g_disp;

/* ==================== 运行状态（调试器可直接 watch） ==================== */
static volatile uint32_t s_hb_last_ms;      /* 最近收到核心板心跳的时刻 */
static volatile uint8_t  s_hb_lost;         /* 心跳丢失标志 */
static volatile uint32_t s_sent_count;      /* 上行消息计数 */
static volatile uint32_t s_rx_count;        /* 下行消息计数 */
static uint32_t s_last_hb_tx_ms;            /* 上次心跳发送时刻 */
static uint32_t s_last_status_tx_ms;        /* 上次状态上报时刻 */
static hit_param_t s_param;                 /* 参数副本（P14/P24） */

static uint16_t s_module_uid;               /* MCU UID 低 2 字节 */
static uint32_t s_hb_pause_until;           /* 连续发送失败后的心跳退避截止时刻 */
static uint32_t s_send_fail_streak;         /* 连续发送失败计数 */

/* ---- BoardComm_Loop 阻塞诊断（DWT 周期计数，1 cycle = 12.5ns @80MHz） ---- */
static volatile uint32_t s_bcloop_us;       /* 最近一次 BoardComm_Loop 耗时（µs） */
static volatile uint32_t s_bcloop_max_us;   /* 1s 窗口最大耗时 */
static uint32_t s_bcloop_max_cyc;
static uint32_t s_bcloop_pub_ms;

/* RX 桥接：AppCanRxCallback(port, frame) → ISOTP(ctx, port, frame) */
static void Bc_CanRxBridge(AppCanPort port, const AppCanFrame *frame);

/* ==================== 内部：发送与回调 ==================== */

/* 调度器 send_fn：payload 为已编码的应用帧字节流 */
static bool Bc_SchedSend(void *user_arg, const uint8_t *payload, uint16_t length)
{
    (void)user_arg;
    if (Transport_Isotp_Send(&g_isotp, payload, length) == TRANSPORT_ISOTP_OK)
    {
        s_sent_count++;
        return true;
    }
    return false;
}

/* 调度器 idle_fn：传输空闲时可发下一条（栈内部处理，此处恒 true） */
static bool Bc_SchedIdle(void *user_arg)
{
    (void)user_arg;
    return true;
}

/* ISOTP 发送完成 → 调度器 */
static void Bc_OnTxComplete(void *user_arg, TransportIsotpStatus status)
{
    (void)user_arg;
    Service_RetryAckScheduler_OnTransmitComplete(&g_sched, status);
}

/* ISOTP 收到完整消息 → 解码分发 + ACK 匹配 + 心跳跟踪 */
static void Bc_OnMessage(void *user_arg, const uint8_t *payload, uint16_t length)
{
    ProtocolAppFrame frame;

    (void)user_arg;
    if (!Protocol_AppFrame_Decode(&frame, payload, length))
    {
        return;
    }
    s_rx_count++;

    /* 核心板心跳（0x01）：更新接收时刻，恢复 COMM_LOST */
    if (frame.func_code == FUNC_HEARTBEAT && frame.src_id == BOARD_DST_ID)
    {
        s_hb_last_ms = HAL_GetTick();
        if (s_hb_lost != 0u)
        {
            s_hb_lost = 0u;
            StateMachine_OnCommLost(false);
        }
        return;
    }

    /* ACK 帧 → 调度器匹配 */
    if (frame.func_code == FUNC_ACK)
    {
        Service_RetryAckScheduler_OnReceivedFrame(&g_sched, &frame);
        return;
    }

    /* 其余 → 分发器 */
    (void)Service_Dispatcher_DispatchFrame(&g_disp, APP_CAN_PORT_1, &frame);
}

/* ISOTP 帧日志（可选调试） */
static void Bc_OnFrame(void *user_arg, AppCanPort port, const char *direction, const AppCanFrame *frame)
{
    (void)user_arg; (void)port; (void)direction; (void)frame;
    /* 调试期可打印：App_Log_Printf("[ISOTP] %s id=0x%03lX dlc=%u\r\n", ...) */
}

/* ==================== 下行命令处理 ==================== */

static bool Bc_HandleCtrlCmd(void *user_arg, AppCanPort port, const ProtocolAppFrame *frame)
{
    ProtocolTlvReader reader;
    ProtocolTlvView view;
    uint8_t cmd_index = 0u;
    bool have_cmd = false;
    uint8_t r = 0u, g = 0u, b = 0u, brightness = 100u;
    uint8_t effect = 0u;
    bool have_effect = false, have_value = false, have_brightness = false;
    uint8_t armor_id = 0u;
    bool have_armor = false;
    uint16_t param_id = 0u;
    uint32_t param_val = 0u;
    bool have_param_id = false, have_param_val = false;

    (void)user_arg; (void)port;

    Protocol_TlvReader_Init(&reader, frame->data, frame->data_len);
    while (Protocol_TlvReader_Next(&reader, &view))
    {
        switch (view.type)
        {
        case PROTOCOL_TLV_CMD_INDEX:              /* 0x29 */
            Protocol_Tlv_ReadU8(&view, &cmd_index);
            have_cmd = true;
            break;
        case TLV_EFFECT:                          /* 0x38 本板新增码 */
            Protocol_Tlv_ReadU8(&view, &effect);
            have_effect = true;
            break;
        case PROTOCOL_TLV_VALUE:                  /* 0x05：LED 自定义色 0xRRGGBB */
        {
            uint32_t v = 0u;
            Protocol_Tlv_ReadU32(&view, &v);
            r = (uint8_t)(v >> 16);
            g = (uint8_t)(v >> 8);
            b = (uint8_t)(v);
            have_value = true;
            break;
        }
        case TLV_BRIGHTNESS:                      /* 0x37 */
            Protocol_Tlv_ReadU8(&view, &brightness);
            have_brightness = true;
            break;
        case TLV_ARMOR_ID:                        /* 0x3B */
            Protocol_Tlv_ReadU8(&view, &armor_id);
            have_armor = true;
            break;
        case TLV_PARAM_ID:                        /* 0x39 */
            Protocol_Tlv_ReadU16(&view, &param_id);
            have_param_id = true;
            break;
        case TLV_PARAM_VALUE:                     /* 0x3A */
            Protocol_Tlv_ReadU32(&view, &param_val);
            have_param_val = true;
            break;
        default:
            break;
        }
    }

    if (!have_cmd)
    {
        return false;
    }

    switch (cmd_index)
    {
    case CMD_LED_CTRL:
        if (have_effect)
        {
            if (effect < LED_EFF_COUNT)
            {
                LedStatus_SetEffect((led_effect_t)effect);
            }
        }
        if (have_value)
        {
            (void)r; (void)g; (void)b;   /* 自定义颜色 TODO(Step 4+)：led_status 增加自定义色接口 */
        }
        if (have_brightness)
        {
            LedStatus_SetBrightness(brightness);
        }
        break;

    case CMD_PARAM_SET:
        if (have_param_id && have_param_val)
        {
            /* TODO(Step 4+)：按 param_id 写参数表并回读校验（Flash 持久化 Step 5） */
            (void)param_id; (void)param_val;
        }
        break;

    case CMD_PARAM_GET:
        if (have_param_id)
        {
            /* TODO(Step 4+)：回帧 TLV_PARAM_VALUE */
            (void)param_id;
        }
        break;

    case CMD_ID_SET:
        if (have_armor)
        {
            LedStatus_SetTeamColor(armor_id & 0x01u);
            StateMachine_OnIdSet(false);          /* 慢闪 1s 后回 NORMAL */
            /* TODO(Step 5)：Flash 持久化 armor_id */
        }
        break;

    case CMD_SELF_TEST:
        /* TODO(Step 5)：触发完整自检流程并上报 */
        BoardComm_ReportStatus(HitDetect_GetFaultFlags());
        break;

    case CMD_ZERO_CAL:
        /* TODO(Step 5)：当前基线固化 Flash */
        break;

    default:
        return false;
    }

    /* 下行命令 seq≠0 → 回 ACK（经调度器重试） */
    if (Protocol_AppFrame_RequiresAck(frame))
    {
        ProtocolAppFrame ack;
        if (Protocol_AppAck_Build(frame, 0u, false, 0u, &ack))
        {
            (void)Service_RetryAckScheduler_Enqueue(&g_sched, &ack,
                                                    PROTOCOL_PRIORITY_HIGH, false, 3u, 200u, NULL);
        }
    }
    return true;
}

/* ==================== 上行消息 ==================== */

/* 心跳（0x01，U32 ms 时间戳，seq=0）；返回是否成功启动发送 */
static bool Bc_SendHeartbeat(void)
{
    ProtocolAppFrame f;
    uint8_t buf[PROTOCOL_APP_FRAME_MAX_ENCODED_SIZE];
    uint16_t len;

    Protocol_AppFrame_Init(&f, BOARD_DST_ID, BOARD_SRC_ID, s_module_uid,
                           FUNC_HEARTBEAT, 0u, PROTOCOL_TYPE_U32);
    Protocol_AppFrame_SetU32(&f, HAL_GetTick());
    if (!Protocol_AppFrame_Encode(&f, buf, &len, sizeof(buf)))
    {
        return false;
    }
    if (Transport_Isotp_Send(&g_isotp, buf, len) != TRANSPORT_ISOTP_OK)
    {
        return false;
    }
    s_sent_count++;
    return true;
}

/* 状态上报（0x02，TLV_STATE 字符串 + 故障位图，seq=0） */
void BoardComm_ReportStatus(uint16_t fault_flags)
{
    ProtocolAppFrame f;
    ProtocolTlvWriter w;
    uint8_t buf[PROTOCOL_APP_FRAME_MAX_ENCODED_SIZE];
    uint16_t len;
    const char *state = "normal";

    switch (StateMachine_Get())
    {
    case SM_STATE_FAULT:       state = "fault";      break;
    case SM_STATE_COMM_LOST:   state = "comm_lost";  break;
    case SM_STATE_HIT:         state = "hit";        break;
    case SM_STATE_ID_SETUP:    state = "id_setup";   break;
    case SM_STATE_ID_CONFLICT: state = "id_conflict"; break;
    default:                   state = "normal";     break;
    }

    Protocol_TlvWriter_Init(&w, buf, sizeof(buf));
    Protocol_TlvWriter_AppendString(&w, PROTOCOL_TLV_STATE, state);
    Protocol_TlvWriter_AppendU16(&w, TLV_FAULT_FLAGS, fault_flags);

    Protocol_AppFrame_Init(&f, BOARD_DST_ID, BOARD_SRC_ID, s_module_uid,
                           FUNC_STATUS_REPORT, 0u, PROTOCOL_TYPE_TLV_STREAM);
    Protocol_AppFrame_SetData(&f, buf, Protocol_TlvWriter_GetLength(&w));

    if (Protocol_AppFrame_Encode(&f, buf, &len, sizeof(buf)))
    {
        if (Transport_Isotp_Send(&g_isotp, buf, len) == TRANSPORT_ISOTP_OK)
        {
            s_sent_count++;
        }
    }
}

/* 击打事件（0x31，TLV 流，seq=0 禁止重试） */
void BoardComm_ReportHitEvent(const hit_event_t *e)
{
    ProtocolAppFrame f;
    ProtocolTlvWriter w;
    uint8_t buf[PROTOCOL_APP_FRAME_MAX_ENCODED_SIZE];
    uint16_t len;

    if (e == NULL)
    {
        return;
    }
    /* COMM_LOST/FAULT 态不发送只计数（设计文档 11 章语义）；
     * 同时避免无总线时击打路径上的 CAN 发送阻塞（2026-08-21） */
    if (s_hb_lost != 0u || StateMachine_Get() == SM_STATE_FAULT)
    {
        return;
    }

    Protocol_TlvWriter_Init(&w, buf, sizeof(buf));
    Protocol_TlvWriter_AppendU8(&w,  TLV_HIT_CHANNEL,   e->ch);
    Protocol_TlvWriter_AppendU8(&w,  TLV_HIT_INTENSITY, e->intensity);
    Protocol_TlvWriter_AppendU16(&w, TLV_HIT_FORCE,     e->force_01n);
    Protocol_TlvWriter_AppendU32(&w, TLV_HIT_TS,        e->ts_ms);
    Protocol_TlvWriter_AppendU32(&w, TLV_HIT_PEAK,      e->peak);

    Protocol_AppFrame_Init(&f, BOARD_DST_ID, BOARD_SRC_ID, s_module_uid,
                           FUNC_HIT_EVENT, 0u, PROTOCOL_TYPE_TLV_STREAM);
    Protocol_AppFrame_SetData(&f, buf, Protocol_TlvWriter_GetLength(&w));

    if (Protocol_AppFrame_Encode(&f, buf, &len, sizeof(buf)))
    {
        if (Transport_Isotp_Send(&g_isotp, buf, len) == TRANSPORT_ISOTP_OK)
        {
            s_sent_count++;
        }
    }
}

/* ==================== 初始化与主循环 ==================== */

void BoardComm_Init(void)
{
    TransportIsotpConfig cfg;

    Cal_GetDefaults(&s_param);
    s_module_uid = (uint16_t)(HAL_GetUIDw0() & 0xFFFFu);

    memset(&cfg, 0, sizeof(cfg));
    cfg.port                    = APP_CAN_PORT_1;
    cfg.tx_id                   = BOARD_CAN_TX_ID;
    cfg.rx_id                   = BOARD_CAN_RX_ID;
    cfg.block_size              = 0u;
    cfg.st_min_ms               = 10u;
    cfg.tx_timeout_ms           = 100u;
    cfg.tx_require_flow_control = true;
    cfg.get_ms                  = HAL_GetTick;
    cfg.on_message              = Bc_OnMessage;
    cfg.on_frame                = Bc_OnFrame;
    cfg.on_tx_complete          = Bc_OnTxComplete;
    cfg.user_arg                = NULL;
    Transport_Isotp_Init(&g_isotp, &cfg);

    Service_RetryAckScheduler_Init(&g_sched, Bc_SchedSend, Bc_SchedIdle, HAL_GetTick, NULL);
    Service_Dispatcher_Init(&g_disp);
    Service_Dispatcher_Register(&g_disp, FUNC_CTRL_CMD, Bc_HandleCtrlCmd, NULL);

    App_Can_SetRxCallback(Bc_CanRxBridge);

    s_hb_last_ms = HAL_GetTick();
    s_hb_lost = 0u;
    s_last_hb_tx_ms = 0u;
    /* 状态上报网格错开心跳网格 25ms：避免两者同迭代对齐时两次 5ms 发送阻塞
     * 叠加成 10ms（超过 ADS 环形 8.2ms → 跳帧），错开后单次迭代最长阻塞 5ms < 环容量。 */
    s_last_status_tx_ms = 25u;
}

void BoardComm_Loop(void)
{
    uint32_t now = HAL_GetTick();
    uint32_t cyc0 = DWT->CYCCNT;

    Transport_Isotp_Poll(&g_isotp);
    Service_RetryAckScheduler_Poll(&g_sched);

    /* Bus-Off 恢复（主循环上下文执行） */
    App_Can_RecoverBusOff();

    /* 心跳发送节拍（50ms = 20Hz）；连续失败退避：无总线时避免持续制造 CAN 错误 */
    if ((now - s_last_hb_tx_ms) >= s_param.heartbeat_ms)
    {
        s_last_hb_tx_ms = now;
        if ((int32_t)(now - s_hb_pause_until) >= 0)
        {
            if (Bc_SendHeartbeat())
            {
                s_send_fail_streak = 0u;
            }
            else
            {
                s_send_fail_streak++;
                if (s_send_fail_streak >= 3u)
                {
                    s_hb_pause_until = now + 500u;   /* 连续 3 次失败 → 退避 500ms */
                    s_send_fail_streak = 0u;
                }
            }
        }
    }

    /* 状态上报节拍（P24，默认 1Hz） */
    if ((now - s_last_status_tx_ms) >= s_param.status_period_ms)
    {
        s_last_status_tx_ms = now;
        BoardComm_ReportStatus(HitDetect_GetFaultFlags());
    }

    /* 心跳超时跟踪（P14=200ms）→ COMM_LOST */
    if ((s_hb_lost == 0u) && ((now - s_hb_last_ms) >= s_param.comm_timeout_ms))
    {
        s_hb_lost = 1u;
        StateMachine_OnCommLost(true);
    }

    /* 耗时统计：1s 窗口发布一次最大值 */
    {
        uint32_t dt = DWT->CYCCNT - cyc0;
        s_bcloop_us = dt / 80u;
        if (dt > s_bcloop_max_cyc)
        {
            s_bcloop_max_cyc = dt;
        }
        if ((int32_t)(now - s_bcloop_pub_ms) >= 1000)
        {
            s_bcloop_pub_ms = now;
            s_bcloop_max_us = s_bcloop_max_cyc / 80u;
            s_bcloop_max_cyc = 0u;
        }
    }
}

/* RX 桥接：AppCanFrame → ISOTP（协议栈签名含 port） */
static void Bc_CanRxBridge(AppCanPort port, const AppCanFrame *frame)
{
    Transport_Isotp_OnCanFrame(&g_isotp, port, frame);
}
