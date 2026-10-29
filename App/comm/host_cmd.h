/**
 ******************************************************************************
 * @file    host_cmd.h
 * @brief   上位机 USART1 下行命令通道（HCMD 帧）。
 *
 * 帧格式与命令表权威：Docs/USART1波形数据帧格式.md（v1.1 HCMD 节）。
 * 并发规则：RX 走 DMA（DMA1_Ch5 NORMAL，零接收中断），字节由 DMA 后台写入
 * 缓冲、主循环读计数器消费；解析/应用/ACK 全部在主循环（HostCmd_Loop，
 * App_Loop 内 wave-TX 块之后）。Flash 写只允许主循环。
 ******************************************************************************
 */
#ifndef APP_HOST_CMD_H
#define APP_HOST_CMD_H

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化：复位解析器/计数器，武装 USART1 RX DMA（.ioc 已配 DMA1_Ch5）。
 * 必须在 App_Log_Init 之后（启动日志 [CMD] 行）。 */
void HostCmd_Init(void);

/* 主循环每圈调用：DMA 计数器排空 → 帧校验 → 派发（每圈 ≤2 命令）；
 * IDLE 轮询清理残帧 + 重启 DMA；DMA 失联看门狗。 */
void HostCmd_Loop(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_HOST_CMD_H */
