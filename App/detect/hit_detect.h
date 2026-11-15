/**
 ******************************************************************************
 * @file    hit_detect.h
 * @brief   击打检测管线接口（四传感器融合模型 + 强度量化 + 健康诊断 + 极性校对）。
 *
 * 设计文档 v1.5 第 7 章为唯一权威来源：融合模型与管线 7.1、强度量化 7.2、
 * 事件结构 7.3、参数表 7.4、故障诊断 7.5、自检 7.6、标定 7.7、
 * 弹速/入射角/击打位置反推预留 7.8。
 ******************************************************************************
 */
#ifndef APP_HIT_DETECT_H
#define APP_HIT_DETECT_H

#include <stdint.h>
#include <stdbool.h>
#include "bsp/ads131m04.h"
#include "detect/calibration.h"
#include "app/faults.h"    /* 统一故障位图（FAULT_*，7.5） */

/* 击打事件（7.3）：合力判定、四通道峰值分布、预留反推字段 */
typedef struct
{
    uint8_t  ch;              /* 触发方式：0xFF=融合判定（合力信号），预留 */
    uint8_t  intensity;       /* 强度 0-100%（合力模型，7.2） */
    uint16_t force_01n;       /* 合力，0.01N 单位（7.2） */
    uint32_t peak;            /* 合力峰值绝对值 */
    uint32_t peak_ch[4];      /* 四通道峰值分布（击打位置估算输入，7.8） */
    uint32_t ts_ms;           /* HAL_GetTick 时间戳 */
    uint32_t impulse;         /* 预留：冲量（单位经标定后定，7.8） */
    uint16_t duration_ms;     /* 预留：撞击时间=脉宽（7.8） */
} hit_event_t;

/* 弹速/入射角反推结果（预留，7.8/M6） */
typedef struct
{
    int32_t velocity_mms;     /* 弹速 mm/s（标定后） */
    int32_t angle_deg100;     /* 入射角 0.01°（标定后） */
} impact_info_t;

/* 击打位置估算结果（预留，7.8/M2 确定坐标轴） */
typedef struct
{
    int32_t x_01;             /* 0.01 单位归一化坐标 */
    int32_t y_01;
} pos_info_t;

/* 冻结窗波形导出（USART1 上位机逐通道分析，Docs/USART1波形数据帧格式.md）：
 * 每次确认完成（接受/相关度拒/校准拒）时一次性快照的元数据；窗数据本身由
 * HitDetect_GetWaveTx 直接从冻结缓冲拷出。中止（Hit_ConfirmAbort）不产生快照，
 * 上位机由 trig_id 跳号发现。 */
typedef struct
{
    uint8_t  type;            /* 1=接受 2=相关度拒绝 3=校准合法性拒绝（接受但未训练） */
    uint8_t  flags;           /* bit0=确认时参考有效 bit1=确认前本上电已训练
                               * bit2=本次确认完成训练 bit3=基线热身已满 */
    uint16_t win_len;         /* 窗长（= P21 预触发 + P22 触发后） */
    uint32_t ts_ms;           /* 触发时刻 HAL_GetTick */
    uint32_t trig_id;         /* 总触发号：含被拒/中止；跳号=中止或发送丢弃 */
    uint16_t corr_q10;        /* 相关度 |ρ|×1024（判据本体） */
    uint32_t pk_idx;          /* |hp_sum| 峰值索引（校准合法性判据） */
    uint32_t peak_dif;        /* |dif_sum| 峰值（= 事件 peak / [HIT] 日志 sum_peak） */
    uint32_t peak_ch[4];      /* 各通道 |hp| 峰值（= 事件 peak_ch） */
} hit_wave_tx_t;

/* 初始化：载入参数表（含 Flash 标定值回读）。 */
void HitDetect_Init(const hit_param_t *p);

/* 每帧（3.906kHz）喂入：EMA→合力→窗口峰值→阈值/持续判定→不应期。 */
void HitDetect_Feed(const ads_frame_t *f);

/* 20Hz 健康评估（TIM2 50ms 分频调用，7.5）：饱和/偏置/死通道/SPI 故障。 */
void HitDetect_Tick(void);

/* 取最新事件（非破坏）；无新事件返回 false。 */
bool HitDetect_GetEvent(hit_event_t *e);

/* ---- 参考样本相关判定（P27，两段式第二段的判据） ----
 * 语义：**热身后**第一次"被接受"的击打的锁帧合力窗固化为参考并落盘；此后每次
 * 确认都与参考做归一化互相关（Pearson |ρ|×1024，与幅值无关），低于 P27 则拒绝
 * （P27=0 时只统计不判拒）。参考的载入/固化见 bsp/ref_store.h。
 *
 * 热身（P01 s_bl 的 5 个 EMA 时间常数，5×2^s_bl 样本，≈10.5s）：基线滤波从 0
 * 起步，前几个 τ 是对数建立瞬态，此窗内固化参考会污染样本。故热身期**不判拒、
 * 不训练**——击打照常触发/计数/上报（不用受污染的窗去否掉真实击打），但参考窗
 * 保持纯净。热身期判据退化为"一律接受"。
 *
 * 无可用参考时（首上电/Flash 空）也一样接受：否则 corr=0 < P27 会把首击拒掉，
 * 训练永不发生，参考永远建立不起来（死锁）。
 *
 * 校准阶段未结束不判拒（2026-10-30 修订，取代 2026-10-17 的「训练前不判拒」）：
 * 开机校准（REF_PENDING）期间一律接受——校准击打不因与 Flash 旧参考相关度差而
 * 被放弃训练。校准结束（训练完成/超时沿用 Flash 参考/上位机上线跳过，与 CAN
 * 在线与否无关）后由状态机调 HitDetect_ArmReference 武装判拒，此后 Flash 参考
 * 参与 P27 判定。旧门用「本上电已训练」导致重启后校准超时退出且窗内无击打时
 * 整个 boot 判拒关闭（异常灵敏）。坏参考的逃生路径：① 任一 corr≥P27 的击打
 * 照常接受并训练刷新参考；② 上位机 HCMD SET_REFERENCE 推送一帧好参照。
 *
 * 训练合法性（2026-10-17 放宽版）：|hp_sum| 峰值 idx 距窗首/尾 ≥5 样本才固化，
 * 贴边窗不训练、机会保留给下一击（诊断经 HitDetect_GetCalReject 一次性取出）。 */

