# App/ 工程结构说明（装甲击打检测模块固件）

> 本文档说明**代码放在哪、按什么规矩写**；"为什么这么做/算法与协议细节"见 **`Docs/装甲模块固件设计方案.md`**（当前 v1.5，以下称《设计方案》，章节号均指该文档）。

---

## 1. 总览

- `App/` 是应用层唯一目录：**不参与 CubeMX 重生成**（重生成只覆盖 `Core/` 与 Makefile），修改 CubeMX 配置不会丢应用代码。
- 分层：`bsp/`（硬件抽象）→ `detect/`、`comm/`（功能与协议）→ `app/`（编排与状态）。**禁止反向依赖**。
- 全部文件经 `app.mk` 加入构建（见 §8）。
- 当前状态：**应用层已实现并在硬件上跑通**——ADC 驱动、灯效、检测管线、统一故障注册表与非阻塞上电自检均已落地。唯一缺口是参数持久化：`Cal_Load`/`Cal_Save` 仍是 `TODO(Step 5)`，每次上电回落 `Cal_GetDefaults()`。

## 2. 完整目录树

```
App/
├─ board.h                      板级公共定义（外设句柄 extern 唯一入口、常量、极性宏）←《设计方案》2/6.7
├─ bsp/
│  ├─ ads131m04.h/.c            ADS131M04 驱动 ← 第 6 章
│  ├─ ws2812_uart.h/.c          WS2812 灯带驱动（USART2+DMA）← 8.1
│  ├─ temp_mon.h/.c             基准温度监测（ADC1 TIM1 触发）← 7.5
│  └─ ref_store.h/.c            击打参考样本 Flash 持久化（**末页 0x0801F800 专用**）← 7.1
├─ detect/
│  ├─ hit_detect.h/.c           击打检测管线（两段式：触发 + 相关度确认）← 第 7 章
│  └─ calibration.h/.c          参数表 P01~P27 + 参数持久化【待实现：Cal_Load/Cal_Save】← 7.4/7.7
├─ comm/
│  ├─ app_can.h                 CAN 平台抽象接口
│  ├─ app_can.c                 CAN 适配层 L432 绑定（500kbps、过滤、Bus-Off 恢复、运行时开启自动重传）
│  ├─ app_log.h                 日志接口
│  ├─ app_log.c                 USART1 非阻塞日志 ← 9.2
│  ├─ can_node.h/.c             NodeID 枚举 / UID 排序 / token / 业务槽位 ID ← Docs/CAN_PROTOCOL.md
│  └─ board_comm.h/.c           NodeID 槽位业务帧（0x130~0x16A、故障/击打/控制/查询/ACK）← Docs/CAN_PROTOCOL.md
└─ app/
   ├─ main_app.h/.c             编排：App_Init/App_Loop/App_OnTick1ms + TIM 回调 ← 5.3/第 10 章
   ├─ state_machine.h/.c        八状态机 ← 第 11 章
   ├─ led_status.h/.c           九灯效联动 ← 8.2
   ├─ faults.h/.c               统一故障位图注册表（状态机/灯/上报的唯一真相源）← 7.5
   ├─ self_test.h/.c            非阻塞上电自检（≈1.1s）← 7.6
   └─ reset_cause.h/.c          复位原因锁存（offset=0x0 帧的数据源）
```

标注说明：**【待实现】**＝接口已定型、实现未完成（当前只剩 `Cal_Load`/`Cal_Save` 的参数持久化）；其余模块均已实现并在硬件上跑通。

## 3. 分层与依赖规则

- 依赖方向（原则上向下）：`app/` → `detect/`+`comm/`+`bsp/`；`detect/`、`comm/` → `bsp/`；`bsp/` → HAL + `board.h`。
- **`detect/` 不得依赖 `comm/`**（检测算法不感知通信）；状态/灯联动经 `app/` 层编排。这是唯一无例外的规则。
- 三处**有意为之**的例外，改动前先看清楚：① `app/faults.h` 与 `detect/calibration.h` 是跨层共享的**接口头**，`detect/`、`bsp/temp_mon`、`comm/` 都直接 include；② `comm/board_comm.c` include `app/` 侧头（它承担状态机/灯效/上报的编排）；③ `bsp/ads131m04.c` include `comm/app_log.h`。
- 允许的 include：`"main.h"`、`"board.h"`、`<stdint.h>` 等标准头、本层头 + 下层头（如 `"bsp/ads131m04.h"`、`"detect/calibration.h"`）。**不要** include 其他模块的 `.c` 或 CubeMX 生成文件内部头。

