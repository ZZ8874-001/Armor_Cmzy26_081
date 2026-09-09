/**
 ******************************************************************************
 * @file    hit_detect.c
 * @brief   击打检测管线实现（设计文档 v1.5 第 7 章，M2 里程碑）。
 *
 * 信号链路（M2 修订：差分级联，消除静止/低速摆动误报）：
 *   raw → EMA 基线（α=2^-13 移位，τ≈2.1s，跟踪重力/温漂）
 *       → hp = raw − bl
 *       → 【差分高通】hit_dif[n] = hp[n] − hp[n−M]（M=P25 dif_depth，
 *          默认 16 样本=4.1ms，截止 ≈39Hz：1Hz 摆动 ≈−39dB、100Hz 冲击边沿无损）
 *       → |hit_dif| 环形窗峰值 + 持续≥P03 判定（差分信号仅用于触发判定）
 *
 * 锁帧（7.8，为高级算法留数据入口）：触发瞬间快照 {hp, hit_sum, hit_dif} 波形环
 * （预触发 32 样本），随后补齐 P22 个触发后样本；HitDetect_GetWaveform() 输出。
 * 冻结缓冲保留未差分的 hp/hit_sum——动量/位置等算法需要原始力学信号。
 *
 * 计算门控：IDLE 低信号仅做 O(1) 维护；窗口回扫仅在触发瞬间执行一次；
 * REFRACTORY 期间跳过判定；健康评估在 20Hz tick 中 O(1) 累计。
 ******************************************************************************
 */
#include "detect/hit_detect.h"
#include "board.h"          /* HAL_GetTick / 板级定义 */

#include <string.h>

/* ==================== 常量 ==================== */
#define HIT_RING_MAX      64u                  /* 环形窗最大深度（P21 ≤ 此值） */
#define HIT_DIF_MAX       64u                  /* 差分延迟线最大深度（P25 ≤ 此值） */
#define HIT_FS_25PCT      2097152              /* 25% 满量程（2^23/4）：偏置异常判据 */
#define HIT_REF_PERIOD_MS 100u                 /* 健康评估节拍（50ms 分频 ×2 累计到 100ms） */
#define HIT_FAULT_RETRY_MS 5000u               /* 故障自动重试周期 */

/* ==================== 状态机 ==================== */
typedef enum
{
    HIT_ST_IDLE = 0,
    HIT_ST_ARMED,
    HIT_ST_FIRING,
    HIT_ST_REFRACTORY,
    HIT_ST_SETTLING     /* 沉降门控：不应期后信号连续低于阈值 P26 样本才回 IDLE（抑制机械余振二次计数） */
} hit_state_t;

/* ==================== 运行状态（调试器可直接 watch） ==================== */
static hit_param_t s_p;                              /* 参数表副本 */
static volatile int32_t s_hit_bl[4];                 /* 四通道 EMA 基线 */
static volatile int32_t s_hit_sum;                   /* 实时合力信号 hp 之和 */
static volatile int32_t s_hit_dif;                   /* 实时差分信号（触发判定用） */
static volatile int32_t s_hit_dif_peak;              /* 最近一次触发峰值 */
static volatile uint8_t  s_hit_state = HIT_ST_IDLE;  /* 状态机状态 */
static volatile hit_event_t s_hit_event;             /* 最新事件 */
static volatile uint32_t s_hit_count;                /* 累计事件数 */
static volatile uint8_t  s_hit_event_pending;        /* 事件待取标志 */
static volatile uint16_t s_hit_faults;               /* 故障位图（7.5） */
static volatile int32_t  s_hit_thr;                  /* 生效阈值副本 */

/* 差分延迟线：hit_dif[n] = hp[n] − hp[n−M] */
static int32_t s_delay_sum[HIT_DIF_MAX];             /* hit_sum 延迟线 */
static int32_t s_delay_hp[HIT_DIF_MAX][4];           /* hp 延迟线 */
static uint32_t s_delay_wr;                          /* 延迟线写指针 */
static uint32_t s_delay_cnt;                         /* 延迟线已写入样本数 */

