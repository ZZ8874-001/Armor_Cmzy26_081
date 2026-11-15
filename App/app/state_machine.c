/**
 ******************************************************************************
 * @file    state_machine.c
 * @brief   模块状态机实现（TIM2 1kHz 中断驱动，设计文档 v1.5 第 11 章）。
 *
 * 八状态：BOOT / NORMAL / HIT / FAULT / COMM_LOST / ID_SETUP / ID_CONFLICT /
 * REF_PENDING（上电校准：紫闪等首次击打记录参考样本）。
 * 转移条件：
 *   BOOT   300ms 自检白灯 → NORMAL
 *   NORMAL 击打事件（StateMachine_OnHitEvent）→ HIT（快闪 300ms 后回 NORMAL）
 *   任一态 Fault_GetBitmap() 非零 → FAULT（故障优先，≤1ms 响应）；清零 → NORMAL
 *   REF_PENDING 校准门：未训练且（探测窗未过 或 无可用参考）→ 两段紫闪；
 *     三条退出路径 → NORMAL（优先级见下）：
 *     ① 训练完成；② 上位机上线且 Flash 有旧参考（整车已在工作，提前结束校准）；
 *     ③ 热身 5τ+20s 超时且 Flash 有旧参考（沿用旧值）。
 *     Flash 空时只剩"训练完成"一条路——没有旧值可沿用，必须照常校准。
 *     └ 灯效分两段：基线 EMA 热身着 5τ（≈10.5s）紫 1Hz 慢闪 → 之后 20s 击打
 *       探测窗紫 5Hz 快闪。热身期检测层不判拒、不训练（见 hit_detect.c），
 *       紫闪只是把"现在不是纯样本窗口"告诉操作者。
 *   COMM_LOST/ID_* 由通信事件进出（board_comm 驱动）
 *
 * 门控优先级（StateMachine_Tick 的判定顺序）：
 *   各类模块故障 > 紫闪校准(REF_PENDING) > 装甲模块离线(COMM_LOST) > 队伍色/ID/受击
 * 即：紫闪期间发生故障立刻转 FAULT（故障图案优先显示），离线时若仍在校准窗口
 * 仍显示紫闪（校准是上电暂态，先完成再显示离线）；紫闪与紫常亮只在 100ms 亮灭上
 * 区分，用户手册需说明两者含义不同。
 ******************************************************************************
 */
#include "app/state_machine.h"
#include "app/led_status.h"
#include "app/faults.h"
#include "comm/board_comm.h"    /* BoardComm_MasterHeard：上位机上线 → 提前结束校准 */
#include "detect/hit_detect.h"

#define SM_BOOT_MS   300u      /* BOOT 白灯时长 */
#define SM_HIT_MS    300u      /* HIT 快闪时长（5Hz × 3 个周期 = 300ms） */
#define REF_TRAIN_TIMEOUT_MS 20000u  /* 探测窗：自热身 5τ 结束起算 20s，逾期沿用上次 Flash 参考值 */

static volatile sm_state_t s_state = SM_STATE_BOOT;
static volatile uint32_t   s_state_ms;   /* 当前状态已驻留 ms 数（1kHz 累计） */
static volatile uint8_t    s_comm_lost;  /* 通信层置位；在 BOOT 结束后仍须持续生效 */
static volatile uint32_t   s_train_ms;   /* 自 BOOT 起算的上电耗时（热身+20s 探测窗基准） */
static uint32_t            s_warmup_ms;  /* 热身 5τ 的毫秒数（Init 时向检测层取，此后恒定） */
static volatile uint8_t    s_ref_phase;  /* 紫闪相位：0=热身着(1Hz) 1=击打探测窗(5Hz) 0xFF=未定 */
static volatile uint16_t   s_comm_hit_ms;/* COMM_LOST 受击紫闪计时：0=空闲，1..SM_HIT_MS=闪灯中 */