/* 已有可用参考：Flash 载入的旧值 或 本次上电训练写入的新值。 */
bool HitDetect_RefValid(void);

/* 本次上电是否已完成训练（状态机紫闪校准的退出条件之一；Flash 载入不算训练）。 */
bool HitDetect_RefTrainedThisBoot(void);

/* 校准阶段结束通知（幂等）：此后 Flash 参考参与 P27 判拒。
 * 由状态机在校准门放行处（训练完成/超时/上位机跳过）调用，与 CAN 在线与否无关。 */
void HitDetect_ArmReference(void);

/* 基线热身时长 ms（P01 s_bl 的 5τ；s_bl=13 → 40960 样本 → 10485ms）。
 * 状态机 Init 时取一次作紫闪 1Hz→5Hz 的相位翻转点与探测窗起点。 */
uint32_t HitDetect_GetWarmupMs(void);

/* 最近一次确认得到的相关度 q10（接受/拒绝都更新；诊断用，
 * 热身期/无参考/窗长失配时为 0）。 */
uint16_t HitDetect_GetLastCorr(void);

/* 取最新一次"相关度不足"拒绝的相关度（一次性取出，深度 1）；无新拒绝返回 false。
 * detect 层不依赖 comm，日志由 app 层经此取走（同上，一次只有相关度一个字段）。 */
bool HitDetect_GetCorrReject(uint16_t *corr_q10);

/* 取最新一次"校准合法性拒绝"的峰值 idx（一次性取出，深度 1）；无新拒绝返回 false。
 * 触发条件：训练击打的 |hp_sum| 峰值落在窗首/尾 5 样本内（见 hit_detect.c）。 */
bool HitDetect_GetCalReject(uint16_t *pk_idx);

/* 取最新一次确认的冻结窗 4 通道 hp 波形 + 元数据（一次性取出，深度 1）；
 * 无新快照返回 false。hp 按 128×4 的 int32 数组分配（窗长上限 128，同
 * HitDetect_GetWaveform），布局 [sample][ch] 与冻结缓冲一致（直 memcpy）；
 * *n 返回样本数。
 * 不动 GetWaveform 的一次性语义（s_wave_ready 不受影响）。
 * 三种确认结果都有帧；中止无帧 → 看 trig_id 是否跳号。 */
bool HitDetect_GetWaveTx(hit_wave_tx_t *info, int32_t *hp, uint16_t *n);

/* 取最新一次"训练完成"通知（一次性取出，深度 1）：样本数 + 落盘结果
 * （flash_err：0=成功，其余见 RefStore_GetErr）。 */
bool HitDetect_GetTrainedNotice(uint16_t *n, uint32_t *flash_err);

/* 用宿主推送的合力窗立即替换参考（HCMD SET_REFERENCE，主循环专用）：
 * 校验窗长 == P21+P22、拒绝锁帧确认中/平窗后，先装入 RAM（置位本上电已训练，
 * 立即参与相关度判定）再落盘页 63。
 * 返回 0=RAM 安装且落盘成功；1=RAM 安装但落盘失败（仍生效）；负值=拒绝，
 * RAM 与 Flash 均未动（-1 参数空，-2 窗长不匹配，-3 平窗，-4 确认中）。 */
int HitDetect_SetReference(const int32_t *sum, uint16_t n, uint32_t *flash_err);

/* 当前冻结窗长度（P21+P22），宿主推送前校验用。 */
uint16_t HitDetect_GetWindowLen(void);

/* 锁帧后正在等 P22 个触发后样本（Flash 擦写会打断本次确认）。 */
bool HitDetect_IsConfirming(void);

/* 运行时更新参数。 */
void HitDetect_UpdateParams(const hit_param_t *p);

/* 当前故障位图（7.5，每 20Hz 评估）。 */
uint16_t HitDetect_GetFaultFlags(void);

/* 预留：开机击打校对极性（6.7，M2 落地）。 */
int  HitDetect_CalibratePolarity(void);

/* 波形捕获：取上次触发冻结的窗口（一次性取出，置 s_wave_ready=0）。
 * 返回样本数 n（= P21+P22）；buf 需 ≥ HIT_WAVE_MAX = 128 样本（上限，当前 P21=P22=64
 * 即取满），该宏定义在 hit_detect.c 内，调用方须自行按 128 分配。ch=0xFF 取合力通道。 */
int  HitDetect_GetWaveform(uint8_t ch, ads_sample_t *buf, uint16_t *n);

/* 预留：弹速/入射角反推（7.8，M6 实现）。 */
int  HitDetect_EstimateImpact(const hit_event_t *e, impact_info_t *info);

/* 预留：击打位置估算（7.8，M2 确定坐标轴/M6 标定）。 */
int  HitDetect_EstimatePosition(const hit_event_t *e, pos_info_t *pos);

#endif /* APP_HIT_DETECT_H */
