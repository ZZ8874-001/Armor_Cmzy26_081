/**
 ******************************************************************************
 * @file    ws2812_uart.c
 * @brief   WS2812 灯带驱动（USART2 3.75MBd + TX 反相 + TX DMA）。
 *
 * 方案移植自已验证的发射机构工程（SHOOT_cmzy_REV081/Core/Src/ws2812_uart.c，
 * 2026-08-21）：
 *   - USART2 @ 3.75MBd（位宽 ≈267ns），**TX 反相（TXINV）**——空闲即低电平 = WS 复位码，
 *     帧间无需显式复位字节；
 *   - 每 WS 位 4 个 UART 位：'0'=[0,0,0,1]、'1'=[0,1,1,1]（线电平），每字节 2 个 WS 位；
 *   - 编码表 {0xEF, 0x8F, 0xEC, 0x8C}（含反相与 start/stop 位参与组帧）；
 *   - 颜色序 **GRB**（G→R→B），每颜色字节 4 字节 → 每灯 12 字节。
 * 之前自研的 2.667MBd 分数格方案实测混色乱序，弃用。
 ******************************************************************************
 */
#include "bsp/ws2812_uart.h"

#include <string.h>

/* 2 WS 位组 → 1 字节编码表（TXINV 反相后线电平：'0'=[0,0,0,1]，'1'=[0,1,1,1]） */
static const uint8_t s_ws_enc[4] =
{
    0xEFu, 0x8Fu, 0xECu, 0x8Cu    /* 00 01 10 11 */
};

static volatile bool s_busy = false;
static uint8_t  s_tx_buf[WS2812_FRAME_BYTES];   /* 内部发送缓冲（DMA 使用） */

void Ws2812_Init(void)
{
    s_busy = false;
    memset(s_tx_buf, 0, sizeof(s_tx_buf));

    /* TX 反相（空闲低电平 = WS 复位码）已由 CubeMX 配置
     * （USART2 Advanced Features → TX Pin Active Level Inverted，2026-08-21 定稿） */
}

/* 单灯 RGB → 12 字节编码（GRB 序，MSB 先行，每字节 2 个 WS 位） */
void Ws2812_EncodeLed(uint8_t dst[12], uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t  c[3];
    uint32_t i, j;

    c[0] = g; c[1] = r; c[2] = b;   /* GRB 序 */

    j = 0u;
    for (i = 0u; i < 3u; i++)
    {
        dst[j++] = s_ws_enc[(c[i] >> 6) & 0x03u];
        dst[j++] = s_ws_enc[(c[i] >> 4) & 0x03u];
        dst[j++] = s_ws_enc[(c[i] >> 2) & 0x03u];
        dst[j++] = s_ws_enc[(c[i] >> 0) & 0x03u];
    }
}

int Ws2812_Send(const uint8_t *frame)
{
    if (frame == NULL || s_busy)
    {
        return -1;
    }
    memcpy(s_tx_buf, frame, WS2812_FRAME_BYTES);
    s_busy = true;
    if (HAL_UART_Transmit_DMA(&huart2, s_tx_buf, WS2812_FRAME_BYTES) != HAL_OK)
    {
        s_busy = false;
        return -1;
    }
    return 0;
}

bool Ws2812_IsBusy(void)
{
    return s_busy;
}

void Ws2812_IrqTxDone(void)
{
    s_busy = false;
    /* TXINV：发送完成后线路回到空闲低电平 = WS 复位码，帧间隔 50ms ≫ 50µs 自然锁存 */
}

/* weak 回调桥接（it.c 已备好 USART2_IRQHandler；DMA1_Ch7 IRQ → HAL_UART_IRQHandler） */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)
    {
        Ws2812_IrqTxDone();
    }
}
