/**
 ******************************************************************************
 * @file    ads131m04.c
 * @brief   ADS131M04 驱动骨架（桩实现，函数签名已按设计文档 v1.5 第 5.3/6 章定型）。
 *
 * TODO(Step 1)：实现完整驱动——
 *   1) HAL_TIMEx_PWMN_Start(&htim16, TIM_CHANNEL_1) 启动 CLKIN 8MHz（4.1）；
 *   2) 初始化序列（6.3）：PC15 复位 ≥10µs → 等 DRDY 上升沿 → UNLOCK →
 *      全部可写寄存器显式写入（6.2 表）→ 回读逐字节校验 → LOCK → 丢弃 2 帧；
 *   3) 方案 A 读取链（6.4）：DRDY EXTI → 拉低 CS → HAL_SPI_TransmitReceive_DMA
 *      （TX=静态 NULL 缓冲/DMA1_Ch3，RX=K 帧环形缓冲槽/DMA1_Ch2）→ 完成中断
 *      拉高 CS、槽标记满；主循环按读指针消费；
 *   4) 帧解析：STATUS 健康位检查、ch1/ch3 按 ADC_CH_SIGN_MASK 取反、诊断计数。
 ******************************************************************************
 */
#include "bsp/ads131m04.h"
#include "board.h"
#include "main.h"

/* ---- 静态状态（骨架阶段仅占位，Step 1 填充） ---- */
static bool        s_frame_ready = false;
static ads_diag_t  s_diag = {0};
static uint8_t     s_sign_mask = ADC_CH_SIGN_MASK;

/* ==================== 公开接口 ==================== */

void ADS131M04_Init(void)
{
    /* TODO(Step 1)：完整初始化序列见文件头注释。骨架阶段仅复位 SYNC 引脚电平。 */
    HAL_GPIO_WritePin(ADC_SYNC_GPIO_Port, ADC_SYNC_Pin, GPIO_PIN_SET);
}

int ADS131M04_ReadFrame(ads_frame_t *f)
{
    (void)f;
    /* TODO(Step 1)：从环形缓冲取最新就绪帧；解析 STATUS 与 4 通道数据。 */
    s_frame_ready = false;
    return -1;   /* 骨架阶段无帧 */
}

int ADS131M04_RegWrite(uint8_t addr, uint16_t val)
{
    (void)addr; (void)val;
    /* TODO(Step 1)：WREG 帧 + RREG 回读校验（6.5）。 */
    return -1;
}

int ADS131M04_RegRead(uint8_t addr, uint16_t *val)
{
    (void)addr; (void)val;
    /* TODO(Step 1)：RREG 帧 + NULL 帧两拍。 */
    return -1;
}

int ADS131M04_RunSelfTest(void)
{
    /* TODO(Step 1/Step 5)：ID/全寄存器回读、DRDY 翻转、偏移与注入检查（7.6）。 */
    return -1;
}

void ADS131M04_SetChannelSign(uint8_t sign_mask)
{
    s_sign_mask = sign_mask & 0x0Fu;
    /* TODO(Step 1)：驱动层按掩码对 ch0-3 取反（6.7）。 */
}

void ADS131M04_GetDiag(ads_diag_t *d)
{
    if (d != NULL) { *d = s_diag; }
}

bool ADS131M04_IsFrameReady(void)
{
    return s_frame_ready;
}

/* ==================== 中断服务（由 weak 回调桥接） ==================== */

void ADS131M04_IrqDrdy(void)
{
    /* TODO(Step 1)：环形缓冲取空闲槽（满则 drop_cnt++）→ 拉低 CS →
     * 启动 HAL_SPI_TransmitReceive_DMA（TX 静态 NULL 18B / RX 槽 18B）。 */
    s_diag.drdy_cnt++;
}

void ADS131M04_IrqDmaDone(void)
{
    /* TODO(Step 1)：拉高 CS、槽标记满、置帧就绪标志。 */
    s_frame_ready = true;
}

/* ==================== weak 回调覆写（生成代码已备好 IRQ handler） ==================== */

/* EXTI15_10（PC14=ADC_NDRDY 下降沿）：it.c 已调用 HAL_GPIO_EXTI_IRQHandler(ADC_NDRDY_Pin) */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == ADC_NDRDY_Pin)
    {
        ADS131M04_IrqDrdy();
    }
}

/* SPI1 TX/RX DMA 完成：it.c 已备好 SPI1_IRQHandler 与 DMA1_Channel2/3_IRQHandler */
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI1)
    {
        ADS131M04_IrqDmaDone();
    }
}
