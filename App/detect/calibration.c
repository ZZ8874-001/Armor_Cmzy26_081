/**
 ******************************************************************************
 * @file    calibration.c
 * @brief   可调参数表默认值（P01~P27，设计文档 v1.5 第 7.4 节）+ Flash 持久化。
 *
 * 持久化布局（CLAUDE.md Flash 规则：页 63 归 ref_store，其余从页 62 向下）：
 *   槽 A = 页 62（0x0801F000），槽 B = 页 61（0x0801E800）——槽必须跨页，
 *   因为 STM32L4 以 2KB 整页擦除，同页双槽无法独立擦。
 *   记录 96B：magic 'CAL1' | version | size | seq | hit_param_t(80B) | crc32。
 *   Save 只擦更旧/无效槽所在页（seq+1 写新槽），掉电时最新有效槽完好；
 *   Load 纯读，取 magic/version/CRC + 参数合理性全过且 seq 最新者，否则默认值。
 *   擦+写 ~31ms 停取指——只允许主循环/初始化调用（flash_store.h 同款约束）。
 ******************************************************************************
 */
#include "detect/calibration.h"
#include "bsp/flash_store.h"
#include <stddef.h>
#include <string.h>

/* 0.9 × 2^23 = 7549747.2（P08 默认饱和阈值，数据手册线性区） */
#define DEFAULT_SAT_THR  7549747

/* ---- Flash 记录 ---- */
#define CAL_MAGIC        0x314C4143UL          /* 'CAL1' */
#define CAL_VERSION      1u
#define CAL_FLASH_BASE   0x08000000UL
#define CAL_PAGE_SIZE    0x800UL
#define CAL_SLOT_A_ADDR  (CAL_FLASH_BASE + 62UL * CAL_PAGE_SIZE)   /* 0x0801F000 */
#define CAL_SLOT_B_ADDR  (CAL_FLASH_BASE + 61UL * CAL_PAGE_SIZE)   /* 0x0801E800 */

typedef struct
{
    uint32_t   magic;
    uint16_t   version;
    uint16_t   size;                /* sizeof(hit_param_t) */
    uint32_t   seq;                 /* 每次 Save +1，双槽取新 */
    hit_param_t params;
    uint32_t   crc;                 /* 覆盖 [0, offsetof(crc)) */
} cal_rec_t;

_Static_assert(sizeof(hit_param_t) == 80u, "hit_param_t 布局变更：同步 cal 记录");
_Static_assert(sizeof(cal_rec_t) == 96u, "cal 记录必须 12 双字");
_Static_assert(CAL_SLOT_A_ADDR == 0x0801F000UL, "槽 A 地址钉死");
_Static_assert(CAL_SLOT_B_ADDR == 0x0801E800UL, "槽 B 地址钉死");

/* 最近一次 Load/Save 结果（Ozone 可观察标量）。 */
static uint8_t  s_cal_loaded;
static uint32_t s_cal_seq;
static uint32_t s_cal_err;

