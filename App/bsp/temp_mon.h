/**
 ******************************************************************************
 * @file    temp_mon.h
 * @brief   基准温度监测接口（ADC1 4 通道扫描 + TIM1 CC1 硬件触发）。
 *
 * 设计文档 v1.5：第 2.1 节（TMP_REF1~4）、4 章配置表 ADC1/TIM1 行、
 * 7.5 节（TEMP 窗检与超温，保护阈值默认最宽松 P19=0.75V）。
 ******************************************************************************
 */
#ifndef APP_TEMP_MON_H
#define APP_TEMP_MON_H

#include <stdint.h>

/* 故障位（与 HitDetect 故障位图合并上报，7.5） */
#define FAULT_TEMP        (1u << 0)   /* TEMP 出窗（开路/短路） */
#define FAULT_OVER_TEMP   (1u << 1)   /* 超温（>P19，默认等效不触发） */

/* 初始化：启动 ADC1（TIM1 CC1 触发 + 4 通道扫描由 CubeMX 已配置）。 */
void TempMon_Init(void);

/* 20Hz 节拍（TIM2 分频调用）：读取最近转换结果、窗检与超温判定。 */
void TempMon_Tick(void);

/* 当前故障位图。 */
uint16_t TempMon_GetFaultFlags(void);

/* 最近 4 路 TEMP 电压（mV，0.01V 单位 ×10：即 0.5V → 500），供调试/上报。 */
void TempMon_GetLastMv(uint16_t mv[4]);

#endif /* APP_TEMP_MON_H */