/* 进入 FAULT（故障灯效由 led_status 按 Fault_GetBitmap 分类生成） */
static void Sm_ToFault(void)
{
    s_state = SM_STATE_FAULT;
    s_state_ms = 0u;
    LedStatus_SetEffect(LED_EFF_FAULT);
}

/* 是否该停在紫闪校准态：本次上电未训练 且（热身+20s 探测窗未过 或 Flash 无旧值可沿用）。
 * 三条放行路径（都需"未训练"，由 Sm_LeaveRefPending 统一收尾）：
 *   ① 上位机已上线 + Flash 有旧值 → 立即放行（整车已在工作，不该再等热身/探测窗）；
 *   ② 热身+20s 超时 + Flash 有旧值 → 放行（沿用旧值）；
 *   ③ 已训练 → 放行。 */
static uint8_t Sm_RefPendingNeeded(void)
{
    if (HitDetect_RefTrainedThisBoot()) { return 0u; }
    /* Flash 空时 RefValid()=0，两条放行路径都不成立 → 保持紫闪走正常校准。 */
    if (BoardComm_MasterHeard() && HitDetect_RefValid()) { return 0u; }
    if (s_train_ms < s_warmup_ms + REF_TRAIN_TIMEOUT_MS) { return 1u; }
    return HitDetect_RefValid() ? 0u : 1u;
}

/* 退出校准回 NORMAL。方案 B（2026-10-17）：上电 Load 不再擦页，页里始终保留
 * 旧参考，本上电唯一一次写入是训练击打的擦+写（Hit_RefTrain → RefStore_Save，
 * 落在 50ms 不应期内）——因此"未训练即退出"无需任何回写，参考天然跨断电保留，
 * 掉电窗口只剩写入进行中的 ~31ms（单页无备份，物理上无法避免）。
 * 所有退出路径统一走这里，只负责置 NORMAL + 灯效。 */
static void Sm_LeaveRefPending(void)
{
    s_state = SM_STATE_NORMAL;
    s_state_ms = 0u;
    LedStatus_SetEffect(LED_EFF_NORMAL);
}

/* 按已耗时切紫闪相位（0=热身 1Hz 慢闪 / 1=探测窗 5Hz 快闪），仅在相位变化时刷灯。
 * 必须用 s_train_ms 而非 s_state_ms：HIT/ID 态会打断 REF_PENDING 并清零 s_state_ms，
 * 而热身进度是自 BOOT 起算的墙钟，不受打断影响。 */
static void Sm_RefPhaseTick(void)
{
    uint8_t phase = (s_train_ms >= s_warmup_ms) ? 1u : 0u;   /* 判据与检测层同一时点 */

    if (phase != s_ref_phase)
    {
        s_ref_phase = phase;
        LedStatus_SetEffect(phase != 0u ? LED_EFF_REF_PENDING : LED_EFF_REF_WARMUP);
    }
}

/* 进入紫闪校准态（幂等：已在其中则不打断驻留计时）。
 * 相位翻转必须由本函数驱动：顶层校准门每 tick 调用它并提前 return，
 * `case SM_STATE_REF_PENDING` 只在门放行后才可达——翻转逻辑放在 case 里永不执行。 */
static void Sm_ToRefPending(void)
{
    if (s_state != SM_STATE_REF_PENDING)
    {
        s_state = SM_STATE_REF_PENDING;
        s_state_ms = 0u;
        s_ref_phase = 0xFFu;   /* 刚进来相位未定：强制下一行重刷，覆盖 HIT/FAULT 残留灯效 */
    }
    Sm_RefPhaseTick();
}

void StateMachine_Init(void)
{
    s_state = SM_STATE_BOOT;
    s_state_ms = 0u;
    s_comm_lost = 0u;
    s_train_ms = 0u;
    s_ref_phase = 0xFFu;                  /* 相位未定，首次 Sm_ToRefPending 时定 */
    s_comm_hit_ms = 0u;
    s_warmup_ms = HitDetect_GetWarmupMs(); /* HitDetect_Init 已先跑（main_app 调用序），无竞态 */
    LedStatus_SetEffect(LED_EFF_BOOT);
}

