/**
 ******************************************************************************
 * @file    state_machine.c
 * @brief   状态机骨架（桩实现，状态枚举已按设计文档 v1.5 第 11 章定型）。
 *
 * TODO(Step 2/Step 5)：实现完整转移——
 *   BOOT →(自检通过) NORMAL；NORMAL →(有效击打) HIT →(50ms) NORMAL；
 *   故障位闩存 → FAULT（每 5s 自动重试自检）；200ms 无心跳 → COMM_LOST；
 *   0x51 ID_SET → ID_SETUP；主控 ACK 拒绝 → ID_CONFLICT。
 * 骨架阶段：Init 后直接进入 NORMAL。
 ******************************************************************************
 */
#include "app/state_machine.h"

static sm_state_t s_state = SM_STATE_BOOT;

void StateMachine_Init(void)
{
    /* TODO(Step 5)：自检流程（7.6）通过后才进 NORMAL；骨架阶段直接进入。 */
    s_state = SM_STATE_NORMAL;
}

void StateMachine_Tick(void)
{
    /* TODO(Step 2/Step 5)：1kHz 步进的状态转移判定（见文件头注释）。 */
}

sm_state_t StateMachine_Get(void)
{
    return s_state;
}
