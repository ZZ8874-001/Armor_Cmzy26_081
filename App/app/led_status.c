/**
 ******************************************************************************
 * @file    led_status.c
 * @brief   灯光联动实现（官方 6 语义 → WS2812 + PB0/PB1，设计文档 v1.5 第 8.2 节）。
 *
 * 灯效（TIM2 1kHz 相位推进；20Hz 网格重建帧并经 DMA 发送）：
 *   NORMAL     队色常亮          HIT       队色 5Hz 快闪（亮灭各 100ms）
 *   ID_SETUP   队色 1Hz 慢闪      FAULT     按故障类别查图案表（Led_FaultRgb）
 *   COMM_LOST  紫色常亮          ID_CONFLICT 红蓝紫 333ms 循环
 *   BOOT       白色（300ms 后由状态机切走）
 * FAULT 图案表（2026-09-09 定稿，周期均为 50ms 整数倍）：
 *   传感器类(bit0-11) 官方红蓝 500ms 交替（粉/湖蓝）
 *   SPI/ADC(bit12)    琥珀 2Hz（250/250ms）
 *   PVD(bit13)        白色单闪 2Hz（亮 50ms 灭 450ms）
 *   TEMP 出窗(bit14)  青 5Hz（100/100ms）
 *   超温(bit15)       粉红常亮 + PB0 亮
 * PB0=超温指示（FAULT_OVER_TEMP 亮）、PB1=系统正常（NORMAL/HIT/ID_SETUP 亮）。
 * 高电平点亮（硬件实测确认 2026-08-21）。
 ******************************************************************************
 */
#include "app/led_status.h"
#include "board.h"
#include "bsp/ws2812_uart.h"
#include "detect/calibration.h"   /* TEAM_COLOR_RED/BLUE */
#include "app/faults.h"

#include <string.h>

/* 颜色定义（RGB；队伍色与发射机构 SHOOT_cmzy_REV081 对齐，2026-08-21） */
#define COL_RED     255u, 50u, 100u    /* 红队官方色=粉色（SHOOT COLOR_PINK：G=50 R=255 B=100） */
#define COL_BLUE    0u, 180u, 255u    /* 蓝队官方色=湖蓝（SHOOT COLOR_LAKE_BLUE：G=180 R=0 B=255） */
#define COL_PURPLE  128u, 0u, 128u
#define COL_WHITE   255u, 255u, 255u

/* 故障类别（用于 FAULT 灯效图案表；优先级 15>14>13>12>0-11） */
typedef enum
{
    LED_FLD_SENSOR = 0,   /* bit0-11 传感器读取值故障：官方红蓝交替 */
    LED_FLD_SPI,          /* bit12   采集链路（SPI/DRDY/ADC init）：琥珀 2Hz */
    LED_FLD_POWER,        /* bit13   PVD 欠压：白色单闪 2Hz */
    LED_FLD_TEMP,         /* bit14   TEMP 出窗：青 5Hz */
    LED_FLD_OVER_TEMP     /* bit15   超温：粉红常亮 + PB0 */
} led_fault_kind_t;

static volatile led_effect_t s_effect = LED_EFF_BOOT;
static volatile uint8_t      s_team_color = 0u;
static volatile uint32_t     s_phase_ms;            /* 1kHz 相位累计 */
static volatile uint8_t      s_brightness = 60u;    /* P15 */
static volatile uint16_t     s_fault_bits;          /* 20Hz 网格快照（帧图案与 PB0 一致） */
static uint8_t  s_frame[WS2812_FRAME_BYTES];        /* 完整发送帧（124B） */

/* 故障位图 → 类别（优先级：超温 > 温窗 > PVD > SPI > 传感器；无位默认官方图案） */
static led_fault_kind_t Led_FaultKind(uint16_t bits)
{
    if ((bits & FAULT_OVER_TEMP) != 0u) { return LED_FLD_OVER_TEMP; }
    if ((bits & FAULT_TEMP) != 0u)      { return LED_FLD_TEMP; }
    if ((bits & FAULT_POWER) != 0u)     { return LED_FLD_POWER; }
    if ((bits & FAULT_SPI) != 0u)       { return LED_FLD_SPI; }
    return LED_FLD_SENSOR;
}

/* FAULT 图案生成：返回 0=灭（周期均为 50ms 整数倍，20Hz 网格无量化） */
static uint8_t Led_FaultRgb(led_fault_kind_t kind, uint32_t p, uint8_t *r, uint8_t *g, uint8_t *b)
{
    switch (kind)
    {
    case LED_FLD_OVER_TEMP:                       /* 粉红常亮 + PB0 亮 */
        *r = 255u; *g = 50u; *b = 100u;
        return 1u;
    case LED_FLD_TEMP:                            /* 青 5Hz：周期 200ms，亮灭各 100ms */
        if ((p % 200u) < 100u) { *r = 0u; *g = 255u; *b = 255u; return 1u; }
        return 0u;
    case LED_FLD_POWER:                           /* 白色单闪 2Hz：周期 500ms，亮 50ms 灭 450ms */
        if ((p % 500u) < 50u) { *r = 255u; *g = 255u; *b = 255u; return 1u; }
        return 0u;
    case LED_FLD_SPI:                             /* 琥珀 2Hz：周期 500ms，亮灭各 250ms */
        if ((p % 500u) < 250u) { *r = 255u; *g = 140u; *b = 0u; return 1u; }
        return 0u;
    case LED_FLD_SENSOR:
    default:                                      /* 官方红蓝 500ms 交替（粉/湖蓝） */
        if ((p % 1000u) < 500u) { *r = 255u; *g = 50u;  *b = 100u; }
        else                    { *r = 0u;   *g = 180u; *b = 255u; }
        return 1u;
    }
}