/* 检测环形窗（差分信号）：每样本存 dif_sum + dif_ch[4]，触发时回扫取峰值 */
static int32_t s_ring_dif[HIT_RING_MAX];
static int32_t s_ring_difch[HIT_RING_MAX][4];
static uint32_t s_ring_wr;
static uint32_t s_ring_cnt;

/* 波形环形窗（原始力学信号）：供锁帧预触发段（hp + hit_sum） */
static int32_t s_wave_ring_sum[HIT_RING_MAX];
static int32_t s_wave_ring_hp[HIT_RING_MAX][4];

/* 锁帧（7.8）：冻结缓冲 = 预触发 P21 + 触发后 P22 样本 × {sum, hp[4], dif} */
#define HIT_WAVE_MAX (HIT_RING_MAX + 64u)
static int32_t s_wave_frozen_sum[HIT_WAVE_MAX];
static int32_t s_wave_frozen_hp[HIT_WAVE_MAX][4];
static int32_t s_wave_frozen_dif[HIT_WAVE_MAX];
static volatile uint16_t s_wave_total;
static volatile uint8_t  s_wave_ready;
static uint16_t s_wave_idx;
static uint16_t s_freeze_cntdown;
static uint8_t  s_freeze_active;

/* 持续判定与不应期 */
static uint16_t s_dur_cnt;
static uint16_t s_refractory_cnt;
static uint16_t s_settle_cnt;                     /* 沉降门控连续低于阈值计数 */

/* 健康评估累计 */
static uint32_t s_sat_cnt[4];
static uint32_t s_off_cnt[4];
static uint32_t s_dead_tick_cnt[4];
static uint32_t s_spi_err_last;
static uint32_t s_drdy_last;      /* DRDY 停滞检测快照（7.5，50ms 窗口增量） */
static uint32_t s_fault_tick;

/* ==================== 内部函数 ==================== */

static int32_t Hit_ClampInt(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return v;
}

/* 差分延迟线：压入当前 sum/hp，取出 dif */
static void Hit_DelayPush(int32_t sum, const int32_t hp[4], int32_t *dif_sum, int32_t dif_ch[4])
{
    uint32_t idx = s_delay_wr;

    if (s_delay_cnt >= s_p.dif_depth)
    {
        uint32_t oldest = (idx + HIT_DIF_MAX - s_p.dif_depth) % HIT_DIF_MAX;
        *dif_sum = sum - s_delay_sum[oldest];
        dif_ch[0] = hp[0] - s_delay_hp[oldest][0];
        dif_ch[1] = hp[1] - s_delay_hp[oldest][1];
        dif_ch[2] = hp[2] - s_delay_hp[oldest][2];
        dif_ch[3] = hp[3] - s_delay_hp[oldest][3];
    }
    else
    {
        *dif_sum = 0;
        dif_ch[0] = 0; dif_ch[1] = 0; dif_ch[2] = 0; dif_ch[3] = 0;
        s_delay_cnt++;
    }

    s_delay_sum[idx] = sum;
    s_delay_hp[idx][0] = hp[0];
    s_delay_hp[idx][1] = hp[1];
    s_delay_hp[idx][2] = hp[2];
    s_delay_hp[idx][3] = hp[3];
    s_delay_wr = (idx + 1u) % HIT_DIF_MAX;
}

/* 检测环形窗写入 */
static void Hit_RingPush(int32_t dif_sum, const int32_t dif_ch[4])
{
    uint32_t idx = s_ring_wr;

    s_ring_dif[idx] = dif_sum;
    s_ring_difch[idx][0] = dif_ch[0];
    s_ring_difch[idx][1] = dif_ch[1];
    s_ring_difch[idx][2] = dif_ch[2];
    s_ring_difch[idx][3] = dif_ch[3];
    s_ring_wr = (idx + 1u) % s_p.ring_depth;
    if (s_ring_cnt < s_p.ring_depth)
    {
        s_ring_cnt++;
    }
}

