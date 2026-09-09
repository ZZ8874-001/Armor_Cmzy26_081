/**
 ******************************************************************************
 * @file    self_test.c
 * @brief   非阻塞自检实现（设计文档 v1.5 第 7.6 节，2026-09-09 落地）。
 *
 * 状态机由主循环每圈步进（SelfTest_Poll），任何单步阻塞 <1ms：
 *   ST_REGS  3×RegRead 回读校验（ID 0x24xx / MODE 0x0510 / CLOCK 0x0F0E）
 *   ST_DRDY  1s 窗口统计 DRDY 频率，通过域 3125~4687（3906±20%，CLOCK 不写故固定）
 *   ST_STATS 256 帧（≈66ms）统计均值/RMS；|mean|≤25%FS 且 1≤RMS≤50（7.6）；
 *            300ms 无帧提前判失败（采集停滞）
 *   ST_TEMP  TempMon 4 路在 P11 窗内且未超 P19
 *   ST_DONE  自检期间 spi_err/drop 增量检查；通过 → 清 sticky（POWER/SPI）
 * 运行期间自检独占帧消费并逐帧喂 HitDetect（管线持续运行、环形不溢出）。
 ******************************************************************************
 */
#include "app/self_test.h"
#include "board.h"
#include "bsp/ads131m04.h"
#include "bsp/temp_mon.h"
#include "detect/hit_detect.h"
#include "app/faults.h"
#include "comm/app_log.h"

#include <math.h>
#include <string.h>

/* 阈值（7.6） */
#define ST_ABORT_MS       5000u     /* 总时长超限 abort */
#define ST_DRDY_MS        1000u     /* DRDY 频率统计窗口 */
#define ST_DRDY_LOW       3125u     /* 3906×0.8 */
#define ST_DRDY_HIGH      4687u     /* 3906×1.2 */
#define ST_STATS_N        256u      /* 噪声统计帧数 */
#define ST_STATS_WATCHDOG 300u      /* 统计期无帧 watchdog */
#define ST_MEAN_LIMIT     2097152   /* 25% 满量程（2^23/4，同 OFFSET 判据） */
#define ST_RMS_LOW        1u        /* 方差下限（抓死通道） */
#define ST_RMS_HIGH       50u       /* 噪声上限（7.6：1<方差<50 counts） */

typedef enum
{
    STP_IDLE = 0,
    STP_REGS,
    STP_DRDY,
    STP_STATS,
    STP_TEMP,
    STP_DONE
} st_phase_t;

static st_phase_t  s_phase = STP_IDLE;
static hit_param_t s_p;
static self_test_t s_result;
static volatile uint8_t s_result_ready;   /* 主循环写、BoardComm 主循环读，同一上下文 */

static uint32_t s_start_ms;       /* 自检启动时刻 */
static uint32_t s_drdy_start;     /* DRDY 相位起点计数 */
static uint32_t s_last_frame_ms;  /* 最近一帧时刻（统计 watchdog） */
static uint32_t s_stats_n;
static int64_t  s_sum[4];
static int64_t  s_sqsum[4];
static uint32_t s_diag_spi_err0;  /* 启动时 spi_err 快照 */
static uint32_t s_diag_drop0;     /* 启动时 drop_cnt 快照 */