/* 当前灯效的 RGB（返回 0=灭） */
static uint8_t Led_EffectRgb(uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint32_t p = s_phase_ms;
    uint8_t  on;

    switch (s_effect)
    {
    case LED_EFF_NORMAL:
        on = 1u;
        break;
    case LED_EFF_HIT:                       /* 5Hz：周期 200ms，亮灭各 100ms */
        on = ((p % 200u) < 100u) ? 1u : 0u;
        break;
    case LED_EFF_ID_SETUP:                  /* 1Hz：周期 1000ms，亮灭各 500ms */
        on = ((p % 1000u) < 500u) ? 1u : 0u;
        break;
    case LED_EFF_FAULT:                     /* 按故障类别查图案表（2026-09-09 定稿） */
        return Led_FaultRgb(Led_FaultKind(s_fault_bits), p, r, g, b);
    case LED_EFF_COMM_LOST:
        *r = 128u; *g = 0u; *b = 128u;
        return 1u;
    case LED_EFF_ID_CONFLICT:               /* 红蓝紫 333ms 循环（官方队伍色：粉/湖蓝/紫） */
        if ((p % 1000u) < 333u)          { *r = 255u; *g = 50u;  *b = 100u; }
        else if ((p % 1000u) < 666u)     { *r = 0u;   *g = 180u; *b = 255u; }
        else                             { *r = 128u; *g = 0u;   *b = 128u; }
        return 1u;
    case LED_EFF_BOOT:
    default:
        *r = 255u; *g = 255u; *b = 255u;
        return 1u;
    }

    if (on == 0u)
    {
        return 0u;                          /* 灭 */
    }
    if (s_team_color == TEAM_COLOR_BLUE) { *r = 0u; *g = 0u; *b = 255u; }
    else                                 { *r = 255u; *g = 0u; *b = 0u; }
    return 1u;
}

/* 重建帧（20Hz 网格调用）：13 灯同色（当前版本无独立灯控），亮度缩放，尾部复位码 */
static void Led_RebuildFrame(void)
{
    uint8_t r = 0u, g = 0u, b = 0u;
    uint8_t lr, lg, lb;
    uint32_t i;

    if (Led_EffectRgb(&r, &g, &b) == 0u)
    {
        lr = 0u; lg = 0u; lb = 0u;
    }
    else
    {
        lr = (uint8_t)((uint32_t)r * s_brightness / 100u);
        lg = (uint8_t)((uint32_t)g * s_brightness / 100u);
        lb = (uint8_t)((uint32_t)b * s_brightness / 100u);
    }

    for (i = 0u; i < LED_COUNT; i++)
    {
        Ws2812_EncodeLed(&s_frame[i * 12u], lr, lg, lb);
    }
}

void LedStatus_Init(void)
{
    /* PB0/PB1 高电平点亮、低电平灭（硬件实测确认）。初始灭。 */
    HAL_GPIO_WritePin(IND_ROHT_GPIO_Port, IND_ROHT_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(IND_NORM_GPIO_Port, IND_NORM_Pin, GPIO_PIN_RESET);

    Ws2812_Init();
    memset(s_frame, 0, sizeof(s_frame));
    s_phase_ms = 0u;
}

void LedStatus_Tick(void)
{
    s_phase_ms++;

    /* 20Hz 网格：重建帧并发送（50ms 周期；DMA 忙则跳过本次） */
    if ((s_phase_ms % 50u) == 0u)
    {
        /* 故障位图快照（单次原子读）：帧图案与 PB0 一致，最坏 50ms 显示滞后 */
        s_fault_bits = Fault_GetBitmap();

        Led_RebuildFrame();
        (void)Ws2812_Send(s_frame);

        /* PB1：系统正常指示（NORMAL/HIT/ID_SETUP 亮） */
        if (s_effect == LED_EFF_NORMAL || s_effect == LED_EFF_HIT || s_effect == LED_EFF_ID_SETUP)
        {
            HAL_GPIO_WritePin(IND_NORM_GPIO_Port, IND_NORM_Pin, GPIO_PIN_SET);
        }
        else
        {
            HAL_GPIO_WritePin(IND_NORM_GPIO_Port, IND_NORM_Pin, GPIO_PIN_RESET);
        }
        /* PB0：超温指示（FAULT_OVER_TEMP 亮，任何状态有效） */
        HAL_GPIO_WritePin(IND_ROHT_GPIO_Port, IND_ROHT_Pin,
                          ((s_fault_bits & FAULT_OVER_TEMP) != 0u) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
}

void LedStatus_SetEffect(led_effect_t effect)
{
    if (effect < LED_EFF_COUNT)
    {
        s_effect = effect;
    }
}

void LedStatus_SetTeamColor(uint8_t team_color)
{
    s_team_color = team_color & 0x01u;
}

void LedStatus_SetBrightness(uint8_t brightness)
{
    s_brightness = (brightness > 100u) ? 100u : brightness;
}
