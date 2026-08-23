/**
 ******************************************************************************
 * @file    ws2812_uart.c
 * @brief   WS2812 灯带驱动骨架（桩实现，接口已按设计文档 v1.5 第 8.1 节定型）。
 *
 * TODO(Step 3)：实现——
 *   1) 3bit→字节编码表（110=1 码 / 100=0 码）与 RGB→帧缓冲填充；
 *   2) Ws2812_Send：HAL_UART_Transmit_DMA(&huart2, frame, WS2812_FRAME_BYTES)；
 *   3) 亮度缩放（v×B/100）与 20Hz 帧刷新节拍（由 LedStatus 调用）。
 ******************************************************************************
 */
#include "bsp/ws2812_uart.h"
#include "main.h"

static bool s_busy = false;

void Ws2812_Init(void)
{
    /* huart2（2.6667MBd TX-only）与 hdma_usart2_tx 已由 CubeMX 初始化，
     * PA2 已为推挽 AF_PP。骨架阶段无额外动作。 */
    s_busy = false;
}

int Ws2812_Send(const uint8_t *frame)
{
    (void)frame;
    /* TODO(Step 3)：忙检查 → HAL_UART_Transmit_DMA。 */
    if (s_busy) { return -1; }
    s_busy = true;
    return 0;
}

bool Ws2812_IsBusy(void)
{
    return s_busy;
}

void Ws2812_IrqTxDone(void)
{
    /* TODO(Step 3)：置可发送标志。 */
    s_busy = false;
}

/* USART2 TX DMA 完成：it.c 已备好 USART2_IRQHandler + DMA1_Channel7_IRQHandler */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)
    {
        Ws2812_IrqTxDone();
    }
}
