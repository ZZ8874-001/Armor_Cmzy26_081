/**
 ******************************************************************************
 * @file    state_machine.h
 * @brief   模块状态机接口（TIM2 1kHz 中断驱动，设计文档 v1.5 第 11 章）。
 *
 * 八状态：BOOT / NORMAL / HIT / FAULT / COMM_LOST / ID_SETUP / ID_CONFLICT /
 * REF_PENDING（上电校准：基线 EMA 热身 5τ 紫 1Hz 慢闪，随后 20s 击打探测窗紫 5Hz
 *               快闪，等首次击打记录参考样本；无可用参考时不超时退出）。
 * 转移条件见设计文档 11 章状态表；灯光联动见 led_status.h。
 ******************************************************************************
 */
#ifndef APP_STATE_MACHINE_H
#define APP_STATE_MACHINE_H

#include <stdint.h>
#include "detect/hit_detect.h"   /* hit_event_t */

typedef enum
{
    SM_STATE_BOOT = 0,
    SM_STATE_NORMAL,
    SM_STATE_HIT,
    SM_STATE_FAULT,
    SM_STATE_COMM_LOST,
    SM_STATE_ID_SETUP,
    SM_STATE_ID_CONFLICT,
    SM_STATE_REF_PENDING,   /* 紫闪校准：热身 5τ 慢闪 + 20s 探测窗快闪（故障之后、离线之前） */
    SM_STATE_COUNT
} sm_state_t;

/* 初始化（进入 BOOT；骨架阶段自检占位后直接 NORMAL）。 */
void StateMachine_Init(void);

/* 1kHz 步进（TIM2 中断调用）：状态转移判定与灯模式切换。 */
void StateMachine_Tick(void);

/* 击打事件输入（main_app 在检测到事件时调用；FAULT/COMM_LOST 态忽略只计数）。 */
void StateMachine_OnHitEvent(const hit_event_t *e);

/* 心跳丢失/恢复（board_comm 心跳超时跟踪调用，Step 4）。 */
void StateMachine_OnCommLost(bool lost);

/* ID 设置/冲突（下行命令调用，Step 4；1s 后自动回 NORMAL）。 */
void StateMachine_OnIdSet(bool conflict);

/* 当前状态。 */
sm_state_t StateMachine_Get(void);

#endif /* APP_STATE_MACHINE_H */
