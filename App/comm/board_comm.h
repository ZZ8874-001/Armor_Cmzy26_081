/**
 ******************************************************************************
 * @file    board_comm.h
 * @brief   本板消息集定义与收发处理接口（车内总线协议）。
 *
 * 设计文档 v1.5 第 9.3/9.4 节为唯一权威来源。CAN 层 ID、src_id 与新 TLV
 * 类型码均为建议值，需与协议维护者确认后固化（风险 R15）。
 ******************************************************************************
 */
#ifndef APP_BOARD_COMM_H
#define APP_BOARD_COMM_H

#include <stdint.h>
#include "detect/hit_detect.h"

/* ---- CAN 层（9.3，建议值待确认） ---- */
#define BOARD_CAN_TX_ID     0x1A0u
#define BOARD_CAN_RX_ID     0x2A0u

/* ---- 应用层地址（9.3） ---- */
#define BOARD_SRC_ID        0x0Au   /* 装甲板（建议值） */
#define BOARD_DST_ID        0x01u   /* 核心板 */

/* ---- 功能码（isotp 栈 通信协议v2） ---- */
#define FUNC_HEARTBEAT      0x01u
#define FUNC_STATUS_REPORT  0x02u
#define FUNC_ACK            0x06u
#define FUNC_HIT_EVENT      0x31u
#define FUNC_CTRL_CMD       0x51u

/* ---- 下行命令（0x51 TLV_CMD_INDEX，9.4） ---- */
#define CMD_LED_CTRL        0x01u
#define CMD_PARAM_SET       0x02u
#define CMD_PARAM_GET       0x03u
#define CMD_ID_SET          0x04u
#define CMD_SELF_TEST       0x05u
#define CMD_ZERO_CAL        0x06u

/* ---- 本板新增 TLV 类型码（0x30~0x3B，占用协议预留区，需同步协议文档） ---- */
#define TLV_HIT_CHANNEL     0x30u
#define TLV_HIT_INTENSITY   0x31u
#define TLV_HIT_FORCE       0x32u
#define TLV_HIT_TS          0x33u
#define TLV_HIT_PEAK        0x34u
#define TLV_FAULT_FLAGS     0x35u
#define TLV_TEMP            0x36u
#define TLV_BRIGHTNESS      0x37u
#define TLV_EFFECT          0x38u
#define TLV_PARAM_ID        0x39u
#define TLV_PARAM_VALUE     0x3Au
#define TLV_ARMOR_ID        0x3Bu

/* 初始化：注册协议栈实例与消息处理（主循环上下文调用）。 */
void BoardComm_Init(void);

/* 主循环调用：心跳/状态上报节拍检查与入队（协议入队仅主循环上下文）。 */
void BoardComm_Loop(void);

/* 击打事件上报（seq=0，事件唯一性禁止重试，9.4）。 */
void BoardComm_ReportHitEvent(const hit_event_t *e);

/* 状态上报（状态变化 + 周期，默认 1Hz 可调 P24）。 */
void BoardComm_ReportStatus(uint16_t fault_flags);

#endif /* APP_BOARD_COMM_H */