## 4. 命名与代码规范

- 函数/全局符号前缀＝模块名：`ADS131M04_`、`Ws2812_`、`TempMon_`、`RefStore_`、`HitDetect_`、`Cal_`、`App_Can_`、`App_Log_`、`CanNode_`、`BoardComm_`、`StateMachine_`、`LedStatus_`、`Fault_`、`SelfTest_`、`ResetCause_`、`App_`（编排）。
- 类型后缀 `_t`（如 `ads_frame_t`、`hit_event_t`、`hit_param_t`）；模块内私有符号一律 `static`。
- 注释用中文；桩实现标注 `TODO(Step N)：…`（N＝《设计方案》第 12 章实现步骤），完成一步删除对应 TODO。
- 常量/宏全大写；位域用 `(1u << n)` 风格并注释含义。

## 5. 头文件规范

- include guard：`APP_<文件名大写>_H`（如 `APP_ADS131M04_H`、`APP_HIT_DETECT_H`、`APP_BOARD_COMM_H`）。`ref_store.h` 的 `APP_BSP_REF_STORE_H` 是全库唯一的带层写法，新文件按前一种。
- 包含顺序：先本模块头，再 `"board.h"`/`"main.h"`，再标准库，再下层模块头。
- **`board.h` 是引用生成代码外设句柄（`hspi1` 等 extern）的唯一入口**——新增外设引用一律在 `board.h` 加 extern，禁止各文件自行 extern。
- 生成代码的引脚宏（`ADC_CS_Pin` 等）来自 `main.h`（CubeMX 重生成后自动更新，代码直接用宏名，不写死 GPIO_PIN_x）。

## 6. 中断与回调归属表

| weak 回调 | 定义位置 | 触发源（it.c 已生成） | 处理规则 |
|---|---|---|---|
| `HAL_TIM_PeriodElapsedCallback` | app/main_app.c | TIM2_IRQHandler（1kHz） | 仅 `htim->Instance==TIM2` 时调 `App_OnTick1ms()` |
| `HAL_GPIO_EXTI_Callback` | bsp/ads131m04.c | EXTI15_10_IRQHandler（PA15 DRDY，it.c 生成） | 仅 `ADC_NDRDY_Pin` 时调 `ADS131M04_IrqDrdy()` |
| `HAL_SPI_TxRxCpltCallback` | bsp/ads131m04.c | SPI1/DMA1_Ch2/3 IRQ | 仅 SPI1 时调 `ADS131M04_IrqDmaDone()` |
| `HAL_UART_TxCpltCallback` | bsp/ws2812_uart.c | USART2/DMA1_Ch7 IRQ | 仅 USART2 时调 `Ws2812_IrqTxDone()` |
| `HAL_CAN_RxFifo0MsgPendingCallback` | comm/app_can.c | CAN1_RX0_IRQHandler | 排空 FIFO0 并分发 |
| `HAL_CAN_ErrorCallback` | comm/app_can.c | CAN1_SCE_IRQHandler | 记录 LEC / 置 Bus-Off 恢复标志 |
| `HAL_PWR_PVD_Callback` | app/faults.c | PVD 中断（SYS_V_PVD_IN） | 置 sticky `FAULT_POWER`，仅自检通过或复位可清 |

**中断内铁律**：ISR 只做置标志/启动 DMA/计数/把小帧复制进队列，**禁止在中断里发送 CAN 或做协议处理**（发送与应答统一回主循环 `BoardComm_Loop`）；共享数据结构——写指针仅 ISR 改、读指针仅主循环改、槽状态用原子标志（《设计方案》R17）。

## 7. main.c USER CODE 接入点清单

| USER CODE 块 | 内容 | 说明 |
|---|---|---|
| `Includes` | `#include "app/main_app.h"` | CubeMX 重生成后需恢复 |
| `2` | `App_Init();`（在 `MX_TIM2_Init();` 之后） | 同上 |
| `3`（while 循环内） | `App_Loop();` | 同上 |

重生成后恢复步骤：① Makefile 重新追加 `-include app.mk`（§8）；② 检查上述三个 USER CODE 块内容仍在（CubeMX 保留 USER CODE 块，通常无需重做）。

## 8. app.mk 与构建

