/**
 ******************************************************************************
 * @file    state_machine.c
 * @brief   模块状态机实现（TIM2 1kHz 中断驱动，设计文档 v1.5 第 11 章）。
 *
 * Step 3 基础版：BOOT / NORMAL / HIT / FAULT 四态已实现；
 * COMM_LOST / ID_SETUP / ID_CONFLICT 待 Step 4（通信）接入后落地。
 * 转移条件：
 *   BOOT   300ms 自检白灯 → NORMAL
 *   NORMAL 击打事件（StateMachine_OnHitEvent）→ HIT（快闪 300ms 后回 NORMAL）
 *   任一态 HitDetect 故障位非零 → FAULT；故障清除 → NORMAL
 ******************************************************************************
 */
#include "app/state_machine.h"
#include "app/led_status.h"
#include "detect/hit_detect.h"

#define SM_BOOT_MS   300u    /* BOOT 白灯时长 */
#define SM_HIT_MS    300u    /* HIT 快闪时长（5Hz × 3 个周期 = 300ms） */

static volatile sm_state_t s_state = SM_STATE_BOOT;
static volatile uint32_t   s_state_ms;   /* 当前状态已驻留 ms 数（1kHz 累计） */

void StateMachine_Init(void)
{
    s_state = SM_STATE_BOOT;
    s_state_ms = 0u;
    LedStatus_SetEffect(LED_EFF_BOOT);
}

void StateMachine_Tick(void)
{
    s_state_ms++;

    switch (s_state)
    {
    case SM_STATE_BOOT:
        if (s_state_ms >= SM_BOOT_MS)
        {
            s_state = SM_STATE_NORMAL;
            s_state_ms = 0u;
            LedStatus_SetEffect(LED_EFF_NORMAL);
        }
        break;

    case SM_STATE_NORMAL:
        if (HitDetect_GetFaultFlags() != 0u)
        {
            s_state = SM_STATE_FAULT;
            s_state_ms = 0u;
            LedStatus_SetEffect(LED_EFF_FAULT);
        }
        break;

    case SM_STATE_HIT:
        if (s_state_ms >= SM_HIT_MS)
        {
            s_state = SM_STATE_NORMAL;
            s_state_ms = 0u;
            LedStatus_SetEffect(LED_EFF_NORMAL);
        }
        break;

    case SM_STATE_FAULT:
        if (HitDetect_GetFaultFlags() == 0u)
        {
            s_state = SM_STATE_NORMAL;
            s_state_ms = 0u;
            LedStatus_SetEffect(LED_EFF_NORMAL);
        }
        break;

    /* TODO(Step 4)：COMM_LOST（心跳超时）/ ID_SETUP / ID_CONFLICT（下行命令） */
    case SM_STATE_COMM_LOST:
    case SM_STATE_ID_SETUP:
    case SM_STATE_ID_CONFLICT:
    default:
        break;
    }
}

void StateMachine_OnHitEvent(const hit_event_t *e)
{
    (void)e;
    /* NORMAL 或 HIT 中再受击 → 刷新快闪计时（HIT 中重击不重新开始也可，取刷新） */
    if (s_state == SM_STATE_NORMAL || s_state == SM_STATE_HIT)
    {
        s_state = SM_STATE_HIT;
        s_state_ms = 0u;
        LedStatus_SetEffect(LED_EFF_HIT);
    }
    /* FAULT/COMM_LOST 态不发送只计数（11 章语义）——事件仍由 main_app 日志输出 */
}

sm_state_t StateMachine_Get(void)
{
    return (sm_state_t)s_state;
}
