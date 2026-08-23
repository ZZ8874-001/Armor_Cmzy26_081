/**
 ******************************************************************************
 * @file    board_comm.c
 * @brief   本板消息集骨架（桩实现，消息定义已按设计文档 v1.5 第 9.4 节固化）。
 *
 * TODO(Step 4)：实现——
 *   1) Transport_Isotp_Init（tx_id=0x1A0/rx_id=0x2A0）+ ServiceRetryAckScheduler 实例化；
 *   2) 上行：心跳 20Hz（50ms）、STATUS_REPORT（默认 1Hz，P24 可调）、
 *      MSG_HIT_EVENT（seq=0 禁止重试）；
 *   3) 下行：0x51 命令处理（LED 控制/参数配置读取/ID 设置/自检/零偏标定，ACK 3 次×200ms）；
 *   4) 掉线判定：200ms（4×心跳）无核心板心跳 → COMM_LOST。
 ******************************************************************************
 */
#include "comm/board_comm.h"

void BoardComm_Init(void)
{
    /* TODO(Step 4)：协议栈实例化与消息处理注册。 */
}

void BoardComm_Loop(void)
{
    /* TODO(Step 4)：心跳/状态上报节拍检查与入队（协议入队仅主循环上下文，10.3）。 */
}

void BoardComm_ReportHitEvent(const hit_event_t *e)
{
    (void)e;
    /* TODO(Step 4)：构造 MSG_HIT_EVENT（TLV：通道/强度/力度/时间戳/峰值），seq=0 发送。 */
}

void BoardComm_ReportStatus(uint16_t fault_flags)
{
    (void)fault_flags;
    /* TODO(Step 4)：构造 MSG_STATUS_REPORT（TLV_STATE + TLV_VALUE 故障位图 + TLV_TEXT）。 */
}