/* 进入 DONE：固化结果、日志、清理状态 */
static void St_Finish(uint8_t extra_fail)
{
    ads_diag_t d;

    s_result.fails |= extra_fail;

    /* 诊断增量检查：自检期间不许出现 SPI 错误/跳帧 */
    ADS131M04_GetDiag(&d);
    if (d.spi_err != s_diag_spi_err0 || d.drop_cnt != s_diag_drop0)
    {
        s_result.fails |= ST_FAIL_DIAG;
    }

    s_result.overall = (s_result.fails == 0u) ? ST_RESULT_PASS : ST_RESULT_FAIL;

    if (s_result.overall == ST_RESULT_PASS)
    {
        /* 自检通过：清除事件型故障（PVD 欠压可自愈；SPI sticky 仅在此恢复） */
        Fault_ClearSticky(FAULT_POWER | FAULT_SPI);
        App_Log_Printf("[SELFTEST] PASS drdy=%lu\r\n", (unsigned long)s_result.drdy_hz);
    }
    else
    {
        App_Log_Printf("[SELFTEST] FAIL bits=0x%02X drdy=%lu spi=%lu drop=%lu\r\n",
                       (unsigned int)s_result.fails, (unsigned long)s_result.drdy_hz,
                       (unsigned long)d.spi_err, (unsigned long)d.drop_cnt);
    }

    s_phase = STP_IDLE;
    s_result_ready = 1u;
}

/* 帧消费（DRDY/STATS 相位）：逐帧喂 HitDetect，STATS 相位顺带累计 */
static void St_DrainFrames(void)
{
    ads_frame_t f;

    while (ADS131M04_IsFrameReady())
    {
        if (ADS131M04_ReadFrame(&f) == 0)
        {
            s_last_frame_ms = HAL_GetTick();
            HitDetect_Feed(&f);   /* 自检期间管线持续运行（基线/健康不停摆） */

            if (s_phase == STP_STATS && s_stats_n < ST_STATS_N)
            {
                uint32_t i;
                for (i = 0u; i < 4u; i++)
                {
                    s_sum[i]   += f.ch[i];
                    s_sqsum[i] += (int64_t)f.ch[i] * f.ch[i];
                }
                s_stats_n++;
            }
        }
    }
}

void SelfTest_Init(const hit_param_t *p)
{
    if (p != NULL) { s_p = *p; }
    else           { Cal_GetDefaults(&s_p); }
    s_phase = STP_IDLE;
    s_result_ready = 0u;
    memset(&s_result, 0, sizeof(s_result));
}

bool SelfTest_Request(void)
{
    ads_diag_t d;

    if (s_phase != STP_IDLE)
    {
        return false;   /* 运行中忽略重复触发 */
    }

    memset(&s_result, 0, sizeof(s_result));
    ADS131M04_GetDiag(&d);
    s_diag_spi_err0 = d.spi_err;
    s_diag_drop0    = d.drop_cnt;
    s_stats_n = 0u;
    memset(s_sum, 0, sizeof(s_sum));
    memset(s_sqsum, 0, sizeof(s_sqsum));

    s_phase = STP_REGS;
    s_start_ms = HAL_GetTick();
    return true;
}

