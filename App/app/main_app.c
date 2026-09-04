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

/* ---- 诊断：1s 速率快照（调试器直接 watch，检查漏跑情况） ---- */
static volatile uint32_t s_rate_drdy;     /* 每秒 DRDY 中断数（期望 ≈3906） */
static volatile uint32_t s_rate_frames;   /* 每秒消费帧数（应 ≈s_rate_drdy） */
static volatile uint32_t s_rate_spi_err;  /* 每秒 SPI 错误增量 */
static volatile uint32_t s_rate_drop;     /* 每秒跳帧增量 */
static uint32_t s_last_drdy;
static uint32_t s_last_frames;
static uint32_t s_last_spi_err;
static uint32_t s_last_drop;

/* 最近一帧解析结果（调试器实时观察 ADC 数据用，每 256µs 更新一次） */
static volatile ads_frame_t s_last_frame;

void App_Init(void)
{
    /* 参数：骨架阶段用默认值；TODO(Step 5)：Cal_Load 从 Flash 恢复标定值。 */
    Cal_GetDefaults(&s_param);

    App_Log_Init();
    App_Log_Printf("\r\nApp skeleton OK (v0.1)\r\n");

    App_Can_Init();            /* CAN 启动 + 通知使能（Step 4 补齐发送/分发） */
    ADS131M04_Init();          /* Step 1：复位 + 全寄存器写入 + 回读校验 + 方案 A 读取链 */
    ADS131M04_RunSelfTest();   /* Step 1：M1 验证（回读值/DRDY 频率/噪声 RMS 打印）——TODO(Step 2) 并入统一自检流程 */
    TempMon_Init();            /* TODO(Step 5) */
    Ws2812_Init();             /* TODO(Step 3)：发送实现 */
    HitDetect_Init(&s_param);  /* TODO(Step 2)：检测管线 */
    LedStatus_Init();          /* TODO(Step 3)：灯效生成 */
    StateMachine_Init();       /* 骨架：BOOT→NORMAL 占位 */
    BoardComm_Init();          /* TODO(Step 4)：协议栈实例化 */

    /* 启动 1kHz 系统时基（TIM2 更新中断）——CubeMX 只生成 Init 不 Start，必须显式启动；
     * 放在全部模块初始化之后，避免首个 tick 命中半初始化模块。 */
    HAL_TIM_Base_Start_IT(&htim2);
}

void App_Loop(void)
{
    /* --- 帧消费 + 检测管线（Step 1/2 起启用） --- */
    if (ADS131M04_IsFrameReady())
    {
        ads_frame_t frame;
        if (ADS131M04_ReadFrame(&frame) == 0)
        {
            s_last_frame = frame;   /* 调试观察点（Ozone 2Hz 刷新即可看到流动数据） */
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

    /* 诊断：每秒快照 DRDY 速率/错误增量/帧消费速率（Step 1 验证用） */
    if ((s_tick_ms % 1000u) == 0u)
    {
        ads_diag_t d;
        ADS131M04_GetDiag(&d);
        s_rate_drdy    = d.drdy_cnt - s_last_drdy;
        s_last_drdy    = d.drdy_cnt;
        s_rate_frames  = d.frames_read - s_last_frames;
        s_last_frames  = d.frames_read;
        s_rate_spi_err = d.spi_err - s_last_spi_err;
        s_last_spi_err = d.spi_err;
        s_rate_drop    = d.drop_cnt - s_last_drop;
        s_last_drop    = d.drop_cnt;
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