- `app.mk` 位于仓库根目录：`APP_SOURCES`（16 个源文件）+ `C_INCLUDES`（`-IApp/...` 五条）。
- Makefile 中 `-include app.mk` 的位置有讲究：**必须在 Makefile 自身 `C_SOURCES`/`C_INCLUDES` 定义之后、`OBJECTS` 计算之前**（否则 `=` 赋值覆盖追加、或新源不参与编译）。当前位于 C_INCLUDES 块之后。
- **CubeMX 重生成会覆写 Makefile → 必须重新追加 `-include app.mk`**（已写入 CLAUDE.md 提醒）。
- **文件名全局唯一**：Makefile 按 `notdir` 扁平化目标，`APP_SOURCES` 内任何两个文件不得同名（当前 16 个均唯一）。
- 新增模块三步：① 建 `App/<层>/xxx.c/.h`；② `app.mk` 的 `APP_SOURCES`/`C_INCLUDES` 各加一行；③ `make -j` 验证。
- 常用命令：
  ```bash
  make -j                      # 构建 → build/Armor_Cmzy26_081.{elf,hex,bin,map}
  make clean
  make GCC_PATH=<toolchain>    # 工具链不在 PATH 时
  arm-none-eabi-size build/Armor_Cmzy26_081.elf    # 内存核对
  ```
- 烧录（openocd，ST-Link）：
  ```bash
  openocd -f interface/stlink.cfg -f target/stm32l4x.cfg \
      -c "program build/Armor_Cmzy26_081.elf verify reset exit"
  ```
- `compile_commands.json`（clangd 用）**当前与 `app.mk` 同步**（App 侧 16 条，全库 48 条）。仓库内没有生成脚本，新增源文件后需手工照 `app.mk` 的 `APP_SOURCES` 补条目。

## 9. 参数表约定

- 唯一权威：`detect/calibration.h` 的 `hit_param_t`，字段与《设计方案》7.4 的 P01~P27 一一对应（默认值在 `Cal_GetDefaults()`，标注了依据）。
- 修改参数默认值 → 只改 `calibration.c` 的 `Cal_GetDefaults()`；新增参数 → 结构体加字段 + 默认值 + 同步《设计方案》7.4 表与下行命令参数 ID。
- Flash 持久化：`Cal_Load`/`Cal_Save`（双槽 + CRC，Step5 实现）；标定值（K_N/K_ch/极性掩码）经此恢复。
- **Flash 末页 `0x0801F800` 已被 `bsp/ref_store.c`（击打参考样本）占用**——新增任何持久化一律从倒数第二页往低地址排，勿碰末页。
- **窗口长度约束**：`P21 + P22 ≤ HIT_WAVE_MAX = 128`（超出由 `Hit_ClampParams` 钳位），当前默认取满（64+64，锁帧 32.8ms）。RAM 按上限静态分配，改小不省内存；**改窗长会让已存参考失配**（`s_ref_n` ≠ 本次窗长 → corr 恒 0），需靠下电重训或本上电第一击重建。

## 10. 状态与灯效枚举