/* 波形环形窗写入（原始 hp/sum） */
static void Hit_WaveRingPush(int32_t sum, const int32_t hp[4])
{
    uint32_t idx = s_ring_wr;   /* 与检测环共用写指针（同步） */

    s_wave_ring_sum[idx] = sum;
    s_wave_ring_hp[idx][0] = hp[0];
    s_wave_ring_hp[idx][1] = hp[1];
    s_wave_ring_hp[idx][2] = hp[2];
    s_wave_ring_hp[idx][3] = hp[3];
}

/* 按环形索引取值（0 = 最老，cnt-1 = 最新）；det 环与 wave 环共用索引规则 */
static void Hit_RingGet(uint32_t age, int32_t *dif_sum, int32_t dif_ch[4], int32_t *sum, int32_t hp[4])
{
    uint32_t idx;
    uint32_t depth = s_p.ring_depth;

    if (s_ring_cnt < depth)
    {
        idx = age;
    }
    else
    {
        idx = (s_ring_wr + depth - 1u - age) % depth;
    }
    if (dif_sum != NULL)
    {
        *dif_sum = s_ring_dif[idx];
        if (dif_ch != NULL)
        {
            dif_ch[0] = s_ring_difch[idx][0];
            dif_ch[1] = s_ring_difch[idx][1];
            dif_ch[2] = s_ring_difch[idx][2];
            dif_ch[3] = s_ring_difch[idx][3];
        }
    }
    if (sum != NULL)
    {
        *sum = s_wave_ring_sum[idx];
        if (hp != NULL)
        {
            hp[0] = s_wave_ring_hp[idx][0];
            hp[1] = s_wave_ring_hp[idx][1];
            hp[2] = s_wave_ring_hp[idx][2];
            hp[3] = s_wave_ring_hp[idx][3];
        }
    }
}

/* 触发瞬间：扫描差分窗口取合力峰值与各通道峰值（仅触发时执行一次） */
static void Hit_ScanWindow(hit_event_t *e)
{
    uint32_t i;
    int32_t  ds, dch[4];
    int32_t  sum_peak = 0;
    uint32_t ch_peak[4] = {0u, 0u, 0u, 0u};

    for (i = 0u; i < s_ring_cnt; i++)
    {
        uint32_t j;
        int32_t a;

        Hit_RingGet(i, &ds, dch, NULL, NULL);
        a = (ds < 0) ? -ds : ds;
        if (a > sum_peak) { sum_peak = a; }
        for (j = 0u; j < 4u; j++)
        {
            uint32_t b = (dch[j] < 0) ? (uint32_t)(-dch[j]) : (uint32_t)dch[j];
            if (b > ch_peak[j]) { ch_peak[j] = b; }
        }
    }
    e->peak     = (uint32_t)sum_peak;
    e->peak_ch[0] = ch_peak[0];
    e->peak_ch[1] = ch_peak[1];
    e->peak_ch[2] = ch_peak[2];
    e->peak_ch[3] = ch_peak[3];
}

/* 触发瞬间：锁帧——波形环（hp/sum）+ 差分环快照为预触发段 */
static void Hit_FreezeStart(void)
{
    uint32_t i;

    s_wave_idx = 0u;
    for (i = 0u; i < s_ring_cnt; i++)
    {
        int32_t sum, hp[4], ds, dch[4];
        Hit_RingGet(s_ring_cnt - 1u - i, &ds, dch, &sum, hp);   /* 最老→最新 */
        s_wave_frozen_sum[s_wave_idx] = sum;
        s_wave_frozen_hp[s_wave_idx][0] = hp[0];
        s_wave_frozen_hp[s_wave_idx][1] = hp[1];
        s_wave_frozen_hp[s_wave_idx][2] = hp[2];
        s_wave_frozen_hp[s_wave_idx][3] = hp[3];
        s_wave_frozen_dif[s_wave_idx] = ds;
        s_wave_idx++;
    }
    s_freeze_cntdown = s_p.wave_window;
    s_freeze_active = 1u;
}

