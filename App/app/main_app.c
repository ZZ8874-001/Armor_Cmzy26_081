/**
 ******************************************************************************
 * @file    main_app.c
 * @brief   应用层编排（设计文档 v1.5 第 5.3/10 章）。
 *
 * 骨架阶段自证：TIM2 1kHz tick 驱动 PB1 1Hz 闪烁（tick 链路存活证明），
 * 上电经 USART1 打印 "App skeleton OK"。各模块桩按步骤填充：
 *   Step 1 ADS131M04 / Step 2 检测 / Step 3 灯 / Step 4 通信 / Step 5 收尾。
 ******************************************************************************
 */
#include "app/main_app.h"

#include "board.h"
#include "bsp/ads131m04.h"
#include "bsp/temp_mon.h"
#include "bsp/ws2812_uart.h"
#include "detect/calibration.h"
#include "detect/hit_detect.h"
#include "comm/app_can.h"
#include "comm/app_log.h"
#include "comm/board_comm.h"
#include "app/state_machine.h"
#include "app/led_status.h"

/* ---- 运行参数（Step 5 起从 Flash 载入标定值） ---- */
static hit_param_t s_param;
static uint32_t    s_tick_ms = 0u;

void App_Init(void)
{
    /* 参数：骨架阶段用默认值；TODO(Step 5)：Cal_Load 从 Flash 恢复标定值。 */
    Cal_GetDefaults(&s_param);

    App_Log_Init();
    App_Log_Printf("\r\nApp skeleton OK (v0.1)\r\n");

    App_Can_Init();            /* CAN 启动 + 通知使能（Step 4 补齐发送/分发） */
    ADS131M04_Init();          /* TODO(Step 1)：完整初始化序列 */
    TempMon_Init();            /* TODO(Step 5) */
    Ws2812_Init();             /* TODO(Step 3)：发送实现 */
    HitDetect_Init(&s_param);  /* TODO(Step 2)：检测管线 */
    LedStatus_Init();          /* TODO(Step 3)：灯效生成 */
    StateMachine_Init();       /* 骨架：BOOT→NORMAL 占位 */
    BoardComm_Init();          /* TODO(Step 4)：协议栈实例化 */
}

void App_Loop(void)
{
    /* --- 帧消费 + 检测管线（Step 1/2 起启用） --- */
    if (ADS131M04_IsFrameReady())
    {
        ads_frame_t frame;
        if (ADS131M04_ReadFrame(&frame) == 0)
        {
            HitDetect_Feed(&frame);
        }
    }

    /* TODO(Step 4)：Transport_Isotp_Poll + Service_RetryAckScheduler_Poll + 心跳/状态入队 */
    BoardComm_Loop();
}

void App_OnTick1ms(void)
{
    s_tick_ms++;

    StateMachine_Tick();    /* 1kHz 状态机步进（第 11 章） */
    LedStatus_Tick();       /* 1kHz 灯效相位推进（20Hz 帧刷新） */

    if ((s_tick_ms % 50u) == 0u)
    {
        TempMon_Tick();     /* 20Hz 健康评估 + TEMP 窗检（7.5） */
    }

    /* 骨架自证：PB1 1Hz 闪烁证明 tick 链路通。
     * TODO(Step 3)：改为 LedStatus/状态机驱动（PB1=系统正常指示）。 */
    if ((s_tick_ms % 500u) == 0u)
    {
        HAL_GPIO_TogglePin(IND_NORM_GPIO_Port, IND_NORM_Pin);
    }
}

/* ==================== weak 回调覆写（it.c 已备好 TIM2_IRQHandler） ==================== */

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM2)
    {
        App_OnTick1ms();
    }
}
