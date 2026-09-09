/**
 ******************************************************************************
 * @file    faults.h
 * @brief   统一故障位图与故障注册表接口（状态机/灯/上报的唯一真相源）。
 *
 * 设计文档 v1.5 第 7.5 节为故障判据权威来源；位布局（2026-09-09 定稿）：
 *   bit0-11   传感器类：CH0~3 饱和/偏置/死通道（live，5s 自动重试）
 *   bit12     FAULT_SPI：SPI 错误 / DRDY 停滞(>P12) / ADC 初始化失败（sticky）
 *   bit13     FAULT_POWER：PVD 欠压事件（sticky，仅自检通过或复位清除）
 *   bit14     FAULT_TEMP：TEMP 出 P11 窗（live，5s 去抖）
 *   bit15     FAULT_OVER_TEMP：TEMP > P19 超温（live，5s 去抖，联动 PB0）
 * TLV_FAULT_FLAGS（0x35）按 U16 上报，位布局即本表；不再使用 FAULT_DAC。
 ******************************************************************************
 */
#ifndef APP_FAULTS_H
#define APP_FAULTS_H

#include <stdint.h>

/* ---- 故障位（7.5） ---- */
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
#define FAULT_SPI         (1u << 12)   /* SPI 错误 / DRDY 停滞 / ADC 初始化失败 */
#define FAULT_POWER       (1u << 13)   /* PVD 欠压（sticky） */
#define FAULT_TEMP        (1u << 14)   /* TEMP 出窗（开路/短路） */
#define FAULT_OVER_TEMP   (1u << 15)   /* 超温（>P19） */

/* ---- 类别掩码 ---- */
#define FAULT_MASK_SENSOR     0x0FFFu   /* bit0-11 传感器类（官方红蓝交替灯效） */
#define FAULT_MASK_HITDETECT  0x1FFFu   /* 传感器 + SPI：HitDetect 发布域 */
#define FAULT_MASK_TEMPMON    0xC000u   /* TEMP + OVER_TEMP：TempMon 发布域 */

/* 初始化：清零注册表 + PVD 配置（IT_RISING + NVIC 使能，FAULT_POWER 通路）。 */
void Fault_Init(void);

/* 合并位图（live|sticky）。16 位读取原子，任何上下文可读。 */
uint16_t Fault_GetBitmap(void);
uint16_t Fault_GetLive(void);
uint16_t Fault_GetSticky(void);

/* live 域子集替换。调用方（HitDetect_Tick/TempMon_Tick）在同一 TIM2 ISR 内
 * 顺序执行，主循环读者为 16 位原子读，无需临界区。 */
void Fault_UpdateLive(uint16_t mask, uint16_t value);

/* sticky 域置位/清除（PVD ISR 与主循环自检均可调用，内部临界区保护）。 */
void Fault_SetSticky(uint16_t bits);
void Fault_ClearSticky(uint16_t bits);

#endif /* APP_FAULTS_H */