void StateMachine_Tick(void)
{
    s_state_ms++;
    s_train_ms++;

    /* 安全状态优先于 BOOT/HIT/ID 等暂态。这样即使 BOOT 在掉线后完成，
     * 也不会把 COMM_LOST 覆盖为 NORMAL。
     * 合并自 44bfd88(faults 统一注册表) 与 83bd9e7(COMM_LOST 锁存)：
     * 顶层门统一以 Fault_GetBitmap() 判定，满足「任一态故障 ≤1ms 进 FAULT」。 */
    if (Fault_GetBitmap() != 0u)
    {
        if (s_state != SM_STATE_FAULT)
        {
            s_state = SM_STATE_FAULT;
            s_state_ms = 0u;
            LedStatus_SetEffect(LED_EFF_FAULT);
        }
        return;
    }
    /* 上电校准门——优先级：故障之后、离线之前（用户指定）。
     * BOOT 白灯 300ms 不打断（"初始化之后"才紫闪）。
     * 门内紫闪分两段：热身 5τ 紫 1Hz 慢闪 → 20s 探测窗紫 5Hz 快闪（相位由 Sm_RefPhaseTick 切）。 */
    if (s_state != SM_STATE_BOOT)
    {
        if (Sm_RefPendingNeeded())
        {
            Sm_ToRefPending();
            return;
        }
        /* 校准阶段已结束（训练完成 / 超时沿用 Flash 参考 / 上位机上线跳过）：
         * 参考判拒武装——与 CAN 在线与否无关（2026-10-30 决策）。
         * 挂在此处而非 Sm_LeaveRefPending 内：Sm_RefPendingNeeded 是校准的唯一权威
         * 判据，放行即结束；故障恢复/离线恢复等旁路不经 Sm_LeaveRefPending 也能覆盖。
         * 幂等，每 tick 一次置位无副作用。
         * **必须与校准门同受 BOOT 守卫**：BOOT 期间武装会让判拒在紫闪校准窗内
         * 生效，Flash 旧参考会拒掉校准击打 → 训练永不可达（2026-10-30 修）。 */
        HitDetect_ArmReference();
    }

    if (s_comm_lost != 0u)
    {
        if (s_state != SM_STATE_COMM_LOST)
        {
            s_state = SM_STATE_COMM_LOST;
            s_state_ms = 0u;
            LedStatus_SetEffect(LED_EFF_COMM_LOST);
        }
        /* 离线受击快闪：到点恢复紫常亮（状态保持 COMM_LOST，不上报、PB1 不亮） */
        if (s_comm_hit_ms != 0u)
        {
            if (++s_comm_hit_ms >= SM_HIT_MS)
            {
                s_comm_hit_ms = 0u;
                LedStatus_SetEffect(LED_EFF_COMM_LOST);
            }
        }
        return;
    }

    switch (s_state)
    {
    case SM_STATE_BOOT:
        if (s_state_ms >= SM_BOOT_MS)
        {
            /* 校准未完成直接进紫闪，不闪一帧队伍色（20Hz 网格上会是可见的 50ms） */
            if (Sm_RefPendingNeeded()) { Sm_ToRefPending(); }
            else { Sm_LeaveRefPending(); }
        }
        break;

    case SM_STATE_REF_PENDING:
        /* 只有校准门放行才会走到这里；Sm_LeaveRefPending 置 NORMAL（方案 B：无需回写，页里仍是旧参考）。 */
        Sm_LeaveRefPending();
        break;

    case SM_STATE_NORMAL:
        if (Fault_GetBitmap() != 0u)
        {
            Sm_ToFault();
        }
        break;

    case SM_STATE_HIT:
        /* 故障优先：受击闪灯期间新故障 ≤1ms 内转入 FAULT */
        if (Fault_GetBitmap() != 0u)
        {
            Sm_ToFault();
        }
        else if (s_state_ms >= SM_HIT_MS)
        {
            s_state = SM_STATE_NORMAL;
            s_state_ms = 0u;
            LedStatus_SetEffect(LED_EFF_NORMAL);
        }
        break;

    case SM_STATE_FAULT:
        if (Fault_GetBitmap() == 0u)
        {
            s_state = SM_STATE_NORMAL;
            s_state_ms = 0u;
            LedStatus_SetEffect(LED_EFF_NORMAL);
        }
        break;

    case SM_STATE_COMM_LOST:
        /* 故障优先于通信状态；无故障保持紫常亮（恢复由 OnCommLost(false) 驱动） */
        if (Fault_GetBitmap() != 0u)
        {
            Sm_ToFault();
        }
        break;

    case SM_STATE_ID_SETUP:
    case SM_STATE_ID_CONFLICT:
        if (Fault_GetBitmap() != 0u)
        {
            Sm_ToFault();
        }
        else if (s_state_ms >= 1000u)   /* 1s 显示后自动回 NORMAL */
        {
            s_state = SM_STATE_NORMAL;
            s_state_ms = 0u;
            LedStatus_SetEffect(LED_EFF_NORMAL);
        }
        break;

    default:
        break;
    }
}