void Cal_GetDefaults(hit_param_t *p)
{
    /* --- 检测管线 --- */
    p->s_bl           = 13u;
    p->win_len        = 32u;
    p->min_dur        = 4u;
    p->thr_hit        = 25000;             /*滑动窗幅度触发阈值 */
    p->refractory_ms  = 200u;

    /* --- 标定与诊断 --- */
    p->k_n            = 0.0f;             /* 待标定 */
    p->k_ch[0]        = 1.0f;
    p->k_ch[1]        = 1.0f;
    p->k_ch[2]        = 1.0f;
    p->k_ch[3]        = 1.0f;
    p->sat_thr        = DEFAULT_SAT_THR;
    p->sat_time_ms    = 1000u;
    p->dead_var       = 8u;

    /* --- 温度与时序 --- */
    p->temp_win_low_mv  = 300u;           /* 0.30V */
    p->temp_win_high_mv = 750u;           /* 0.75V */
    p->drdy_timeout_ms  = 3u;

    /* --- 通信与灯光 --- */
    p->heartbeat_ms   = 50u;              /* 20Hz */
    p->comm_timeout_ms = 200u;            /* 4×心跳 */
    p->brightness     = 100u;             /* P15 亮度：默认 100%，与发射机构 SHOOT 全亮度显示对齐（2026-08-21） */
    p->team_color     = TEAM_COLOR_RED;

    /* --- ADC 与开关 --- */
    p->gain1[0] = 8u;   /* P17 量产版：4 通道 PGA=8（传感器 0.25× + 前端 0.5435× 补偿，与 s_reg_init GAIN1=0x3333 一致） */
    p->gain1[1] = 8u;
    p->gain1[2] = 8u;
    p->gain1[3] = 8u;
    p->evt_enable     = 0x01u;            /* 事件使能开，CRC 关 */
    p->temp_over_mv   = 750u;             /* 保护阈值默认最宽松：=窗上限，等效不触发 */
    p->osr            = 1024u;

    /* --- 缓冲与上报 --- */
    p->ring_depth     = 64u;              /* P21 = HIT_RING_MAX 上限：预触发历史 32.8/2 = 16.4ms */
    p->wave_window    = 64u;              /* P22 上限：P21+P22 = 128 = HIT_WAVE_MAX（锁帧 32.8ms） */
    p->status_period_ms = 1000u;          /* 默认 1Hz，由裁判系统仲裁确定、可调 */
    p->dif_depth       = 16u;             /* P25 差分高通深度：16 样本=4.1ms，截止≈39Hz（M2 修订） */
    p->settle_depth    = 64u;             /* P26 沉降门控：不应期后信号连续低于阈值 64 样本（16.4ms）才重新武装 */

    /* --- 触发后确认（P27）：默认值一律偏向"接受"——漏报（实击被拒）比误报更严重，
     * 收紧前必须先在台架采相关度分布（s_conf_corr 为可观察标量 + USART1 日志）。 --- */
    p->corr_min_q10      = 609u;          /* P27 相关度判据（台架整定中，改这一行即改阈值）：本字段值
                                           * 本身即可 —— corr ≥ 它接受、< 它拒绝（漏报优先，宁松勿紧）；
                                           * 0=只统计不判拒（重采分布整定时用）。
                                           * 注：基线热身 5τ 内与无可用参考时不适用本判据，一律接受 */
}

/* ---- CRC32（反射 0xEDB88320，init/final XOR；与 WAV1/Ref_Crc32 逐字节一致。
 *      按分层约定刻意第三份副本，不与 comm/bsp 共享实现。） ---- */
static uint32_t Cal_Crc32(const uint8_t *data, uint32_t len)
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

/* 参数合理性门（Load 用，比上位机精选通道宽松、与 Hit_ClampParams 的钳位域一致）：
 * 拒绝会破坏管线不变量的值（移位未定义、零阈值、P21+P22 超冻结缓冲等）。 */
static bool Cal_ParamsSanity(const hit_param_t *p)
{
    if (p->s_bl < 1u || p->s_bl > 24u) { return false; }
    if (p->min_dur == 0u)              { return false; }
    if (p->thr_hit <= 0)               { return false; }
    if (p->refractory_ms == 0u)        { return false; }
    if (p->ring_depth == 0u || p->ring_depth > 64u) { return false; }
    if (p->dif_depth == 0u || p->dif_depth > 64u)   { return false; }
    if (p->wave_window > (uint16_t)(128u - p->ring_depth)) { return false; }
    if (p->corr_min_q10 > 1024u)       { return false; }
    return true;
}

/* 读槽：magic/version/size/CRC/合理性全过才算可用（纯读，绝不擦除）。 */
static bool Cal_ReadRec(uint32_t addr, cal_rec_t *rec)
{
    memcpy(rec, (const void *)addr, sizeof(cal_rec_t));
    if (rec->magic != CAL_MAGIC || rec->version != CAL_VERSION ||
        rec->size != sizeof(hit_param_t))
    {
        return false;
    }
    if (rec->crc != Cal_Crc32((const uint8_t *)rec,
                              (uint32_t)offsetof(cal_rec_t, crc)))
    {
        return false;
    }
    return Cal_ParamsSanity(&rec->params);
}

