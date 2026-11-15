/**
 ******************************************************************************
 * @file    host_cmd.c
 * @brief   上位机 USART1 下行命令通道（HCMD 帧）。
 *
 * 物理层：PA10/USART1 RX @921600，RX DMA（DMA1_Ch5，NORMAL，1024B，.ioc
 * 已配）零接收中断；字节由 DMA 后台写入缓冲，主循环读计数器消费。帧格式
 * 权威：Docs/USART1波形数据帧格式.md v1.1 HCMD 节。
 *
 * 并发（R17 简化）：缓冲只由 DMA 写、主循环读，无 ISR 参与接收；
 * 协议解析/参数应用/ACK 全部在主循环。
 *
 * Flash 写（SAVE_PARAMS / SET_REFERENCE）擦+写 ~31ms 停取指：锁帧确认中或
 * 自检运行中拒绝执行；DMA 失联（被中止/缓冲满完成）由主循环看门狗重启。
 *
 * ACK 走 App_Log_Printf 文本（上位机日志面板天然可见；WAV1 DMA 在飞时自动排队）。
 *
 * 分层：comm→app 依赖是沿用 board_comm 的刻意例外；detect/ 不依赖本模块。
 ******************************************************************************
 */
#include "comm/host_cmd.h"
#include "comm/app_log.h"
#include "app/main_app.h"
#include "app/self_test.h"
#include "board.h"
#include "detect/calibration.h"
#include "detect/hit_detect.h"
#include <string.h>

/* ---- 帧格式 ---- */
#define HC_MAGIC        0x444D4348UL      /* 'HCMD' LE */
#define HC_VERSION      1u
#define HC_HDR_LEN      8u
#define HC_MAX_PAYLOAD  514u              /* 2 + 128×4（SET_REFERENCE 上限） */
#define HC_PARSE_MAX    (HC_HDR_LEN + HC_MAX_PAYLOAD + 4u)   /* 526 */
#define HC_RX_DMA_LEN   1024u             /* RX DMA 缓冲（NORMAL 模式） */
#define HC_RX_DRAIN_MAX 128u              /* 每圈最多消费字节 */
#define HC_CMD_PER_PASS 2u                /* 每圈最多派发命令 */

#define HC_CMD_SET_PARAMS    0x01u
#define HC_CMD_SAVE_PARAMS   0x02u
#define HC_CMD_SET_REFERENCE 0x03u
#define HC_CMD_GET_PARAMS    0x04u
#define HC_CMD_SET_DEFAULTS  0x05u

#define HC_SET_PARAMS_LEN    12u           /* <BBiHHH */

/* 精选可调参数范围（与上位机 commands.py PARAM_SPECS 同步，双端校验） */
#define HC_SBL_MIN      8u
#define HC_SBL_MAX      16u
#define HC_MINDUR_MIN   1u
#define HC_MINDUR_MAX   64u
#define HC_THR_MIN      1000L
#define HC_THR_MAX      2000000L
#define HC_REFR_MIN     50u
#define HC_REFR_MAX     1000u
#define HC_SETTLE_MIN   1u
#define HC_SETTLE_MAX   512u
#define HC_CORR_MIN     0u
#define HC_CORR_MAX     1024u

/* ---- 状态 ---- */
static uint8_t           s_rx_dma[HC_RX_DMA_LEN];   /* DMA 后台写入，主循环消费 */
static uint16_t          s_dma_last;                /* 已消费位置（仅主循环） */
static uint8_t           s_parse[HC_PARSE_MAX];
static uint16_t          s_parse_len;
static int32_t           s_ref_tmp[128];  /* SET_REFERENCE 载荷对齐缓冲 */
static volatile uint32_t s_rx_err;        /* ORE 真丢字节次数（正常 0） */
static volatile uint32_t s_fe_ne;         /* FE/NE 计数（字节照常进 DMA，仅诊断） */
static uint32_t          s_crc_err;       /* CRC 错帧（不回 ACK） */
static uint32_t          s_frame_err;     /* 版本/长度错帧（不回 ACK） */
static uint32_t          s_cmd_ok;
static uint32_t          s_cmd_err;

