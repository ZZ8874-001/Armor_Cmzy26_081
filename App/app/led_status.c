/**
 ******************************************************************************
 * @file    led_status.c
 * @brief   灯光联动骨架（桩实现，灯效枚举已按设计文档 v1.5 第 8.2 节定型）。
 *
 * TODO(Step 3)：实现——
 *   1) 六灯效相位生成（5Hz/1Hz 闪烁、500ms/333ms 交替、常亮/灭）；
 *   2) RGB→WS2812 帧缓冲填充 + 亮度缩放（P15）→ Ws2812_Send（20Hz 刷新）；
 *   3) PB1（系统正常：NORMAL/HIT/ID_SETUP 亮）、PB0（超温：FAULT_TEMP 亮）联动。
 ******************************************************************************
 */
#include "app/led_status.h"
#include "board.h"

static led_effect_t s_effect    = LED_EFF_BOOT;
static uint8_t      s_team_color = 0u;

void LedStatus_Init(void)
{
    /* PB0/PB1 置 0 点亮、置 1 灭（灌电流，2.1 节）。初始灭。 */
    HAL_GPIO_WritePin(IND_ROHT_GPIO_Port, IND_ROHT_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(IND_NORM_GPIO_Port, IND_NORM_Pin, GPIO_PIN_SET);
}

void LedStatus_Tick(void)
{
    /* TODO(Step 3)：1kHz 相位推进；20Hz 分频在相位变化点重算帧并启动 DMA。 */
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