/* 触发后每帧调用：把当前样本补入冻结缓冲 */
static void Hit_FreezeAppend(int32_t sum, const int32_t hp[4], int32_t dif_sum)
{
    if (s_freeze_active == 0u || s_wave_idx >= HIT_WAVE_MAX)
    {
        s_freeze_active = 0u;
        return;
    }
    s_wave_frozen_sum[s_wave_idx] = sum;
    s_wave_frozen_hp[s_wave_idx][0] = hp[0];
    s_wave_frozen_hp[s_wave_idx][1] = hp[1];
    s_wave_frozen_hp[s_wave_idx][2] = hp[2];
    s_wave_frozen_hp[s_wave_idx][3] = hp[3];
    s_wave_frozen_dif[s_wave_idx] = dif_sum;
    s_wave_idx++;

    if (s_freeze_cntdown > 0u) { s_freeze_cntdown--; }
    if (s_freeze_cntdown == 0u)
    {
        s_freeze_active = 0u;
        s_wave_total = s_wave_idx;
        s_wave_ready = 1u;
    }
}

/* 触发：填事件 + 锁帧 + 进入不应期 */
static void Hit_Fire(void)
{
    hit_event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.ch = 0xFFu;                 /* 融合判定 */
    ev.ts_ms = HAL_GetTick();
    Hit_ScanWindow(&ev);

    /* 强度量化（7.2）：k_n=0 未标定 → force/intensity=0，仍报峰值分布 */
    if (s_p.k_n > 0.0f)
    {
        float f = 0.0f;
        uint32_t i;
        for (i = 0u; i < 4u; i++)
        {
            f += (float)ev.peak_ch[i] * s_p.k_ch[i];
        }
        f *= s_p.k_n;
        ev.force_01n = (uint16_t)Hit_ClampInt((int32_t)f, 0, 65535);
        ev.intensity = (uint8_t)Hit_ClampInt(((int32_t)f - 1900) * 100 / (49000 - 1900), 0, 100);
    }

    Hit_FreezeStart();

    s_hit_dif_peak = (int32_t)ev.peak;
    s_hit_event = ev;
    s_hit_event_pending = 1u;
    s_hit_count++;
    s_hit_state = HIT_ST_REFRACTORY;
    s_refractory_cnt = (uint16_t)((uint32_t)s_p.refractory_ms * 3906u / 1000u);
}

/* ==================== 对外接口 ==================== */

void HitDetect_Init(const hit_param_t *p)
{
    uint32_t i;

    if (p != NULL) { s_p = *p; }
    else { Cal_GetDefaults(&s_p); }
    s_hit_thr = s_p.thr_hit;

    for (i = 0u; i < 4u; i++)
    {
        s_hit_bl[i] = 0;
        s_sat_cnt[i] = 0u;
        s_off_cnt[i] = 0u;
        s_dead_tick_cnt[i] = 0u;
    }
    s_delay_wr = 0u;
    s_delay_cnt = 0u;
    s_ring_wr = 0u;
    s_ring_cnt = 0u;
    s_hit_state = HIT_ST_IDLE;
    s_hit_event_pending = 0u;
    s_hit_count = 0u;
    s_hit_faults = 0u;
    s_hit_dif = 0;
    s_hit_dif_peak = 0;
    s_dur_cnt = 0u;
    s_refractory_cnt = 0u;
    s_settle_cnt = 0u;
    s_wave_ready = 0u;
    s_wave_total = 0u;
    s_freeze_active = 0u;
    s_fault_tick = 0u;

    {
        ads_diag_t d;
        ADS131M04_GetDiag(&d);
        s_spi_err_last = d.spi_err;
        s_drdy_last    = d.drdy_cnt;
    }
}

