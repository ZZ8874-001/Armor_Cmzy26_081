/**
 ******************************************************************************
 * @file    hit_detect.c
 * @brief   击打检测管线骨架（桩实现，接口已按设计文档 v1.5 第 7 章定型）。
 *
 * TODO(Step 2)：实现——
 *   1) 每通道 EMA 基线（bl += (raw-bl)>>S_BL，τ≈2.1s）与高通 hp；
 *   2) 融合：hit_sum = Σhp(ch)，32 样本窗口峰值 + 持续≥4 样本判定；
 *   3) 50ms 不应期；事件填充（合力/力度/四通道峰值分布/时间戳）；
 *   4) 20Hz 健康评估（7.5 六类故障判据，迟滞 + 闩存）；
 *   5) 极性校对与波形捕获接口（6.7/7.8）。
 ******************************************************************************
 */
#include "detect/hit_detect.h"

#include <stddef.h>

static hit_param_t s_param;
static uint16_t    s_fault_flags = 0u;

void HitDetect_Init(const hit_param_t *p)
{
    HitDetect_UpdateParams(p);
    s_fault_flags = 0u;
}

void HitDetect_Feed(const ads_frame_t *f)
{
    (void)f;
    /* TODO(Step 2)：检测管线（见文件头注释）。 */
}

bool HitDetect_GetEvent(hit_event_t *e)
{
    (void)e;
    /* TODO(Step 2)：返回最近触发的事件。 */
    return false;
}

void HitDetect_UpdateParams(const hit_param_t *p)
{
    if (p != NULL) { s_param = *p; }
}

uint16_t HitDetect_GetFaultFlags(void)
{
    return s_fault_flags;
}

int HitDetect_CalibratePolarity(void)
{
    /* TODO(Step 2/M2)：一次已知方向击打（或 DAC 注入）校验 4 通道符号 → 写符号掩码入 Flash。 */
    return -1;
}

int HitDetect_GetWaveform(uint8_t ch, ads_sample_t *buf, uint16_t *n)
{
    (void)ch; (void)buf; (void)n;
    /* TODO(Step 2)：环形窗口冻结 + 拷贝输出（7.8）。 */
    return -1;
}

int HitDetect_EstimateImpact(const hit_event_t *e, impact_info_t *info)
{
    (void)e; (void)info;
    /* TODO(Step 6/M6)：Impulse = K_N·Δt·Σ|hp|；v ≈ Impulse/(m(1+e))；入射角建模。 */
    return -1;
}

int HitDetect_EstimatePosition(const hit_event_t *e, pos_info_t *pos)
{
    (void)e; (void)pos;
    /* TODO(Step 6/M6)：x ∝ ((P2+P3)−(P1+P4))/ΣP 等（7.8）。 */
    return -1;
}
