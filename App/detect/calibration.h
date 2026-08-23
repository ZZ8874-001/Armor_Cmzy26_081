/**
 ******************************************************************************
 * @file    calibration.h
 * @brief   可调参数表（P01~P24）与标定数据持久化接口。
 *
 * 设计文档 v1.5 第 7.4 节为参数唯一权威来源（默认值/范围/依据）。
 ******************************************************************************
 */
#ifndef APP_CALIBRATION_H
#define APP_CALIBRATION_H

#include <stdint.h>
#include <stdbool.h>

/* 队伍色（P16） */
#define TEAM_COLOR_RED   0u
#define TEAM_COLOR_BLUE  1u

/* 可调参数表：字段一一对应设计文档 7.4 的 P01~P24 */
typedef struct
{
    /* --- 检测管线（P01~P05） --- */
    uint8_t  s_bl;              /* P01 基线 EMA 移位位数（默认 13，τ≈2.1s） */
    uint16_t win_len;           /* P02 窗口长度（样本，默认 32=8.2ms） */
    uint8_t  min_dur;           /* P03 最小持续（样本，默认 4=1.02ms） */
    int32_t  thr_hit;           /* P04 击打阈值 counts（标定后写入；调试初值 20000） */
    uint16_t refractory_ms;     /* P05 不应期（默认 50ms，≤20Hz） */

    /* --- 标定与诊断（P06~P10） --- */
    float    k_n;               /* P06 力度系数 counts/0.01N（19N/100N 两点标定） */
    float    k_ch[4];           /* P07 通道增益补偿（默认 1.0） */
    int32_t  sat_thr;           /* P08 饱和阈值（默认 0.9×2^23=7549747） */
    uint16_t sat_time_ms;       /* P09 饱和判定时长（默认 1000ms） */
    uint16_t dead_var;          /* P10 死区方差（默认 8 counts） */

    /* --- 温度与时序（P11~P12） --- */
    uint16_t temp_win_low_mv;   /* P11 TEMP 窗下限（默认 300mV，抓开路） */
    uint16_t temp_win_high_mv;  /* P11 TEMP 窗上限（默认 750mV，抓短路） */
    uint16_t drdy_timeout_ms;   /* P12 DRDY 超时（默认 3ms） */

    /* --- 通信与灯光（P13~P16） --- */
    uint16_t heartbeat_ms;      /* P13 心跳周期（默认 50ms，20Hz） */
    uint16_t comm_timeout_ms;   /* P14 通信超时（默认 200ms，4×心跳） */
    uint8_t  brightness;        /* P15 亮度（默认 60%） */
    uint8_t  team_color;        /* P16 队伍色（TEAM_COLOR_RED/BLUE） */

    /* --- ADC 与开关（P17~P20） --- */
    uint8_t  gain1[4];          /* P17 GAIN1 档位（默认全 1；1/2/4/8/16/32/64/128） */
    uint8_t  evt_enable;        /* P18 事件使能/CRC 开关（bit0 事件使能，bit1 CRC） */
    uint16_t temp_over_mv;      /* P19 超温阈值（默认 750mV=窗上限，等效不触发） */
    uint16_t osr;               /* P20 OSR 档位（默认 1024） */

    /* --- 缓冲与上报（P21/P22/P24） --- */
    uint8_t  ring_depth;        /* P21 环形缓冲深度（默认 32 帧） */
    uint16_t wave_window;       /* P22 触发后窗口（默认 32 样本=8.2ms） */
    uint16_t status_period_ms;  /* P24 状态上报周期（默认 1000ms，仲裁可调） */
} hit_param_t;

/* 取默认参数表（按设计文档 7.4）。 */
void Cal_GetDefaults(hit_param_t *p);

/* Flash 持久化（双槽 + CRC）。TODO(Step 5)：实现读写；骨架阶段仅返回默认值。 */
void Cal_Load(hit_param_t *p);
int  Cal_Save(const hit_param_t *p);

#endif /* APP_CALIBRATION_H */
