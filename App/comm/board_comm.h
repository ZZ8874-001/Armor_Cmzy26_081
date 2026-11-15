/**
 ******************************************************************************
 * @file    board_comm.h
 * @brief   装甲板固定帧 CAN 协议接口。
 *
 * 所有报文均为 500kbps、标准 11 位 CAN 数据帧；完整定义见
 * Docs/CAN_PROTOCOL.md。协议不使用 ISO-TP、TLV 或应用层封包。
 ******************************************************************************
 */
#ifndef APP_BOARD_COMM_H
#define APP_BOARD_COMM_H

#include <stdint.h>
#include <stdbool.h>
#include "detect/hit_detect.h"

/* NodeID 分配完成后，每块板使用自己的 CAN ID 槽位。 */
#define BOARD_CAN_CMD_LED_NORMAL     0x00u
#define BOARD_CAN_CMD_TEAM_RED       0x01u
#define BOARD_CAN_CMD_TEAM_BLUE      0x02u
#define BOARD_CAN_CMD_BRIGHTNESS     0x03u
#define BOARD_CAN_CMD_EFFECT         0x04u

void BoardComm_Init(void);
void BoardComm_Loop(void);
void BoardComm_ReportHitEvent(const hit_event_t *e);
void BoardComm_ReportStatus(uint16_t fault_flags);

/* 本次上电是否收到过**上位机（电管/裁判系统）方向**的报文（锁存，收到即置位）。
 * 只认上位机→装甲板方向：枚举 0x120/0x122 + 本板槽位的查询/LED 控制/ID 查询/击打 ACK；
 * 兄弟板与本板上行的 0x121/0x123/0x110~0x112/击打事件等**不算**。
 * 用途：开机校准期间一听到上位机就判定"整车已上线"，停止校准直接用 Flash 参考
 * （见 state_machine.c）。ISR 置位、1kHz 状态机读取，单字节读写原子。 */
bool BoardComm_MasterHeard(void);

#endif /* APP_BOARD_COMM_H */