void HitDetect_Feed(const ads_frame_t *f)
{
    int32_t hp[4];
    int32_t dif_ch[4];
    int32_t sum, dif_sum, abs_dif;
    uint32_t i;

    /* --- 每帧必做（O(1)，纯整数） --- */
    for (i = 0u; i < 4u; i++)
    {
        int32_t diff = f->ch[i] - s_hit_bl[i];
        s_hit_bl[i] += diff >> s_p.s_bl;      /* EMA：α=2^-s_bl */
        hp[i] = f->ch[i] - s_hit_bl[i];

        /* 健康累计：饱和 / 偏置异常持续计数 */
        {
            int32_t a = (f->ch[i] < 0) ? -f->ch[i] : f->ch[i];
            if (a > s_p.sat_thr) { s_sat_cnt[i]++; } else { s_sat_cnt[i] = 0u; }
            if (s_hit_bl[i] > HIT_FS_25PCT || s_hit_bl[i] < -HIT_FS_25PCT)
            { s_off_cnt[i]++; } else { s_off_cnt[i] = 0u; }
        }
    }
    sum = hp[0] + hp[1] + hp[2] + hp[3];
    s_hit_sum = sum;

    /* 差分高通：hit_dif[n] = hp[n] − hp[n−M] */
    Hit_DelayPush(sum, hp, &dif_sum, dif_ch);
    s_hit_dif = dif_sum;
    abs_dif = (dif_sum < 0) ? -dif_sum : dif_sum;

    Hit_RingPush(dif_sum, dif_ch);
    Hit_WaveRingPush(sum, hp);
    Hit_FreezeAppend(sum, hp, dif_sum);

    /* --- 状态机（门控：IDLE 低信号 / REFRACTORY 走最短路径） --- */
    switch (s_hit_state)
    {
    case HIT_ST_IDLE:
        if (abs_dif >= s_hit_thr)
        {
            s_dur_cnt = 1u;
            s_hit_state = HIT_ST_ARMED;
        }
        break;

    case HIT_ST_ARMED:
        if (abs_dif >= s_hit_thr)
        {
            s_dur_cnt++;
            if (s_dur_cnt >= s_p.min_dur)
            {
                Hit_Fire();
            }
        }
        else
        {
            s_dur_cnt = 0u;
            s_hit_state = HIT_ST_IDLE;
        }
        break;

    case HIT_ST_FIRING:
        s_hit_state = HIT_ST_REFRACTORY;
        s_refractory_cnt = (uint16_t)((uint32_t)s_p.refractory_ms * 3906u / 1000u);
        break;

    case HIT_ST_REFRACTORY:
        if (s_refractory_cnt > 0u)
        {
            s_refractory_cnt--;
        }
        else
        {
            s_settle_cnt = 0u;
            s_hit_state = HIT_ST_SETTLING;   /* 不应期结束 → 沉降门控，不立即武装 */
        }
        break;

    case HIT_ST_SETTLING:
    default:
        if (abs_dif >= s_hit_thr)
        {
            s_settle_cnt = 0u;               /* 余振仍在：重新计时，保持封锁 */
        }
        else
        {
            s_settle_cnt++;
            if (s_settle_cnt >= s_p.settle_depth)
            {
                s_hit_state = HIT_ST_IDLE;   /* 连续 P26 样本低于阈值：余振结束，重新武装 */
            }
        }
        break;
    }
}

bool HitDetect_GetEvent(hit_event_t *e)
{
    if (s_hit_event_pending == 0u || e == NULL) { return false; }
    *e = s_hit_event;
    s_hit_event_pending = 0u;
    return true;
}

void HitDetect_UpdateParams(const hit_param_t *p)
{
    if (p != NULL)
    {
        s_p = *p;
        s_hit_thr = p->thr_hit;
    }
}

uint16_t HitDetect_GetFaultFlags(void)
{
    return (uint16_t)s_hit_faults;
}

