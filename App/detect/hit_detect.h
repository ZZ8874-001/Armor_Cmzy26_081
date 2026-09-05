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

/* 故障位图（与 TempMon 故障位合并上报，7.5） */
#define FAULT_CH0_SAT     (1u << 0)
#define FAULT_CH1_SAT     (1u << 1)
#define FAULT_CH2_SAT     (1u << 2)
#define FAULT_CH3_SAT     (1u << 3)
#define FAULT_CH0_OFFSET  (1u << 4)
#define FAULT_CH1_OFFSET  (1u << 5)
#define FAULT_CH2_OFFSET  (1u << 6)
#define FAULT_CH3_OFFSET  (1u << 7)
#define FAULT_CH0_DEAD    (1u << 8)
#define FAULT_CH1_DEAD    (1u << 9)
#define FAULT_CH2_DEAD    (1u << 10)
#define FAULT_CH3_DEAD    (1u << 11)
#define FAULT_SPI         (1u << 12)
#define FAULT_POWER       (1u << 13)   /* PVD 触发 */
#define FAULT_DAC         (1u << 14)   /* DAC 注入自检失效 */

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

/* 运行时更新参数。 */
void HitDetect_UpdateParams(const hit_param_t *p);

/* 当前故障位图（7.5，每 20Hz 评估）。 */
uint16_t HitDetect_GetFaultFlags(void);

/* 预留：开机击打校对极性（6.7，M2 落地）。 */
int  HitDetect_CalibratePolarity(void);

/* 预留：波形捕获（环形窗口冻结，7.8，M2 实现）。 */
int  HitDetect_GetWaveform(uint8_t ch, ads_sample_t *buf, uint16_t *n);

/* 预留：弹速/入射角反推（7.8，M6 实现）。 */
int  HitDetect_EstimateImpact(const hit_event_t *e, impact_info_t *info);

/* 预留：击打位置估算（7.8，M2 确定坐标轴/M6 标定）。 */
int  HitDetect_EstimatePosition(const hit_event_t *e, pos_info_t *pos);

#endif /* APP_HIT_DETECT_H */
