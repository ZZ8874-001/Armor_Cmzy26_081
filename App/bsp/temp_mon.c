/**
 ******************************************************************************
 * @file    temp_mon.c
 * @brief   基准温度监测实现（2026-09-09 实装；设计文档 v1.5 第 2.2/7.5/10.2 节）。
 *
 * - ADC1 4 通道扫描（CH6/10/11/12 = TMP_REF1~4），TIM1 CC1 TRGO 硬件触发
 *   ≈1.22kHz，DMA1_Ch1 circular 半字缓冲持续刷新（.ioc：ADC1 DMA Circular +
 *   DMA Continuous Requests，TIM1 Pulse>0——触发链实际生效的前提）；
 * - TempMon_Tick（TIM2 50ms 分频，20Hz）：拷贝最近 4 路原始值 → mV 换算
 *   （VDDA 3.3V 基准、12bit：mV = raw×3300/4096）→ P11 窗检 / P19 超温 →
 *   100 拍（5s）双向去抖 → Fault_UpdateLive(FAULT_MASK_TEMPMON, ...)；
 * - 全零守卫：转换链未跑（.ioc 未重生成 / TIM1 未启动 / DMA 启动失败）时
 *   原始值恒 0，判 not-ready 并清故障——避免触发链故障被误报成 TEMP 出窗。
 ******************************************************************************
 */
#include "bsp/temp_mon.h"
#include "board.h"
#include "app/faults.h"
#include "detect/calibration.h"

/* 去抖拍数：100 拍 × 50ms = 5s（7.5"持续 5s"，与 HitDetect 5s 重试节奏一致） */
#define TEMP_DEBOUNCE_TICKS  100u

/* VDDA 基准 3.3V，12bit：raw→mV 舍入常数 */
#define TEMP_MV_MAX          3300u
#define TEMP_RAW_MAX         4096u

static volatile uint16_t s_temp_raw[4];   /* DMA circular 目标（半字 ×4） */
static uint16_t s_last_mv[4]  = {0u, 0u, 0u, 0u};
static uint16_t s_faults      = 0u;        /* 去抖后故障位（FAULT_TEMP/FAULT_OVER_TEMP） */
static uint8_t  s_ready       = 0u;        /* 收到过有效（非全零）转换 */
static uint16_t s_bad_cnt     = 0u;        /* 连续坏拍计数（置位去抖） */
static uint16_t s_good_cnt    = 0u;        /* 连续好拍计数（清零去抖） */
static hit_param_t s_p;                    /* 阈值副本（P11/P19） */

void TempMon_Init(const hit_param_t *p)
{
    uint32_t i;

    if (p != NULL) { s_p = *p; }
    else           { Cal_GetDefaults(&s_p); }

    for (i = 0u; i < 4u; i++)
    {
        s_temp_raw[i] = 0u;
        s_last_mv[i]  = 0u;
    }
    s_faults   = 0u;
    s_ready    = 0u;
    s_bad_cnt  = 0u;
    s_good_cnt = 0u;

    /* 触发链启动：TIM1（TRGO=OC1，Pulse 由 .ioc 配置 >0）→ ADC1 DMA circular。
     * 任一启动失败保持 not-ready（Tick 侧全零守卫不评估故障）。
     * hadc1.DMA_Handle 在 .ioc 配置 ADC1 DMA 并重生成后有效。 */
    if (HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1) != HAL_OK)
    {
        return;
    }
    if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)s_temp_raw, 4u) != HAL_OK)
    {
        return;
    }
}

void TempMon_UpdateParams(const hit_param_t *p)
{
    if (p != NULL)
    {
        s_p = *p;
    }
}

void TempMon_Tick(void)
{
    uint32_t i;
    uint8_t  bad = 0u;
    uint8_t  any_nonzero = 0u;

    for (i = 0u; i < 4u; i++)
    {
        uint16_t raw = s_temp_raw[i];
        uint16_t mv  = (uint16_t)(((uint32_t)raw * TEMP_MV_MAX + TEMP_RAW_MAX / 2u) / TEMP_RAW_MAX);
        s_last_mv[i] = mv;
        if (raw != 0u)
        {
            any_nonzero = 1u;
        }
        if (mv < s_p.temp_win_low_mv || mv > s_p.temp_win_high_mv)
        {
            bad = 1u;
        }
        if (mv > s_p.temp_over_mv)
        {
            bad = 1u;
        }
    }

    if (any_nonzero == 0u)
    {
        /* 转换链未跑：not-ready + 清故障，避免触发链故障误报 TEMP 出窗 */
        s_ready    = 0u;
        s_bad_cnt  = 0u;
        s_good_cnt = 0u;
        s_faults   = 0u;
        Fault_UpdateLive(FAULT_MASK_TEMPMON, 0u);
        return;
    }
    s_ready = 1u;

    /* 100 拍（5s）双向去抖：连续 100 拍坏 → 置位；连续 100 拍好 → 清零 */
    if (bad != 0u)
    {
        s_good_cnt = 0u;
        if (s_bad_cnt < TEMP_DEBOUNCE_TICKS)
        {
            s_bad_cnt++;
            if (s_bad_cnt >= TEMP_DEBOUNCE_TICKS)
            {
                s_faults = 0u;
                for (i = 0u; i < 4u; i++)
                {
                    if (s_last_mv[i] > s_p.temp_over_mv)
                    {
                        s_faults |= FAULT_OVER_TEMP;
                    }
                    else if (s_last_mv[i] < s_p.temp_win_low_mv ||
                             s_last_mv[i] > s_p.temp_win_high_mv)
                    {
                        s_faults |= FAULT_TEMP;
                    }
                }
            }
        }
    }
    else
    {
        s_bad_cnt = 0u;
        if (s_good_cnt < TEMP_DEBOUNCE_TICKS)
        {
            s_good_cnt++;
            if (s_good_cnt >= TEMP_DEBOUNCE_TICKS)
            {
                s_faults = 0u;
            }
        }
    }

    Fault_UpdateLive(FAULT_MASK_TEMPMON, s_faults);
}

uint16_t TempMon_GetFaultFlags(void)
{
    return s_faults;
}

void TempMon_GetLastMv(uint16_t mv[4])
{
    uint32_t i;
    for (i = 0u; i < 4u; i++) { mv[i] = s_last_mv[i]; }
}

bool TempMon_IsReady(void)
{
    return s_ready != 0u;
}