/* 20Hz 健康评估（7.5；在 TIM2 50ms 分频中调用） */
void HitDetect_Tick(void)
{
    uint32_t i;
    uint16_t new_faults = 0u;

    /* 1) 饱和/偏置：每帧累计的持续计数换算（3.906 帧/ms） */
    for (i = 0u; i < 4u; i++)
    {
        if (s_sat_cnt[i] > (uint32_t)s_p.sat_time_ms * 4u)   { new_faults |= (uint16_t)(FAULT_CH0_SAT << i); }
        if (s_off_cnt[i] > 5000u * 4u)                      { new_faults |= (uint16_t)(FAULT_CH0_OFFSET << i); }
    }

    /* 2) 死通道：波形环方差 < P10（50ms 节拍累计 5s = 100 拍） */
    for (i = 0u; i < 4u; i++)
    {
        int64_t mean = 0, sq = 0;
        uint32_t n = s_ring_cnt;
        uint32_t k;

        if (n < 8u) { continue; }
        for (k = 0u; k < n; k++)
        {
            int32_t v = s_wave_ring_hp[k][i];
            mean += v;
            sq += (int64_t)v * v;
        }
        mean /= (int64_t)n;
        if ((sq / (int64_t)n - mean * mean) < (int64_t)s_p.dead_var)
        {
            s_dead_tick_cnt[i]++;
        }
        else
        {
            s_dead_tick_cnt[i] = 0u;
        }
        if (s_dead_tick_cnt[i] >= 100u)                     { new_faults |= (uint16_t)(FAULT_CH0_DEAD << i); }
    }

    /* 3) SPI/DRDY 故障（7.5）：诊断错误增量 + DRDY 停滞（间隔>P12 持续 50ms） */
    {
        ads_diag_t d;
        uint32_t drdy_delta;
        ADS131M04_GetDiag(&d);
        if (d.spi_err != s_spi_err_last)
        {
            s_spi_err_last = d.spi_err;
            new_faults |= FAULT_SPI;
        }
        /* 50ms 窗口 DRDY 增量：正常 ≈195（3906×0.05）；< 50/P12 判停滞
         * （默认 P12=3ms → 阈值 16）。ADC 初始化失败时增量为 0，live 侧
         * 也由此兜底置 FAULT_SPI（sticky 由 main_app 置，见 app/faults.h）。 */
        drdy_delta = d.drdy_cnt - s_drdy_last;
        s_drdy_last = d.drdy_cnt;
        if (s_p.drdy_timeout_ms != 0u && drdy_delta < (50u / s_p.drdy_timeout_ms))
        {
            new_faults |= FAULT_SPI;
        }
    }

    /* 4) 故障闩存 + 每 5s 自动重试 */
    s_hit_faults |= new_faults;
    s_fault_tick += HIT_REF_PERIOD_MS;
    if (s_fault_tick >= HIT_FAULT_RETRY_MS)
    {
        s_fault_tick = 0u;
        s_hit_faults &= new_faults;
    }

    /* 5) 发布到统一故障注册表（live 域）——状态机/灯/上报只读 Fault_GetBitmap() */
    Fault_UpdateLive(FAULT_MASK_HITDETECT, (uint16_t)s_hit_faults);
}

int HitDetect_CalibratePolarity(void)
{
    /* TODO(M2 后半)：开机击打校对——已知方向击打（或 DAC 注入），
     * 校验四通道响应符号 → ADS131M04_SetChannelSign() → Flash 持久化。 */
    return 0;
}

int HitDetect_GetWaveform(uint8_t ch, ads_sample_t *buf, uint16_t *n)
{
    uint16_t i;

    if (buf == NULL || n == NULL || s_wave_ready == 0u || ch >= 4u)
    {
        if (n != NULL) { *n = 0u; }
        return -1;
    }
    for (i = 0u; i < s_wave_total; i++)
    {
        buf[i] = s_wave_frozen_hp[i][ch];
    }
    *n = s_wave_total;
    s_wave_ready = 0u;
    return 0;
}

int HitDetect_EstimateImpact(const hit_event_t *e, impact_info_t *info)
{
    /* 预留（7.8/M6）：弹速/入射角反推——待实弹标定恢复系数 e 与角度模型。 */
    (void)e; (void)info;
    return -1;
}

int HitDetect_EstimatePosition(const hit_event_t *e, pos_info_t *pos)
{
    /* 预留（7.8/M2 确定坐标轴/M6 标定）：由 peak_ch[4] 分布反推击打位置。 */
    (void)e; (void)pos;
    return -1;
}