- 状态机（`app/state_machine.h`）：`SM_STATE_BOOT/NORMAL/HIT/FAULT/COMM_LOST/ID_SETUP/ID_CONFLICT/REF_PENDING`（《设计方案》第 11 章状态表）。**门控优先级：故障 > REF_PENDING 紫闪校准 > COMM_LOST 离线 > 其余**（`StateMachine_Tick` 顶部门顺序即优先级）；`OnHitEvent` 的白名单含 `REF_PENDING`（上电第一击就是训练击打，不能被吞）。紫闪分两段：热身 5τ（`s_warmup_ms` 由 `HitDetect_GetWarmupMs()` 在 `StateMachine_Init` 缓存）紫 1Hz → 之后 20s 探测窗紫 5Hz；相位由 `Sm_RefPhaseTick()` 按 `s_train_ms`（自 BOOT 的墙钟，HIT 打断不清零）判定，且**必须由顶层校准门路径驱动**——门每 tick 调 `Sm_ToRefPending()` 后提前 return，`case SM_STATE_REF_PENDING` 只在门放行后才可达。
- **校准提前退出**：`BoardComm_MasterHeard()`（收到任一"电管→装甲板"方向的报文，`board_comm.c` 的 `Bc_IsMasterFrame`）且 `HitDetect_RefValid()` → `Sm_RefPendingNeeded()` 返回 0，立刻结束校准进 NORMAL（BOOT 阶段同理，白灯 300ms 后直接进 NORMAL 不闪紫）。**Flash 空则不退出**，照常校准训参考。三条退出路径（训练完成 / 上位机上线 / 20s 超时）**统一走 `Sm_LeaveRefPending()`**，只置 NORMAL + 灯效、**无 Flash 写入**——方案 B（2026-10-17）：`RefStore_Load` 上电纯读不擦页，页里始终保留旧参考，本上电唯一写入是训练击打的擦+写；任何非写入时刻掉电都不丢参考。
- 灯效（`app/led_status.h`）：`LED_EFF_NORMAL/HIT/ID_SETUP/FAULT/COMM_LOST/ID_CONFLICT/BOOT/REF_WARMUP/REF_PENDING`；频率：快闪 5Hz、慢闪 1Hz、ID_CONFLICT 红蓝紫 333ms、**REF_WARMUP 紫 1Hz 慢闪（基线 EMA 热身 5τ）→ REF_PENDING 紫 5Hz 快闪（20s 探测窗）**（与 COMM_LOST 紫常亮区分）；帧刷新 20Hz。注：紫 1Hz/5Hz 与 ID_SETUP/HIT 的队色节奏相同，**靠颜色区分**。
- FAULT **不是单一图案**，按故障类别分（`led_status.c` 的 `Led_FaultRgb`）：超温粉红常亮 / 温度出窗青 5Hz / PVD 欠压白单闪 2Hz / SPI·DRDY 链路琥珀 2Hz / 传感器类红蓝 500ms 交替。
- 指示 LED：**PB0=超温指示、PB1=系统正常指示**（NORMAL/HIT/ID_SETUP 亮；**高电平点亮、低电平灭**——硬件实测确认）。PB1 由 `LedStatus` 驱动；骨架阶段 main_app 的 1Hz 自证闪烁已随 Step3 移除。
- 检测调试变量（hit_detect.c）：`s_hit_dif`（实时差分信号）、`s_hit_sum`/`s_hit_bl[4]`（合力/基线）、`s_hit_state`（0=IDLE/1=ARMED/**2=CONFIRMING**/3=REFRACTORY/4=SETTLING）、`s_hit_event`/`s_hit_count`（**语义=接受事件数**）、`s_wave_ready`/`s_wave_frozen_*`（锁帧波形）、`s_hit_faults`。
- 确认段调试变量（hit_detect.c，两段式判定的第二段）：`s_hit_trig_cnt`（总触发数，含被拒）、`s_conf_reject_cnt`（相关度不足拒绝数）、`s_conf_abort_cnt`（冻结异常中止数）、**`s_conf_corr`（本次确认的 \|Pearson ρ\|×1024，判据本体）**、`s_confirm_done`。三路计数满足对账 `s_hit_trig_cnt = s_hit_count（接受） + s_conf_reject_cnt（拒判） + s_conf_abort_cnt（冻结异常）`。整定 P27 时观察 `s_conf_corr` + USART1 的 `[HIT] … corr=` / `[CONF] rej corr=` 日志。
- 热身调试变量（hit_detect.c）：`s_boot_samples`（自 Init 起消费的样本数，含自检喂帧/REFRACTORY 期间）、`s_warmup_samples`（=5×2^s_bl，s_bl=13 → 40960 ≈10.485s）。**热身未满 → 判据不判拒、训练禁用**，故 `s_boot_samples < s_warmup_samples` 时不该出现 `[CONF] trained` 或 `[CONF] rej`。
- 参考样本调试变量（hit_detect.c）：`s_ref_valid`（1=有可用参考）、`s_ref_trained_boot`（1=本次上电已训练）、`s_ref_n`（样本数，应 = 128 = P21+P22；不等则 corr 恒 0）、`s_ref_norm`（Σc²，归一化因子）、`s_ref_train_n`/`s_ref_train_err`（固化样本数 / 落盘结果）。Flash 侧失败码见 `ref_store.c` 的 `RefStore_GetErr()`（0=无错/1=擦除/2=编程/3=解锁）。
- 自检变量（ads131m04.c）：`s_st_dc[4]`（直流偏置，随姿态变化正常）、`s_st_rms[4]`（**去均值后**的真实噪声，期望 <100 counts）。

## 11. 数据流与缓冲约定

