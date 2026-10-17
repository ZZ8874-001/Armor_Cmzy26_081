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
 * 训练永不发生，参考永远建立不起来（死锁）。 */

/* 已有可用参考：Flash 载入的旧值 或 本次上电训练写入的新值。 */
bool HitDetect_RefValid(void);

/* 本次上电是否已完成训练（状态机紫闪校准的退出条件之一；Flash 载入不算训练）。 */
bool HitDetect_RefTrainedThisBoot(void);

/* 基线热身时长 ms（P01 s_bl 的 5τ；s_bl=13 → 40960 样本 → 10485ms）。
 * 状态机 Init 时取一次作紫闪 1Hz→5Hz 的相位翻转点与探测窗起点。 */
uint32_t HitDetect_GetWarmupMs(void);

/* 最近一次确认得到的相关度 q10（接受/拒绝都更新；诊断用，
 * 热身期/无参考/窗长失配时为 0）。 */
uint16_t HitDetect_GetLastCorr(void);

/* 取最新一次"相关度不足"拒绝的相关度（一次性取出，深度 1）；无新拒绝返回 false。
 * detect 层不依赖 comm，日志由 app 层经此取走（同上，一次只有相关度一个字段）。 */
bool HitDetect_GetCorrReject(uint16_t *corr_q10);

/* 取最新一次"训练完成"通知（一次性取出，深度 1）：样本数 + 落盘结果
 * （flash_err：0=成功，其余见 RefStore_GetErr）。 */
bool HitDetect_GetTrainedNotice(uint16_t *n, uint32_t *flash_err);

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
