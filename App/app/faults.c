/**
 ******************************************************************************
 * @file    faults.c
 * @brief   统一故障注册表实现 + PVD 欠压中断（FAULT_POWER）。
 *
 * - live：周期评估故障（HitDetect/TempMon 20Hz 发布，带各自的 5s 级去抖/重试）；
 * - sticky：事件型故障（PVD 欠压、ADC 初始化失败），仅自检通过或复位清除；
 * - PVD：HAL_MspInit 只做了 HAL_PWR_EnablePVD（NORMAL 模式），EXTI16 触发沿与
 *   NVIC 由本模块补齐——HAL_PWR_ConfigPVD 幂等，PWR_PVD_MODE_IT_RISING 会配置
 *   并挂接 EXTI16；startup 的 PVD_PVM_IRQHandler 弱别名在本文件被强定义接管。
 ******************************************************************************
 */
#include "app/faults.h"
#include "board.h"
#include "stm32l4xx_hal_pwr_ex.h"   /* HAL_PWREx_PVD_PVM_IRQHandler（PVD 中断入口） */

/* PVD 阈值（HAL 头注释 level 6 ≈2.9V）。测试期如误触发可放宽为
 * PWR_PVDLEVEL_0（≈2.0V，设计文档 1.2 保护阈值默认最宽松原则）。 */
#define FAULT_PVD_LEVEL   PWR_PVDLEVEL_6

static volatile uint16_t s_live;
static volatile uint16_t s_sticky;

void Fault_Init(void)
{
    PWR_PVDTypeDef cfg;

    s_live   = 0u;
    s_sticky = 0u;

    /* PVD：IT_RISING 挂 EXTI16 + NVIC（优先级 0，与 DRDY EXTI 同级；事件极稀疏） */
    cfg.PVDLevel = FAULT_PVD_LEVEL;
    cfg.Mode     = PWR_PVD_MODE_IT_RISING;
    HAL_PWR_ConfigPVD(&cfg);
    HAL_PWR_EnablePVD();
    HAL_NVIC_SetPriority(PVD_PVM_IRQn, 0u, 0u);
    HAL_NVIC_EnableIRQ(PVD_PVM_IRQn);
}

uint16_t Fault_GetBitmap(void)
{
    return (uint16_t)(s_live | s_sticky);
}

uint16_t Fault_GetLive(void)
{
    return (uint16_t)s_live;
}

uint16_t Fault_GetSticky(void)
{
    return (uint16_t)s_sticky;
}

void Fault_UpdateLive(uint16_t mask, uint16_t value)
{
    s_live = (uint16_t)((s_live & ~mask) | (value & mask));
}

void Fault_SetSticky(uint16_t bits)
{
    __disable_irq();
    s_sticky = (uint16_t)(s_sticky | bits);
    __enable_irq();
}

void Fault_ClearSticky(uint16_t bits)
{
    __disable_irq();
    s_sticky = (uint16_t)(s_sticky & ~bits);
    __enable_irq();
}

/* ==================== PVD 中断（FAULT_POWER 通路） ==================== */

void PVD_PVM_IRQHandler(void)
{
    HAL_PWREx_PVD_PVM_IRQHandler();
}

/* HAL_PWR_PVD_Callback：欠压事件 → sticky 闩存。
 * 只置位不打印（ISR 内阻塞打印会延迟同优先级 DRDY，导致跳帧）；
 * 事件经状态上报沿即时可见。 */
void HAL_PWR_PVD_Callback(void)
{
    Fault_SetSticky(FAULT_POWER);
}