void SelfTest_Poll(void)
{
    if (s_phase == STP_IDLE)
    {
        return;
    }

    /* 总超时 abort（覆盖任何相位挂死） */
    if ((HAL_GetTick() - s_start_ms) > ST_ABORT_MS)
    {
        St_Finish(ST_FAIL_TIMEOUT);
        return;
    }

    switch (s_phase)
    {
    case STP_REGS:
        if (!ADS131M04_IsInitOk())
        {
            /* 初始化失败：其余相位无意义，立即结束（≈0 时延） */
            St_Finish(ST_FAIL_INIT);
            break;
        }
        {
            uint16_t id = 0u, mode = 0u, clock_reg = 0u;
            if (ADS131M04_RegRead(0x00u, &id) != 0 ||
                ADS131M04_RegRead(0x02u, &mode) != 0 ||
                ADS131M04_RegRead(0x03u, &clock_reg) != 0)
            {
                St_Finish(ST_FAIL_REGS);
                break;
            }
            App_Log_Printf("[SELFTEST] ID=0x%04X MODE=0x%04X CLOCK=0x%04X\r\n",
                           id, mode, clock_reg);
            if ((id & 0x0F00u) != 0x0400u || mode != 0x0510u || clock_reg != 0x0F0Eu)
            {
                St_Finish(ST_FAIL_REGS);
                break;
            }
            {
                ads_diag_t d;
                ADS131M04_GetDiag(&d);
                s_drdy_start = d.drdy_cnt;
            }
            s_last_frame_ms = HAL_GetTick();
            s_phase = STP_DRDY;
        }
        break;

    case STP_DRDY:
        St_DrainFrames();
        if ((HAL_GetTick() - s_start_ms) >= ST_DRDY_MS)
        {
            ads_diag_t d;
            ADS131M04_GetDiag(&d);
            s_result.drdy_hz = d.drdy_cnt - s_drdy_start;
            App_Log_Printf("[SELFTEST] DRDY = %lu Hz\r\n", (unsigned long)s_result.drdy_hz);
            if (s_result.drdy_hz < ST_DRDY_LOW || s_result.drdy_hz > ST_DRDY_HIGH)
            {
                St_Finish(ST_FAIL_DRDY);
                break;
            }
            s_last_frame_ms = HAL_GetTick();
            s_phase = STP_STATS;
        }
        break;

    case STP_STATS:
        St_DrainFrames();
        /* 300ms 无帧：采集停滞，提前判失败 */
        if (s_stats_n < ST_STATS_N && (HAL_GetTick() - s_last_frame_ms) > ST_STATS_WATCHDOG)
        {
            St_Finish(ST_FAIL_NOISE);
            break;
        }
        if (s_stats_n >= ST_STATS_N)
        {
            uint32_t i;
            uint8_t noise_fail = 0u;
            for (i = 0u; i < 4u; i++)
            {
                double m = (double)s_sum[i] / (double)ST_STATS_N;
                double v = (double)s_sqsum[i] / (double)ST_STATS_N - m * m;
                int32_t mean = (int32_t)m;
                int32_t rms  = (int32_t)sqrt((v > 0.0) ? v : 0.0);
                s_result.mean[i] = mean;
                s_result.rms[i]  = rms;
                App_Log_Printf("[SELFTEST] CH%lu mean=%ld RMS=%ld\r\n",
                               (unsigned long)i, (long)mean, (long)rms);
                if (mean > ST_MEAN_LIMIT || mean < -ST_MEAN_LIMIT ||
                    rms < (int32_t)ST_RMS_LOW || rms > (int32_t)ST_RMS_HIGH)
                {
                    noise_fail = 1u;
                }
            }
            St_Finish(noise_fail ? ST_FAIL_NOISE : 0u);
        }
        break;

    case STP_TEMP:
        TempMon_GetLastMv(s_result.temp_mv);
        if (!TempMon_IsReady())
        {
            St_Finish(ST_FAIL_TEMP);
            break;
        }
        {
            uint32_t i;
            uint8_t temp_fail = 0u;
            for (i = 0u; i < 4u; i++)
            {
                uint16_t mv = s_result.temp_mv[i];
                if (mv < s_p.temp_win_low_mv || mv > s_p.temp_win_high_mv ||
                    mv >= s_p.temp_over_mv)
                {
                    temp_fail = 1u;
                }
            }
            App_Log_Printf("[SELFTEST] TEMP %u %u %u %u mV\r\n",
                           (unsigned int)s_result.temp_mv[0], (unsigned int)s_result.temp_mv[1],
                           (unsigned int)s_result.temp_mv[2], (unsigned int)s_result.temp_mv[3]);
            St_Finish(temp_fail ? ST_FAIL_TEMP : 0u);
        }
        break;

    case STP_DONE:
    default:
        /* 上相位已调用 St_Finish 转 IDLE，本态不应到达 */
        s_phase = STP_IDLE;
        break;
    }
}

bool SelfTest_IsRunning(void)
{
    return s_phase != STP_IDLE;
}

bool SelfTest_TakeResult(self_test_t *r)
{
    if (r == NULL || s_result_ready == 0u)
    {
        return false;
    }
    *r = s_result;
    s_result_ready = 0u;
    return true;
}
