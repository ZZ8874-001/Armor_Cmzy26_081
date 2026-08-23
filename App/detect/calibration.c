/**
 ******************************************************************************
 * @file    calibration.c
 * @brief   可调参数表默认值（P01~P24，设计文档 v1.5 第 7.4 节）。
 *
 * TODO(Step 5)：Cal_Load/Cal_Save 实现 Flash 双槽 + CRC 持久化。
 ******************************************************************************
 */
#include "detect/calibration.h"

/* 0.9 × 2^23 = 7549747.2（P08 默认饱和阈值，数据手册线性区） */
#define DEFAULT_SAT_THR  7549747

void Cal_GetDefaults(hit_param_t *p)
{
    /* --- 检测管线 --- */
    p->s_bl           = 13u;
    p->win_len        = 32u;
    p->min_dur        = 4u;
    p->thr_hit        = 20000;            /* 调试初值，标定后按 19N×K_N×1.2 写入 */
    p->refractory_ms  = 50u;

    /* --- 标定与诊断 --- */
    p->k_n            = 0.0f;             /* 待标定 */
    p->k_ch[0]        = 1.0f;
    p->k_ch[1]        = 1.0f;
    p->k_ch[2]        = 1.0f;
    p->k_ch[3]        = 1.0f;
    p->sat_thr        = DEFAULT_SAT_THR;
    p->sat_time_ms    = 1000u;
    p->dead_var       = 8u;

    /* --- 温度与时序 --- */
    p->temp_win_low_mv  = 300u;           /* 0.30V */
    p->temp_win_high_mv = 750u;           /* 0.75V */
    p->drdy_timeout_ms  = 3u;

    /* --- 通信与灯光 --- */
    p->heartbeat_ms   = 50u;              /* 20Hz */
    p->comm_timeout_ms = 200u;            /* 4×心跳 */
    p->brightness     = 60u;
    p->team_color     = TEAM_COLOR_RED;

    /* --- ADC 与开关 --- */
    p->gain1[0] = 1u;
    p->gain1[1] = 1u;
    p->gain1[2] = 1u;
    p->gain1[3] = 1u;
    p->evt_enable     = 0x01u;            /* 事件使能开，CRC 关 */
    p->temp_over_mv   = 750u;             /* 保护阈值默认最宽松：=窗上限，等效不触发 */
    p->osr            = 1024u;

    /* --- 缓冲与上报 --- */
    p->ring_depth     = 32u;
    p->wave_window    = 32u;
    p->status_period_ms = 1000u;          /* 默认 1Hz，由裁判系统仲裁确定、可调 */
}

void Cal_Load(hit_param_t *p)
{
    /* TODO(Step 5)：Flash 双槽读取 + CRC 校验，失败回退默认值。 */
    Cal_GetDefaults(p);
}

int Cal_Save(const hit_param_t *p)
{
    (void)p;
    /* TODO(Step 5)：Flash 双槽写入 + CRC。 */
    return 0;
}