void StateMachine_OnCommLost(bool lost)
{
    if (lost)
    {
        s_comm_lost = 1u;
        /* BoardComm_Loop 每圈都会带 lost=true 调本函数：守卫必须含
         * COMM_LOST 本身，否则每圈重设紫常亮灯效、把受击闪灯盖掉。 */
        if (s_state != SM_STATE_FAULT && s_state != SM_STATE_COMM_LOST)
        {
            s_state = SM_STATE_COMM_LOST;
            s_state_ms = 0u;
            LedStatus_SetEffect(LED_EFF_COMM_LOST);
        }
    }
    else
    {
        s_comm_lost = 0u;
        s_comm_hit_ms = 0u;    /* 闪灯中恢复链接：终止计时，回 NORMAL 灯效 */
        if (s_state == SM_STATE_COMM_LOST)
        {
            s_state = SM_STATE_NORMAL;
            s_state_ms = 0u;
            LedStatus_SetEffect(LED_EFF_NORMAL);
        }
    }
}

void StateMachine_OnIdSet(bool conflict)
{
    if (conflict)
    {
        s_state = SM_STATE_ID_CONFLICT;
    }
    else
    {
        s_state = SM_STATE_ID_SETUP;
    }
    s_state_ms = 0u;
    LedStatus_SetEffect(conflict ? LED_EFF_ID_CONFLICT : LED_EFF_ID_SETUP);
}

void StateMachine_OnHitEvent(const hit_event_t *e)
{
    (void)e;
    /* NORMAL/HIT/REF_PENDING 中受击 → 刷新快闪计时（HIT 中重击不重新开始也可，取刷新）。
     * REF_PENDING 必须在内：上电后第一击正是"训练击打"，若在此被忽略，
     * main_app 的 `Get()==HIT` 上报门就会把这一击吞掉（电管少记一次击打）。 */
    if (s_state == SM_STATE_NORMAL || s_state == SM_STATE_HIT ||
        s_state == SM_STATE_REF_PENDING)
    {
        s_state = SM_STATE_HIT;
        s_state_ms = 0u;
        LedStatus_SetEffect(LED_EFF_HIT);
    }
    else if (s_state == SM_STATE_COMM_LOST)
    {
        /* 离线也有击打灯效（2026-10-30）：复用紫闪 5Hz 图案（REF_PENDING），
         * 颜色与常亮紫一致；状态不动——不上报 CAN、PB1 保持灭，
         * Tick 离线门 300ms 后恢复紫常亮。闪灯期间再受击 → 重新置 1（刷新）。 */
        s_comm_hit_ms = 1u;
        LedStatus_SetEffect(LED_EFF_REF_PENDING);
    }
    /* FAULT 态不发送只计数（11 章语义）——事件仍由 main_app 日志输出 */
}

sm_state_t StateMachine_Get(void)
{
    return (sm_state_t)s_state;
}