/* ---- CRC32（反射 0xEDB88320，init/final XOR；与 WAV1/Ref/Cal 逐字节一致，
 *       按分层约定第 4 份刻意副本，不跨模块共享。） ---- */
static uint32_t Hc_Crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;
    uint32_t b;

    for (i = 0u; i < len; i++)
    {
        crc ^= data[i];
        for (b = 0u; b < 8u; b++)
        {
            crc = (crc & 1u) ? ((crc >> 1u) ^ 0xEDB88320u) : (crc >> 1u);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ---- 参数应用与校验 ---- */
static bool Hc_ValidateSetParams(uint8_t v_sbl, uint8_t v_mdur, int32_t v_thr,
                                 uint16_t v_refr, uint16_t v_settle, uint16_t v_corr)
{
    if (v_sbl < HC_SBL_MIN || v_sbl > HC_SBL_MAX)
    {
        App_Log_Printf("[CMD] err set-params P01=%u range=%u..%u\r\n",
                       (unsigned)v_sbl, (unsigned)HC_SBL_MIN, (unsigned)HC_SBL_MAX);
        return false;
    }
    if (v_mdur < HC_MINDUR_MIN || v_mdur > HC_MINDUR_MAX)
    {
        App_Log_Printf("[CMD] err set-params P03=%u range=%u..%u\r\n",
                       (unsigned)v_mdur, (unsigned)HC_MINDUR_MIN, (unsigned)HC_MINDUR_MAX);
        return false;
    }
    if (v_thr < HC_THR_MIN || v_thr > HC_THR_MAX)
    {
        App_Log_Printf("[CMD] err set-params P04=%ld range=%ld..%ld\r\n",
                       (long)v_thr, (long)HC_THR_MIN, (long)HC_THR_MAX);
        return false;
    }
    if (v_refr < HC_REFR_MIN || v_refr > HC_REFR_MAX)
    {
        App_Log_Printf("[CMD] err set-params P05=%u range=%u..%u\r\n",
                       (unsigned)v_refr, (unsigned)HC_REFR_MIN, (unsigned)HC_REFR_MAX);
        return false;
    }
    if (v_settle < HC_SETTLE_MIN || v_settle > HC_SETTLE_MAX)
    {
        App_Log_Printf("[CMD] err set-params P26=%u range=%u..%u\r\n",
                       (unsigned)v_settle, (unsigned)HC_SETTLE_MIN, (unsigned)HC_SETTLE_MAX);
        return false;
    }
    if (v_corr < HC_CORR_MIN || v_corr > HC_CORR_MAX)
    {
        App_Log_Printf("[CMD] err set-params P27=%u range=%u..%u\r\n",
                       (unsigned)v_corr, (unsigned)HC_CORR_MIN, (unsigned)HC_CORR_MAX);
        return false;
    }
    return true;
}

static void Hc_LogParams(const char *tag, const hit_param_t *p)
{
    App_Log_Printf("[CMD] %s P01=%u P03=%u P04=%ld P05=%u P26=%u P27=%u\r\n",
                   tag,
                   (unsigned)p->s_bl, (unsigned)p->min_dur, (long)p->thr_hit,
                   (unsigned)p->refractory_ms, (unsigned)p->settle_depth,
                   (unsigned)p->corr_min_q10);
}

/* ---- 命令处理（全部主循环上下文） ---- */
static void Hc_CmdSetParams(const uint8_t *pay)
{
    uint8_t  v_sbl   = pay[0];
    uint8_t  v_mdur  = pay[1];
    int32_t  v_thr   = (int32_t)((uint32_t)pay[2] | ((uint32_t)pay[3] << 8u) |
                                 ((uint32_t)pay[4] << 16u) | ((uint32_t)pay[5] << 24u));
    uint16_t v_refr  = (uint16_t)((uint16_t)pay[6] | ((uint16_t)pay[7] << 8u));
    uint16_t v_settle = (uint16_t)((uint16_t)pay[8] | ((uint16_t)pay[9] << 8u));
    uint16_t v_corr  = (uint16_t)((uint16_t)pay[10] | ((uint16_t)pay[11] << 8u));
    hit_param_t p;

    if (!Hc_ValidateSetParams(v_sbl, v_mdur, v_thr, v_refr, v_settle, v_corr))
    {
        s_cmd_err++;
        return;
    }
    App_GetParams(&p);
    p.s_bl          = v_sbl;
    p.min_dur       = v_mdur;
    p.thr_hit       = v_thr;
    p.refractory_ms = v_refr;
    p.settle_depth  = v_settle;
    p.corr_min_q10  = v_corr;
    App_ApplyHostParams(&p);
    Hc_LogParams("ok set-params", &p);
    s_cmd_ok++;
}

static void Hc_CmdSaveParams(void)
{
    hit_param_t p;

    if (HitDetect_IsConfirming() || SelfTest_IsRunning())
    {
        App_Log_Printf("[CMD] err save busy\r\n");
        s_cmd_err++;
        return;
    }
    App_GetParams(&p);
    if (Cal_Save(&p) != 0)
    {
        App_Log_Printf("[CMD] err save err=%lu\r\n", (unsigned long)Cal_GetErr());
        s_cmd_err++;
    }
    else
    {
        App_Log_Printf("[CMD] ok save seq=%lu err=0\r\n",
                       (unsigned long)Cal_GetSeq());
        s_cmd_ok++;
    }
}

static void Hc_CmdSetReference(const uint8_t *pay, uint16_t len)
{
    uint16_t n = (uint16_t)((uint16_t)pay[0] | ((uint16_t)pay[1] << 8u));
    uint32_t ferr = 0u;
    uint16_t i;
    int r;

    if (len != (uint16_t)(2u + (uint16_t)(n * 4u)) || n == 0u || n > 128u)
    {
        App_Log_Printf("[CMD] err ref arg\r\n");
        s_cmd_err++;
        return;
    }
    if (HitDetect_IsConfirming() || SelfTest_IsRunning())
    {
        App_Log_Printf("[CMD] err ref busy\r\n");
        s_cmd_err++;
        return;
    }
    for (i = 0u; i < n; i++)
    {
        s_ref_tmp[i] = (int32_t)((uint32_t)pay[2u + 4u * i] |
                                 ((uint32_t)pay[3u + 4u * i] << 8u) |
                                 ((uint32_t)pay[4u + 4u * i] << 16u) |
                                 ((uint32_t)pay[5u + 4u * i] << 24u));
    }
    r = HitDetect_SetReference(s_ref_tmp, n, &ferr);
    switch (r)
    {
    case 0:
        App_Log_Printf("[CMD] ok ref n=%u flash=%lu\r\n",
                       (unsigned)n, (unsigned long)ferr);
        s_cmd_ok++;
        break;
    case 1:
        App_Log_Printf("[CMD] err ref flash=%lu\r\n", (unsigned long)ferr);
        s_cmd_err++;
        break;
    case -2:
        App_Log_Printf("[CMD] err ref len=%u win=%u\r\n",
                       (unsigned)n, (unsigned)HitDetect_GetWindowLen());
        s_cmd_err++;
        break;
    case -3:
        App_Log_Printf("[CMD] err ref flat\r\n");
        s_cmd_err++;
        break;
    default:
        App_Log_Printf("[CMD] err ref busy\r\n");
        s_cmd_err++;
        break;
    }
}

static void Hc_CmdGetParams(void)
{
    hit_param_t p;

    App_GetParams(&p);
    Hc_LogParams("params", &p);
    s_cmd_ok++;
}

static void Hc_CmdSetDefaults(void)
{
    hit_param_t d;
    hit_param_t p;

    Cal_GetDefaults(&d);
    App_GetParams(&p);
    /* 只回默认精选 6 字段，其余保持现值（P21/P22 等结构参数不动） */
    p.s_bl          = d.s_bl;
    p.min_dur       = d.min_dur;
    p.thr_hit       = d.thr_hit;
    p.refractory_ms = d.refractory_ms;
    p.settle_depth  = d.settle_depth;
    p.corr_min_q10  = d.corr_min_q10;
    App_ApplyHostParams(&p);
    Hc_LogParams("ok defaults", &p);
    s_cmd_ok++;
}

/* ---- 帧解析（主循环） ---- */
/* 喂一个字节给帧组装器；返回 1 = 一帧就绪（s_parse 内），0 = 继续。 */
static uint8_t Hc_FeedByte(uint8_t b)
{
    if (s_parse_len < HC_HDR_LEN)
    {
        if (s_parse_len < 4u)
        {
            /* 逐字节匹配 magic，中途失配清零重找 */
            if (b == (uint8_t)((HC_MAGIC >> (8u * (uint32_t)s_parse_len)) & 0xFFu))
            {
                s_parse[s_parse_len++] = b;
            }
            else
            {
                s_parse_len = 0u;
            }
            return 0u;
        }
        s_parse[s_parse_len++] = b;
        if (s_parse_len == HC_HDR_LEN)
        {
            uint16_t len = (uint16_t)((uint16_t)s_parse[6] |
                                      ((uint16_t)s_parse[7] << 8u));

            if (s_parse[4] != HC_VERSION || len > HC_MAX_PAYLOAD)
            {
                s_frame_err++;
                memmove(s_parse, s_parse + 1u, HC_HDR_LEN - 1u);  /* 丢 1 字节重同步 */
                s_parse_len = HC_HDR_LEN - 1u;
            }
        }
        return 0u;
    }

    s_parse[s_parse_len++] = b;
    return (s_parse_len == (uint16_t)(8u + 4u +
            (uint16_t)((uint16_t)s_parse[6] | ((uint16_t)s_parse[7] << 8u))))
           ? 1u : 0u;
}

/* 校验 CRC 并派发；完成后 s_parse_len 清零。CRC 错只计数不回 ACK。 */
static void Hc_Dispatch(void)
{
    uint16_t len = (uint16_t)((uint16_t)s_parse[6] | ((uint16_t)s_parse[7] << 8u));
    uint32_t crc_rx = (uint32_t)s_parse[8u + len] |
                      ((uint32_t)s_parse[9u + len] << 8u) |
                      ((uint32_t)s_parse[10u + len] << 16u) |
                      ((uint32_t)s_parse[11u + len] << 24u);
    uint8_t cmd = s_parse[5];

    s_parse_len = 0u;
    if (Hc_Crc32(s_parse, (uint32_t)(8u + len)) != crc_rx)
    {
        s_crc_err++;
        return;
    }

    switch (cmd)
    {
    case HC_CMD_SET_PARAMS:
        if (len != HC_SET_PARAMS_LEN)
        {
            App_Log_Printf("[CMD] err set-params len=%u\r\n", (unsigned)len);
            s_cmd_err++;
        }
        else { Hc_CmdSetParams(&s_parse[8]); }
        break;

    case HC_CMD_SAVE_PARAMS:
        if (len != 0u)
        {
            App_Log_Printf("[CMD] err save len=%u\r\n", (unsigned)len);
            s_cmd_err++;
        }
        else { Hc_CmdSaveParams(); }
        break;

    case HC_CMD_SET_REFERENCE:
        Hc_CmdSetReference(&s_parse[8], len);
        break;

    case HC_CMD_GET_PARAMS:
        if (len != 0u)
        {
            App_Log_Printf("[CMD] err get len=%u\r\n", (unsigned)len);
            s_cmd_err++;
        }
        else { Hc_CmdGetParams(); }
        break;

    case HC_CMD_SET_DEFAULTS:
        if (len != 0u)
        {
            App_Log_Printf("[CMD] err defaults len=%u\r\n", (unsigned)len);
            s_cmd_err++;
        }
        else { Hc_CmdSetDefaults(); }
        break;

    default:
        App_Log_Printf("[CMD] err cmd=0x%02X unknown\r\n", (unsigned)cmd);
        s_cmd_err++;
        break;
    }
}

/* ---- 对外接口 ---- */
void HostCmd_Init(void)
{
    s_dma_last = 0u;
    s_parse_len = 0u;
    s_rx_err = 0u;
    s_fe_ne = 0u;
    s_crc_err = 0u;
    s_frame_err = 0u;
    s_cmd_ok = 0u;
    s_cmd_err = 0u;

    /* PA10 上拉加固：MSP 配置为 NOPULL，未接上位机时浮空可产生噪声。 */
    GPIOA->PUPDR = (GPIOA->PUPDR & ~GPIO_PUPDR_PUPD10) | GPIO_PUPDR_PUPD10_0;

    if (HAL_UART_Receive_DMA(&huart1, s_rx_dma, HC_RX_DMA_LEN) != HAL_OK)
    {
        App_Log_Printf("[CMD] RX DMA arm failed\r\n");
    }
    App_Log_Printf("[CMD] USART1 RX DMA armed\r\n");
}

void HostCmd_Loop(void)
{
    uint16_t pos;
    uint16_t n = 0u;
    uint8_t dispatched = 0u;

    /* 链路质量监控：FE/NE 字节照常被 DMA 搬走，只计 ORE（真丢字节）并清标志。 */
    if ((USART1->ISR & USART_ISR_ORE) != 0u)
    {
        s_rx_err++;
        USART1->ICR = USART_ICR_ORECF;
    }
    if ((USART1->ISR & (USART_ISR_FE | USART_ISR_NE)) != 0u)
    {
        s_fe_ne++;
    }

    /* DMA 看门狗：传输被异常中止或缓冲满完成（RxState 非 BUSY_RX）时重启。 */
    if (huart1.RxState != HAL_UART_STATE_BUSY_RX || huart1.hdmarx == NULL)
    {
        s_dma_last = 0u;
        s_parse_len = 0u;    /* 流断裂：残帧无意义 */
        if (huart1.hdmarx != NULL)
        {
            (void)HAL_UART_Receive_DMA(&huart1, s_rx_dma, HC_RX_DMA_LEN);
        }
        return;
    }

    /* 计数器推字节：已收 = 缓冲长 − CNDTR。 */
    pos = (uint16_t)(HC_RX_DMA_LEN - __HAL_DMA_GET_COUNTER(huart1.hdmarx));
    while (s_dma_last != pos && n < HC_RX_DRAIN_MAX &&
           dispatched < HC_CMD_PER_PASS)
    {
        uint8_t b = s_rx_dma[s_dma_last];

        s_dma_last = (uint16_t)((s_dma_last + 1u) % HC_RX_DMA_LEN);
        n++;
        if (Hc_FeedByte(b) != 0u)
        {
            dispatched++;
            Hc_Dispatch();
        }
    }

    /* 线路静默 = 一帧传完：清标志；缓冲已排空时丢弃等不齐的残帧并重启 DMA
     * 归零计数器（NORMAL 模式不回卷，帧间隔重启防长期累积逼近缓冲尾）。 */
    if ((USART1->ISR & USART_ISR_IDLE) != 0u)
    {
        USART1->ICR = USART_ICR_IDLECF;
        if (s_dma_last == pos)
        {
            s_parse_len = 0u;
            s_dma_last = 0u;
            (void)HAL_UART_DMAStop(&huart1);
            (void)HAL_UART_Receive_DMA(&huart1, s_rx_dma, HC_RX_DMA_LEN);
        }
    }
}

/* ---- 接收通路（2026-10-29 方案 B2：DMA 接收，零接收中断）----
 * USART1 RX DMA（DMA1_Ch5，NORMAL 模式，1024B 缓冲）由 HostCmd_Init 武装一次，
 * 此后字节由 DMA 后台直搬入 s_rx_dma；主循环每圈读 DMA 计数器
 * （已收 = 缓冲长 − CNDTR）把新字节喂解析器；线路静默（IDLE 标志，主循环
 * 轮询）时清理等不齐的残帧并重启 DMA 归零计数器。
 *
 * 为什么弃单字节 IT：击打期间 WAV1 TX DMA/事件流/1kHz tick 与 RXNE 中断竞争，
 * 字节被拖到溢出（预热期无这些活动，所以成功率才高）。DMA 路径对中断延迟
 * 完全免疫；链路脏时 FE/NE 字节照常被 DMA 搬走（错误标志不参与字节流），
 * 真丢字节（ORE）由 CRC 兜底：坏帧静默丢弃、绝不误执行。
 *
 * 中断归属：USART1_IRQHandler 只服务 TX 完成（wave 帧，HAL_UART_TxCpltCallback
 * 仍归 ws2812_uart.c 分发）；RX 不产生任何中断，本模块不再占用
 * HAL_UART_RxCpltCallback / HAL_UART_ErrorCallback（weak 空实现即可）。 ---- */