- **K=32 帧环形缓冲**（`ADS_RING_DEPTH`，32×18B=576B）：DRDY EXTI 取空闲槽写、槽满跳帧 + `drop_cnt` 计数；主循环按读指针消费（写指针仅 ISR、读指针仅主循环，R17）。
- WS2812 帧缓冲：`WS2812_FRAME_BYTES`=156B（13×12，编码为每 UART 字节 2 个 WS 位、每颜色字节 4 字节），DMA 发送 ≈416µs，20Hz 刷新。（118B 是已废弃的 2.667MBd 分数格方案的遗留值。）
- CAN 接收队列和击打事件队列均为 4 项固定帧缓冲；CAN 暂时发送失败时击打事件按 100ms 节拍重试，直到电管回 `offset=0xA` ACK。
- 事件流（**两段式**）：ADS 帧 → `HitDetect_Feed` → 触发（|hit_dif|≥P04 且连续≥P03）→ 锁帧 + CONFIRMING → 等满 P22 触发后样本 → 与**参考样本**做归一化互相关（P27）→ **接受**（**热身 5τ 内或参考不可用时判据一律让路**，见 `hit_detect.h` 参考语义段）：`hit_event_t`（冻结窗峰值 + `peak_ch[4]` + impulse/duration）→ 若为**热身后**本次上电第一次接受则固化参考 + 写 Flash 末页 → `StateMachine_OnHitEvent` → `BoardComm_ReportHitEvent`；**拒绝**：只留相关度 → `HitDetect_GetCorrReject`（main_app 打 `[CONF] rej corr=`，不上 CAN、不进状态机）。两条路都进 50ms 不应期 + P26 沉降门控。
- 层间约束：`detect/` 不依赖 `comm/`，故拒绝/事件都只入队，日志与上报由 `app/` 层取走。
- **Flash 写只在主循环**：`RefStore_Load` 上电**纯读、不擦页**（页保留旧参考直到训练写入覆盖）；`RefStore_Save`（擦+写 ~31ms）仅主循环可调，由训练链路调用（落在 50ms 不应期内），ISR 内禁止碰 Flash。

## 12. 扩展配方（step-by-step 文件清单）

| 场景 | 步骤 |
|---|---|
| **新增 BSP 设备** | ① `App/bsp/xxx.h/.c`（模块前缀 `Xxx_`）；② 外设句柄 extern 加入 `board.h`；③ `app.mk` 两行；④ `main_app.c` 的 `App_Init` 调 `Xxx_Init()` |
| **新增协议消息** | ① `board_comm.h` 分配固定 CAN ID；② `board_comm.c` 实现固定 DLC 的构造/解析；③ 同步 `Docs/CAN_PROTOCOL.md` 与《设计方案》第 9 章 |
| **新增可调参数** | ① `hit_param_t` 加字段；② `Cal_GetDefaults` 默认值；③ 同步《设计方案》7.4。（**当前没有参数下行通道**——CAN 只有 LED 控制 0x00~0x04 五个命令；要在线改参数得先在 `Docs/CAN_PROTOCOL.md` 定义新的固定帧 ID） |
| **新增灯效/状态** | ① `led_effect_t`/`sm_state_t` 加枚举值；② `led_status.c` 灯效生成；③ `state_machine.c` 转移条件；④ 同步《设计方案》8.2/11 章表 |
| **新增 Flash 持久化** | ① 确认**不占用末页 `0x0801F800`**（ref_store 专用）；② 按 `bsp/ref_store.c` 的模式写：上电纯读不擦页、写入时才先擦后写（~31ms，仅主循环），ISR 内禁止碰 Flash；③ CRC 覆盖全字段 |

## 13. 设计文档映射表

| 《设计方案》章节 | 工程落点 |
|---|---|
| 第 2 章 硬件事实 | `App/board.h`（句柄/常量/极性宏） |
| 第 4 章 CubeMX 配置 | `.ioc`（已完成，见 v1.5 配置表） |
| 第 6 章 ADC 驱动 | `App/bsp/ads131m04.h/.c` |
| 第 7 章 检测算法 | `App/detect/hit_detect.c`、`App/detect/calibration.c`、`App/bsp/temp_mon.c` |
| 第 8 章 灯驱动 | `App/bsp/ws2812_uart.c`、`App/app/led_status.c` |
| 第 9 章 通信 | `App/comm/{app_can,can_node,board_comm,app_log}.c` |
| 第 10 章 调度 | `App/app/main_app.c`（App_Loop/App_OnTick1ms） |
| 第 11 章 状态机 | `App/app/state_machine.c` |
| 第 12 章 里程碑 | 本文 §12 步骤与 `TODO(Step N)` 标记 |