void Cal_Load(hit_param_t *p)
{
    cal_rec_t a;
    cal_rec_t b;
    bool a_ok = Cal_ReadRec(CAL_SLOT_A_ADDR, &a);
    bool b_ok = Cal_ReadRec(CAL_SLOT_B_ADDR, &b);
    cal_rec_t *best = NULL;

    s_cal_err    = 0u;
    s_cal_loaded = 0u;
    s_cal_seq    = 0u;

    if (a_ok && b_ok)
    {
        best = ((int32_t)(a.seq - b.seq) >= 0) ? &a : &b;   /* seq 回绕安全 */
    }
    else if (a_ok)      { best = &a; }
    else if (b_ok)      { best = &b; }

    if (best != NULL)
    {
        *p = best->params;
        s_cal_loaded = 1u;
        s_cal_seq    = best->seq;
    }
    else
    {
        Cal_GetDefaults(p);
    }
}

int Cal_Save(const hit_param_t *p)
{
    cal_rec_t a;
    cal_rec_t b;
    cal_rec_t rec;
    cal_rec_t chk;
    bool a_ok;
    bool b_ok;
    uint32_t target;
    uint32_t new_seq;

    if (p == NULL) { return -1; }

    s_cal_err = 0u;

    /* 目标槽 = 更旧/无效者；绝不擦最新有效槽（掉电时它要兜底）。 */
    a_ok = Cal_ReadRec(CAL_SLOT_A_ADDR, &a);
    b_ok = Cal_ReadRec(CAL_SLOT_B_ADDR, &b);
    if (a_ok && b_ok)
    {
        if ((int32_t)(a.seq - b.seq) >= 0)
        {
            target = CAL_SLOT_B_ADDR;
            new_seq = a.seq + 1u;
        }
        else
        {
            target = CAL_SLOT_A_ADDR;
            new_seq = b.seq + 1u;
        }
    }
    else if (a_ok)      { target = CAL_SLOT_B_ADDR; new_seq = a.seq + 1u; }
    else if (b_ok)      { target = CAL_SLOT_A_ADDR; new_seq = b.seq + 1u; }
    else                { target = CAL_SLOT_A_ADDR; new_seq = 1u; }

    memset(&rec, 0, sizeof(rec));
    rec.magic   = CAL_MAGIC;
    rec.version = CAL_VERSION;
    rec.size    = (uint16_t)sizeof(hit_param_t);
    rec.seq     = new_seq;
    rec.params  = *p;
    rec.crc     = Cal_Crc32((const uint8_t *)&rec,
                            (uint32_t)offsetof(cal_rec_t, crc));

    if (Fs_ErasePage(target) != 0)                       { s_cal_err = 1u; return -1; }
    if (Fs_ProgramDwords(target, (const uint8_t *)&rec,
                         sizeof(rec)) != 0)              { s_cal_err = 2u; return -1; }

    /* 回读校验（内容级保险，编程"成功"但值错也拒绝）。 */
    memcpy(&chk, (const void *)target, sizeof(chk));
    if (chk.magic != CAL_MAGIC || chk.size != sizeof(hit_param_t) ||
        chk.crc != Cal_Crc32((const uint8_t *)&chk,
                             (uint32_t)offsetof(cal_rec_t, crc)))
    {
        s_cal_err = 4u;
        return -1;
    }

    s_cal_seq    = new_seq;
    s_cal_loaded = 1u;
    return 0;
}

bool Cal_LoadedFromFlash(void)
{
    return s_cal_loaded != 0u;
}

uint32_t Cal_GetSeq(void)
{
    return s_cal_seq;
}

uint32_t Cal_GetErr(void)
{
    return s_cal_err;
}
