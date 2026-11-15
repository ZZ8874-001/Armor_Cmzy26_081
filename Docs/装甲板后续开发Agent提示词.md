# 装甲板后续开发提示词

你负责继续开发本仓库（装甲板固件仓库根目录）的装甲板 CAN 调试能力，并在需要时同步修改电管主控工程 L431PM 仓库。先阅读 `Docs/CAN_PROTOCOL.md`（v1.5）、`App/comm/can_node.c`、`App/comm/board_comm.c` 以及电管工程的 `Application/Referee/app_armor_enum.c`。

现有约束：四块装甲板使用同一个 bin；电管根据 STM32 UID 自动分配运行时 NodeID 1~4。枚举帧是 `0x120~0x123`，业务 ID 是 `0x130 + (NodeID - 1) * 0x10 + offset`。未 READY 的装甲板不得发送或处理业务帧。CAN RX ISR 只能入队或置标志，解析、发送、串口打印必须在普通任务中完成；不使用动态内存；保持 500 kbps 标准 CAN 帧。

原先计划的三项调试命令均已实现（协议定义见 `Docs/CAN_PROTOCOL.md` v1.5「NodeID 身份确认」与「联调检查」）。本文件因此转为**验收/回归清单**：每次改动通信层后逐条复核，改动前先确认现状，不要重复实现。

1. 电管串口 `ARMOR=LIST`。一条命令输出四项枚举表：NodeID、完整 UID、token、ACK 状态、在线状态、最后接收时间、当前装甲板状态和故障位。验收点：该命令只读取 `armor_enum_diag` 与 `referee_can_monitor.armor_nodes`，不重新枚举。
2. 定向身份确认命令 `ARMOR=ID?=<1..4>`。电管向该 NodeID 的 CAN 槽位发送身份查询；目标装甲板返回自身 NodeID、UID token 和 UID CRC16。验收点：电管串口输出请求的 NodeID、回复的 NodeID、token、CRC 和 `OK`/`MISMATCH`/`TIMEOUT`；重复命令、非法 NodeID、枚举未完成、CAN 发送失败都给出明确结果。
3. 业务槽位 offset `0x8=NODE_ID_QUERY`、`0x9=NODE_ID_REPLY`。验收点：请求只被目标 NodeID 处理，回复使用同一 NodeID 的 reply ID；`CanNode_BusinessId()` 的 offset 上界为 `0xA`，两端 ID 表一致。
4. 调试命令与 25 ms 的常规状态轮询不能互相阻塞；命令等待回复使用明确超时。
5. 不改变 `0x120~0x123` 的枚举线格式，不改变 UID 排序分配规则，不把 NodeID 写入 Flash，不删除现有击打、故障、LED 或状态查询功能。

**回归验收**：四块完全相同固件上电后均获得 1~4；`ARMOR=LIST` 一次展示全部分配；依次发送 `ARMOR=ID?=1` 到 `ARMOR=ID?=4` 都得到对应 NodeID 的身份回复；无效编号和未枚举状态可诊断；装甲板与电管工程均用现有 Makefile 编译通过。不要烧录硬件，除非用户明确要求。
