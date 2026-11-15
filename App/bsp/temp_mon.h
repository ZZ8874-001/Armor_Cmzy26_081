/**
 ******************************************************************************
 * @file    temp_mon.h
 * @brief   基准温度监测接口（ADC1 4 通道扫描 + TIM1 CC1 硬件触发 + DMA circular）。
 *
 * 设计文档 v1.5：第 2.1 节（TMP_REF1~4）、4 章配置表 ADC1/TIM1 行、
 * 7.5 节（TEMP 窗检与超温，保护阈值默认最宽松 P19=0.75V）。
 * 触发链前提：.ioc 已配置 ADC1 DMA1_Ch1 Circular + DMA Continuous Requests、
 * TIM1 CH1 Pulse>0（TRGO=OC1 才会翻转），重生成后生效。
 ******************************************************************************
 */
#ifndef APP_TEMP_MON_H
#define APP_TEMP_MON_H

#include <stdint.h>
#include <stdbool.h>
#include "detect/calibration.h"   /* hit_param_t */
#include "app/faults.h"           /* 统一故障位图（FAULT_TEMP/FAULT_OVER_TEMP = bit14/15） */

/* 初始化：拷贝阈值，启动 TIM1（TRGO=OC1）与 ADC1 DMA circular。 */
void TempMon_Init(const hit_param_t *p);

/* 运行时更新阈值（P11/P19）。 */
void TempMon_UpdateParams(const hit_param_t *p);

/* 20Hz 节拍（TIM2 分频调用）：读取 DMA 最近转换、mV 换算、窗检/超温判定
 * （100 拍=5s 双向去抖）并发布到故障注册表（FAULT_MASK_TEMPMON）。 */
void TempMon_Tick(void);

/* 去抖后故障位（FAULT_TEMP/FAULT_OVER_TEMP；注册表已含，此处供调试观察）。 */
uint16_t TempMon_GetFaultFlags(void);

/* 最近 4 路 TEMP 电压（mV，0.01V 单位 ×10：即 0.5V → 500），供调试/上报。 */
void TempMon_GetLastMv(uint16_t mv[4]);

/* 采集链就绪（DMA 启动且收到过有效转换；未就绪时不评估故障，避免误报）。 */
bool TempMon_IsReady(void);

#endif /* APP_TEMP_MON_H */
