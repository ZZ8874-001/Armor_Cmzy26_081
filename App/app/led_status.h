/**
 ******************************************************************************
 * @file    led_status.h
 * @brief   灯光联动接口（官方 6 语义 → WS2812 + PB0/PB1，设计文档 v1.5 第 8.2 节）。
 *
 * 灯效频率：快闪 5Hz（亮灭各 100ms）、慢闪 1Hz（亮灭各 500ms）、
 * FAULT 红蓝 500ms 交替、ID_CONFLICT 红蓝紫 333ms 循环；帧刷新 20Hz。
 * PB0=超温指示、PB1=系统正常指示（NORMAL/HIT/ID_SETUP 亮）。
 ******************************************************************************
 */
#ifndef APP_LED_STATUS_H
#define APP_LED_STATUS_H

#include <stdint.h>

typedef enum
{
    LED_EFF_NORMAL = 0,     /* 队色常亮 */
    LED_EFF_HIT,            /* 队色 5Hz 快闪 */
    LED_EFF_ID_SETUP,       /* 队色 1Hz 慢闪 */
    LED_EFF_FAULT,          /* 红蓝 500ms 交替 */
    LED_EFF_COMM_LOST,      /* 紫色常亮 */
    LED_EFF_ID_CONFLICT,    /* 红蓝紫 333ms 循环 */
    LED_EFF_BOOT,           /* 白色 300ms */
    LED_EFF_COUNT
} led_effect_t;

/* 初始化：WS2812 + PB0/PB1 初始状态。 */
void LedStatus_Init(void);

/* 1kHz 相位推进（TIM2 中断调用）；20Hz 分频触发帧刷新（仅相位变化点重算）。 */
void LedStatus_Tick(void);

/* 切换灯效（状态机/下行命令调用）。 */
void LedStatus_SetEffect(led_effect_t effect);

/* 队伍色设置（P16，ID 设置时确定）。 */
void LedStatus_SetTeamColor(uint8_t team_color);

#endif /* APP_LED_STATUS_H */
