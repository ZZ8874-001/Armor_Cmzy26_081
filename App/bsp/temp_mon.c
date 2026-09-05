/**
 ******************************************************************************
 * @file    temp_mon.c
 * @brief   基准温度监测骨架（桩实现）。
 *
 * TODO(Step 5)：实现——
 *   1) TempMon_Init：HAL_ADC_Start(&hadc1)（TIM1 CC1 触发每次完成 4 通道序列）；
 *   2) TempMon_Tick（20Hz）：取最近一次 4 通道转换结果（轮询 EOC 或补开 ADC 中断），
 *      换算 mV，按 P11 窗检（300~750mV）与 P19 超温（默认 750mV 等效不触发）置故障位；
 *   3) 超温联动 PB0（IND_ROHT）点亮（第 8.2/11 章）。
 ******************************************************************************
 */
#include "bsp/temp_mon.h"
#include "board.h"

static uint16_t s_fault_flags = 0u;
static uint16_t s_last_mv[4]  = {0u, 0u, 0u, 0u};

void TempMon_Init(void)
{
    /* TODO(Step 5)：HAL_ADC_Start(&hadc1)。 */
}

void TempMon_Tick(void)
{
    /* TODO(Step 5)：读转换结果 → 窗检/超温判定 → s_fault_flags。 */
}

uint16_t TempMon_GetFaultFlags(void)
{
    return s_fault_flags;
}

void TempMon_GetLastMv(uint16_t mv[4])
{
    for (int i = 0; i < 4; i++) { mv[i] = s_last_mv[i]; }
}
