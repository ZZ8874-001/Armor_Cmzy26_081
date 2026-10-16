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
 *       → |hit_dif| ≥ P04 且连续 ≥P03 样本 → 触发（差分信号仅用于触发判定）
 *
 * 两段式判定（触发段不变，只新增确认段）：
 *   第一段【触发】瞬时 |hit_dif| ≥ P04 且连续 ≥P03 样本 → 锁帧 + 进 CONFIRMING。
 *   第二段【确认】等满 P22 个触发后样本（默认 64≈16.4ms），把冻结窗的合力序列
 *   与"参考样本"做归一化互相关（Pearson |ρ|×1024），区分"检测模块本体震荡 /
 *   整车振动"与"真正打在受击面的击打"。
 *
 * 参考样本（P27，替换了此前的 uni/pre/duty/zc 四特征启发式——台架证明那四项
 * 没有实质区分力）：
 *   训练  基线 EMA 热身 5τ **之后**第一次被接受的击打，其冻结窗 → 去均值固化为参考
 *         → 写 Flash（bsp/ref_store，末页 2KB）→ s_ref_trained_boot=1，退出紫闪校准态。
 *         热身期内的击打照常计数与上报，但**不判拒、不训练**——那时基线还在从 0 建立。
 *   载入  上电时 Flash 有旧值则直接可用；未训练就退出校准的路径（上位机上线、或
 *         热身 5τ+20s 超时）沿用旧值，见 state_machine.c 的 Sm_LeaveRefPending。
 *   判定  corr_q10 = |Σdx·c| ×1024 / (√Σdx² · √Σc²)，dx = 本次窗去均值、c = 参考去均值
 *         （定点实现为 num / (den>>10)，见 Hit_CorrQ10——直接 num×1024 会溢出）。
 *         **归一化保证与幅值无关**：同一位置 19N 与 100N 击打形状相同 → ρ 都趋近 1；
 *         纯幅值缩放不改变 ρ。力度/强度仍由事件字段 force_01n/intensity（K_N 路径）承载。
 *   阈值  P27=corr_min_q10，**字段值本身即门限**：corr ≥ 它接受、< 它拒绝；置 0 = 只统计不判拒。
 *         默认值只在 calibration.c 的 Cal_GetDefaults() 一处定义（台架整定中），勿写死数字。
 *   接受 → 填事件入队；拒绝 → 只留相关度（HitDetect_GetCorrReject，app 层打日志）。
 *   两条路都进 REFRACTORY，不应期自确认完成时刻起算（最小触发间隔 ≈16.4+50ms ≈ 66ms）。
 *   上报时延 ≈ 触发 1.02ms + 确认窗 16.4ms + 入队/CAN <1ms ≈ 18ms（§3.3 已据此放宽）。
 *
 * 锁帧（7.8，为高级算法留数据入口）：触发瞬间快照 {hp, hit_sum, hit_dif} 波形环
 * （预触发 P21 样本，触发样本落在索引 P21−1），随后补齐 P22 个触发后样本；
 * HitDetect_GetWaveform() 输出。冻结缓冲保留未差分的 hp/hit_sum——动量/位置等
 * 算法需要原始力学信号。
 *
 * 计算门控：IDLE 低信号仅做 O(1) 维护；确认段在窗口满的那一帧对冻结窗做两遍
 * O(N) 整数扫描（N=P21+P22=128）+ 一遍相关（共三遍 O(N) 纯整数）；REFRACTORY
 * 期间跳过判定；健康评估在 20Hz tick 中 O(1) 累计。
 ******************************************************************************
 */
#include "detect/hit_detect.h"
#include "bsp/ref_store.h"  /* 参考样本 Flash 持久化（末页 2KB） */
#include "board.h"          /* HAL_GetTick / 板级定义 */

#include <string.h>

/* ==================== 常量 ==================== */
#define HIT_RING_MAX      64u                  /* 环形窗最大深度（P21 ≤ 此值） */
#define HIT_DIF_MAX       64u                  /* 差分延迟线最大深度（P25 ≤ 此值） */
#define HIT_WAVE_MAX      (HIT_RING_MAX + 64u) /* 冻结缓冲 = 预触发 P21 + 触发后 P22 上限 */

/* 参考样本直接载入 s_ref_sum，故 Flash 侧容量必须 ≥ 冻结窗上限 */
_Static_assert(REF_STORE_MAX >= HIT_WAVE_MAX, "ref_store 容量小于冻结窗上限");

#define HIT_FS_25PCT      2097152              /* 25% 满量程（2^23/4）：偏置异常判据 */
#define HIT_REF_PERIOD_MS 100u                 /* 健康评估节拍（50ms 分频 ×2 累计到 100ms） */
#define HIT_FAULT_RETRY_MS 5000u               /* 故障自动重试周期 */

/* ==================== 状态机 ==================== */
typedef enum
{
    HIT_ST_IDLE = 0,
    HIT_ST_ARMED,
    HIT_ST_CONFIRMING,  /* 锁帧后确认：等满 P22 个触发后样本再判真伪（原 FIRING 死代码位复用） */
    HIT_ST_REFRACTORY,
    HIT_ST_SETTLING     /* 沉降门控：不应期后信号连续低于阈值 P26 样本才回 IDLE（抑制机械余振二次计数） */
} hit_state_t;

/* ==================== 运行状态（调试器可直接 watch） ==================== */
static hit_param_t s_p;                              /* 参数表副本 */
static volatile int32_t s_hit_bl[4];                 /* 四通道 EMA 基线 */
static volatile int32_t s_hit_sum;                   /* 实时合力信号 hp 之和 */
static volatile int32_t s_hit_dif;                   /* 实时差分信号（触发判定用） */
static volatile uint8_t  s_hit_state = HIT_ST_IDLE;  /* 状态机状态 */
static volatile hit_event_t s_hit_event;             /* 最新事件 */
static volatile uint32_t s_hit_count;                /* 累计事件数 */
static volatile uint8_t  s_hit_event_pending;        /* 事件待取标志 */
static volatile uint16_t s_hit_faults;               /* 故障位图（7.5） */
static volatile int32_t  s_hit_thr;                  /* 生效阈值副本 */

/* 触发后确认（两段式第二段）——标量（Ozone CSV 无法导出数组）。
 * 计数三件套满足对账：s_hit_trig_cnt = s_hit_count(接受) + s_conf_reject_cnt(拒判) + s_conf_abort_cnt(冻结异常)。 */
static volatile uint32_t s_hit_trig_cnt;             /* 累计触发次数（含被拒） */
static volatile uint32_t s_conf_reject_cnt;          /* 累计"相关度不足"拒绝次数 */
static volatile uint32_t s_conf_abort_cnt;           /* 累计冻结异常中止次数（防挂死路径） */
static volatile uint8_t  s_confirm_done;             /* 冻结窗已满（Hit_FreezeAppend 置位） */
static volatile uint32_t s_conf_ts_ms;               /* 本次触发时刻（事件/拒绝共用） */
static volatile uint16_t s_conf_corr;                /* 本次确认的相关度 |ρ|×1024（判据本体） */
static volatile uint8_t  s_corr_reject_pending;      /* 相关度拒绝待取标志（深度 1） */

/* 参考样本（P27）：s_ref_sum 为原始副本（回写 Flash 用），s_ref_c 为去均值副本
 * （相关计算用）。两者都在训练/载入时一次性固化，判定路径只读。 */
static int32_t  s_ref_sum[HIT_WAVE_MAX];
static int32_t  s_ref_c[HIT_WAVE_MAX];
static volatile uint16_t s_ref_n;                    /* 参考样本数（判定时须与窗长相等） */
static volatile uint8_t  s_ref_valid;                /* 有可用参考（Flash 载入 或 本次训练） */
static volatile uint8_t  s_ref_trained_boot;         /* 本次上电已训练（状态机紫闪退出条件） */
static volatile uint64_t s_ref_norm;                 /* Σc²，训练/载入时预计算（归一化因子） */
static volatile uint8_t  s_ref_persist_req;          /* 回写请求（状态机超时时置，主循环执行） */
static volatile uint32_t s_ref_train_err;            /* 训练时落盘结果（RefStore_GetErr 快照） */
static volatile uint8_t  s_ref_train_notice;         /* 训练完成通知待取标志（深度 1） */
static volatile uint16_t s_ref_train_n;              /* 训练完成通知：样本数 */

/* 基线热身（5τ）：自 BOOT 起已消费的样本数 + 热身样本数。热身未满 → 判据不判拒、
 * 训练禁用（EMA 沉降瞬态会污染冻结窗）。判据在确认时刻取样（见 Hit_ConfirmRun）。 */
static volatile uint32_t s_boot_samples;             /* 自 Init 起消费的样本数（Feed 每帧 +1） */
static volatile uint32_t s_warmup_samples;           /* 5×2^s_bl 样本（s_bl=13 → 40960 ≈ 10.5s） */

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

/* 整数平方根（牛顿迭代，收敛后向下取整）。归一化时分子/分母都是 64 位量，
 * 直接相乘会溢出，故对 Σdx² 与 Σc² 分别开方后再乘。 */
static uint32_t Hit_Isqrt64(uint64_t v)
{
    uint64_t x, y;

    if (v == 0u) { return 0u; }

    x = v;
    y = (x + 1u) >> 1;          /* 初值上界：y ≥ √x */
    while (y < x)
    {
        x = y;
        y = (x + v / x) >> 1;
    }
    return (x > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)x;
}

/* 固化参考：由原始合力样本算出"去均值副本 + 平方和"，两者都只在训练/载入时
 * 算一次（归一化的参考侧到此为止，判定路径不再碰参考的计算）。
 * 源可以是 s_ref_sum 本身（先整体求和再逐点改写，自拷贝安全）。 */
static void Hit_RefFix(const int32_t *sum, uint16_t n)
{
    int64_t  mean = 0;
    uint64_t sq = 0u;
    uint16_t i;

    if (sum == NULL || n == 0u || n > HIT_WAVE_MAX)
    {
        s_ref_valid = 0u;
        s_ref_n = 0u;
        return;
    }

    for (i = 0u; i < n; i++) { mean += (int64_t)sum[i]; }
    mean /= (int64_t)n;

    for (i = 0u; i < n; i++)
    {
        int32_t c = (int32_t)((int64_t)sum[i] - mean);

        s_ref_sum[i] = sum[i];
        s_ref_c[i]   = c;
        sq += (uint64_t)((int64_t)c * c);
    }

    s_ref_n    = n;
    s_ref_norm = sq;
    s_ref_valid = (sq > 0u) ? 1u : 0u;   /* 全常数窗（sq=0）等于死信号，不算有效参考 */
}

/* 归一化互相关 |ρ|×1024：本次冻结窗去均值后与已去均值的参考做点积，
 * 再除以两者标准差之积。与幅值完全无关（只比形状），极性无关（取绝对值，
 * CalibratePolarity 仍是桩）。窗长与参考不等 → 0（安全侧：不构成拒绝理由）。 */
static uint32_t Hit_CorrQ10(uint16_t n)
{
    int64_t  mean = 0;
    int64_t  num  = 0;
    uint64_t sx2  = 0u;
    uint64_t den;
    uint16_t i;

    if (s_ref_valid == 0u || s_ref_norm == 0u || n == 0u || n != s_ref_n) { return 0u; }

    for (i = 0u; i < n; i++) { mean += (int64_t)s_wave_frozen_sum[i]; }
    mean /= (int64_t)n;

    for (i = 0u; i < n; i++)
    {
        int32_t d = (int32_t)((int64_t)s_wave_frozen_sum[i] - mean);

        num += (int64_t)d * (int64_t)s_ref_c[i];
        sx2 += (uint64_t)((int64_t)d * d);
    }
    if (sx2 == 0u) { return 0u; }

    den = (uint64_t)Hit_Isqrt64(sx2) * (uint64_t)Hit_Isqrt64(s_ref_norm);
    if (den < 1024u) { return 0u; }   /* 分母过小 = 两窗都近乎无起伏，相关度无意义 */

    num = (num < 0) ? -num : num;
    {
        /* 分子分母同除 2^10：num ≤ den（Cauchy–Schwarz），但强信号下 num 可达 ~1e17，
         * 直接 num×1024 会溢出 uint64。除数下取整的相对误差 ≤ 1024/den，实际量级
         * （den ≫ 2^20）下可忽略。整数开方各自向下取整 → 商可能略大于 1024，钳到 1024。 */
        uint64_t r = (uint64_t)num / (den >> 10u);

        return (r > 1024u) ? 1024u : (uint32_t)r;
    }
}

/* 训练（热身后每上电第一次被接受的击打）：冻结窗 → 参考 → 落盘。
 * 热身未满直接返回（纵深防御：即使判据侧放行，被沉降瞬态污染的窗也不得固化）。
 * 只在主循环上下文调用（RefStore_Save 会阻塞 ~6ms），训练失败不占用本次机会。 */
static void Hit_RefTrain(void)
{
    if (s_ref_trained_boot != 0u || s_wave_total == 0u ||
        s_boot_samples < s_warmup_samples) { return; }

    Hit_RefFix(s_wave_frozen_sum, s_wave_total);
    if (s_ref_valid == 0u) { return; }

    s_ref_trained_boot = 1u;
    s_ref_train_n = s_ref_n;
    (void)RefStore_Save(s_ref_sum, s_ref_n);
    s_ref_train_err = RefStore_GetErr();   /* 0=成功，其余见 RefStore_GetErr 注释 */
    s_ref_train_notice = 1u;
}

/* 参数钳位（Init/UpdateParams 共用）：环形缓冲与延迟线的实际容量，
 * 以及 P21+P22 ≤ 冻结缓冲——否则 CONFIRMING 永远等不到满窗（见 Hit_ConfirmAbort）。 */
static void Hit_ClampParams(hit_param_t *p)
{
    if (p->ring_depth == 0u || p->ring_depth > HIT_RING_MAX) { p->ring_depth = (uint8_t)HIT_RING_MAX; }
    if (p->dif_depth  == 0u || p->dif_depth  > HIT_DIF_MAX)  { p->dif_depth  = (uint8_t)HIT_DIF_MAX; }
    if (p->wave_window > (uint16_t)(HIT_WAVE_MAX - p->ring_depth))
    {
        p->wave_window = (uint16_t)(HIT_WAVE_MAX - p->ring_depth);
    }
    /* s_bl：EMA 移位位数。钳到 1..24——`diff >> s_bl` 与 `5u << s_bl`（热身 5τ 样本数）
     * 在移位数 ≥32 时都是未定义行为（设计文档 7.4 可用范围 8~16，此处放宽到移位安全域）。 */
    if (p->s_bl == 0u)      { p->s_bl = 1u; }
    else if (p->s_bl > 24u) { p->s_bl = 24u; }
}

/* 热身样本数 = 5 个 EMA 时间常数（5×2^s_bl）。依赖 s_bl 已被 Hit_ClampParams 钳位，
 * 故 Init 与 UpdateParams 都必须在钳位之后调用。 */
static void Hit_WarmupRecompute(void)
{
    s_warmup_samples = 5u << (uint32_t)s_p.s_bl;
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

/* 确认特征（对冻结窗扫描得到；半高脉宽依赖峰值故分两遍） */
typedef struct
{
    uint32_t dif_pk;        /* |dif_sum| 峰值（事件 peak 字段） */
    int32_t  sum_pk;        /* |hp_sum| 峰值（半高脉宽基准） */
    uint32_t hp_pk[4];      /* 各通道 |hp| 峰值（事件 peak_ch 字段） */
    uint64_t abs_ch[4];     /* 各通道 Σ|hp|（冲量积分用） */
    uint32_t duty_cnt;      /* |hp_sum| ≥ sum_pk/2 的样本数（半高脉宽） */
} confirm_feat_t;

/* 扫描冻结窗提取确认特征（窗口满那一帧执行一次，O(N) 两遍整数运算） */
static void Hit_ConfirmEval(confirm_feat_t *f)
{
    uint32_t i, j;
    uint32_t n = s_wave_total;

    memset(f, 0, sizeof(*f));

    /* 第一遍：峰值 + 能量累计 */
    for (i = 0u; i < n; i++)
    {
        int32_t v = s_wave_frozen_sum[i];
        int32_t a = (v < 0) ? -v : v;

        if (a > f->sum_pk) { f->sum_pk = a; }

        v = s_wave_frozen_dif[i];
        a = (v < 0) ? -v : v;
        if ((uint32_t)a > f->dif_pk) { f->dif_pk = (uint32_t)a; }

        for (j = 0u; j < 4u; j++)
        {
            int32_t h = s_wave_frozen_hp[i][j];
            uint32_t b = (h < 0) ? (uint32_t)(-h) : (uint32_t)h;

            if (b > f->hp_pk[j]) { f->hp_pk[j] = b; }
            f->abs_ch[j] += (uint64_t)b;
        }
    }

    /* 第二遍：半高脉宽（峰值在窗内，故须第二遍）。基准就是 hp_sum 的 0 参考点，
     * 不另减均值——相关判定才做去均值归一化。 */
    for (i = 0u; i < n; i++)
    {
        int32_t a = s_wave_frozen_sum[i];
        if (a < 0) { a = -a; }
        if ((a * 2) >= f->sum_pk) { f->duty_cnt++; }
    }
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
    /* 预触发段长度即 s_wave_idx（含触发样本本身，索引 s_wave_idx−1），不再单独留标量 */
    s_freeze_cntdown = s_p.wave_window;
    s_confirm_done = 0u;
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
        s_confirm_done = 1u;        /* 与 s_wave_ready 分离：GetWaveform 仍是一次性语义 */
    }
}

/* 第一段完成：锁帧 + 进确认。不填事件——事件要等确认通过（见 Hit_ConfirmRun）。 */
static void Hit_TriggerStart(void)
{
    s_conf_ts_ms = HAL_GetTick();
    s_hit_trig_cnt++;
    s_confirm_done = 0u;
    s_conf_corr = 0u;
    Hit_FreezeStart();
    s_hit_state = HIT_ST_CONFIRMING;
}

/* 冻结异常（缓冲溢出等）：立即离开 CONFIRMING，避免状态机挂死。
 * 不产生事件也不产生拒绝记录——它既不是"真击打"也不是"相关度不足"。 */
static void Hit_ConfirmAbort(void)
{
    s_conf_corr = 0u;
    s_confirm_done = 0u;
    s_conf_abort_cnt++;

    s_hit_state = HIT_ST_REFRACTORY;
    s_refractory_cnt = (uint16_t)((uint32_t)s_p.refractory_ms * 3906u / 1000u);
}

/* 第二段完成：特征 + 相关度 → 判据 → 接受（填事件）/拒绝（留相关度），两条路都进不应期 */
static void Hit_ConfirmRun(void)
{
    confirm_feat_t ft;
    uint32_t i;
    uint32_t corr_q10;
    uint8_t  reject;
    uint8_t  warmup_elapsed;

    s_confirm_done = 0u;
    Hit_ConfirmEval(&ft);
    corr_q10 = Hit_CorrQ10(s_wave_total);
    warmup_elapsed = (s_boot_samples >= s_warmup_samples) ? 1u : 0u;

    /* 判据（P27）：相关度低于下限 → 形状不像已知的真实击打。P27=0 恒接受
     * （只统计不判拒，重采分布整定时用）。两处门控（2026-10-16 决策）：
     *   ① 热身未满（s_boot_samples < 5τ，s_bl=13 时 ≈10.5s）：基线 EMA 沉降瞬态
     *      污染冻结窗，corr 不可信 → 不判拒（击打照常接受/上报），训练同样被禁
     *      （Hit_RefTrain 同款门控），参考样本保持纯净。
     *   ② 无可用参考（s_ref_valid=0）：corr=0 无意义 → 直接接受并训练，修复死锁
     *      （旧逻辑 corr<下限拒掉首击 → Hit_RefTrain 永不可达 → 新板永不训练）。
     * 参考有效但窗长失配/冻结异常时 corr=0 → 仍落拒判分支，属预期。 */
    reject = (warmup_elapsed != 0u &&
              s_ref_valid != 0u &&
              s_p.corr_min_q10 > 0u &&
              corr_q10 < (uint32_t)s_p.corr_min_q10) ? 1u : 0u;

    s_conf_corr = (uint16_t)corr_q10;    /* 判据本体（Ozone / 日志观察） */

    if (reject == 0u)
    {
        /* ---- 接受：填事件入队（深度 1） ---- */
        hit_event_t ev;

        memset(&ev, 0, sizeof(ev));
        ev.ch = 0xFFu;                 /* 融合判定 */
        ev.ts_ms = s_conf_ts_ms;       /* 事件时刻 = 触发时刻 */
        ev.peak = ft.dif_pk;
        for (i = 0u; i < 4u; i++) { ev.peak_ch[i] = ft.hp_pk[i]; }
        ev.duration_ms = (uint16_t)(ft.duty_cnt * 256u / 1000u);   /* 半高脉宽，1 样本=0.256ms */

        /* 强度量化（7.2）：k_n=0 未标定 → force/intensity/impulse=0，仍报峰值分布。
         * 力度信息走这里（K_N 标定），相关判定只管形状、不承载幅值。 */
        if (s_p.k_n > 0.0f)
        {
            float f = 0.0f, imp = 0.0f;

            for (i = 0u; i < 4u; i++)
            {
                f   += (float)ft.hp_pk[i] * s_p.k_ch[i];
                imp += (float)ft.abs_ch[i] * s_p.k_ch[i];
            }
            f   *= s_p.k_n;
            imp *= s_p.k_n * 0.256f;   /* counts·样本 → 0.01N·ms */

            ev.force_01n = (uint16_t)Hit_ClampInt((int32_t)f, 0, 65535);
            ev.intensity = (uint8_t)Hit_ClampInt(((int32_t)f - 1900) * 100 / (49000 - 1900), 0, 100);
            ev.impulse   = (uint32_t)(imp < 0.0f ? 0.0f : imp);
        }

        s_hit_event = ev;
        s_hit_event_pending = 1u;
        s_hit_count++;                 /* 语义=接受事件数（总触发数见 s_hit_trig_cnt） */

        /* 本次上电第一次被接受的击打 → 固化为参考并落盘（训练在事件之后，
         * 故训练的那一击本身照常上报；写 Flash ~6ms 落在 REFRACTORY 窗内）。 */
        Hit_RefTrain();
    }
    else
    {
        /* ---- 拒绝：只留相关度，不产生事件（不触发状态机/CAN） ---- */
        s_conf_reject_cnt++;
        s_corr_reject_pending = 1u;
    }

    s_hit_state = HIT_ST_REFRACTORY;
    s_refractory_cnt = (uint16_t)((uint32_t)s_p.refractory_ms * 3906u / 1000u);
}

/* ==================== 对外接口 ==================== */

void HitDetect_Init(const hit_param_t *p)
{
    uint32_t i;

    if (p != NULL) { s_p = *p; }
    else { Cal_GetDefaults(&s_p); }
    Hit_ClampParams(&s_p);          /* 环形/延迟线/冻结缓冲尺寸 + s_bl 钳位（防越界、CONFIRMING 挂死、移位 UB） */
    s_hit_thr = s_p.thr_hit;
    Hit_WarmupRecompute();          /* 热身 5τ 样本数（依赖已钳位的 s_bl） */

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
    s_dur_cnt = 0u;
    s_refractory_cnt = 0u;
    s_settle_cnt = 0u;
    s_wave_ready = 0u;
    s_wave_total = 0u;
    s_freeze_active = 0u;
    s_confirm_done = 0u;
    s_corr_reject_pending = 0u;
    s_conf_ts_ms = 0u;
    s_conf_corr = 0u;
    s_hit_trig_cnt = 0u;
    s_conf_reject_cnt = 0u;
    s_conf_abort_cnt = 0u;
    s_fault_tick = 0u;
    s_boot_samples = 0u;

    /* 参考样本：上电从 Flash 载入。RefStore_Load 内含页擦除（~25ms 阻塞）——
     * 这是 HitDetect_Init 必须在 App_Init 内、TIM2 启动前调用的原因。
     * 载入成功 → 去均值固化（参考侧归一化到此完成）；失败 → 无参考，
     * 等本次上电第一次被接受的击打来训练。 */
    s_ref_n = 0u;
    s_ref_valid = 0u;
    s_ref_trained_boot = 0u;
    s_ref_norm = 0u;
    s_ref_persist_req = 0u;
    s_ref_train_err = 0u;
    s_ref_train_notice = 0u;
    s_ref_train_n = 0u;
    {
        uint16_t n = 0u;

        /* 直接载入 s_ref_sum（源即目的：Hit_RefFix 先整体求和再逐点改写，自拷贝安全），
         * 省下 512B 栈缓冲。 */
        if (RefStore_Load(s_ref_sum, &n))
        {
            Hit_RefFix(s_ref_sum, n);
        }
    }

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

    /* 热身计数：每个被消费的样本都计入（含自检喂帧、REFRACTORY/SETTLING 期间）——
     * EMA 收敛只与样本数有关，与击打状态机无关。饱和防翻转。 */
    if (s_boot_samples < 0xFFFFFFFFu) { s_boot_samples++; }

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
                Hit_TriggerStart();      /* 第一段成立 → 锁帧，事件延后到确认通过 */
            }
        }
        else
        {
            s_dur_cnt = 0u;
            s_hit_state = HIT_ST_IDLE;
        }
        break;

    case HIT_ST_CONFIRMING:
        /* 每帧的样本累加已在 switch 之前由 Hit_FreezeAppend 完成 */
        if (s_confirm_done != 0u)
        {
            Hit_ConfirmRun();            /* 窗口满：判真伪 → 接受(事件)/拒绝(记录) */
        }
        else if (s_freeze_active == 0u)
        {
            Hit_ConfirmAbort();          /* 冻结异常：按拒绝处理，绝不停在此状态 */
        }
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

bool HitDetect_GetCorrReject(uint16_t *corr_q10)
{
    if (s_corr_reject_pending == 0u || corr_q10 == NULL) { return false; }
    *corr_q10 = s_conf_corr;
    s_corr_reject_pending = 0u;
    return true;
}

uint16_t HitDetect_GetLastCorr(void)
{
    return (uint16_t)s_conf_corr;
}

bool HitDetect_GetTrainedNotice(uint16_t *n, uint32_t *flash_err)
{
    if (s_ref_train_notice == 0u) { return false; }
    if (n != NULL)         { *n = (uint16_t)s_ref_train_n; }
    if (flash_err != NULL) { *flash_err = (uint32_t)s_ref_train_err; }
    s_ref_train_notice = 0u;
    return true;
}

bool HitDetect_RefValid(void)
{
    return (s_ref_valid != 0u) ? true : false;
}

bool HitDetect_RefTrainedThisBoot(void)
{
    return (s_ref_trained_boot != 0u) ? true : false;
}

uint32_t HitDetect_GetWarmupMs(void)
{
    /* 1 样本 = 256µs → ms = samples × 256 / 1000（40960 → 10485ms）。
     * 状态机 Init 时缓存：既是紫闪 1Hz→5Hz 的相位翻转点，也是 20s 探测窗的起点。 */
    return (uint32_t)(((uint64_t)s_warmup_samples * 256u) / 1000u);
}

void HitDetect_RefRequestPersist(void)
{
    s_ref_persist_req = 1u;
}

bool HitDetect_RefPersistPending(void)
{
    return (s_ref_persist_req != 0u) ? true : false;
}

void HitDetect_RefPersist(void)
{
    s_ref_persist_req = 0u;
    if (s_ref_valid == 0u || s_ref_n == 0u) { return; }

    (void)RefStore_Save(s_ref_sum, s_ref_n);   /* 纯编程 ~6ms，仅主循环上下文 */
    s_ref_train_err = RefStore_GetErr();
}

void HitDetect_UpdateParams(const hit_param_t *p)
{
    if (p != NULL)
    {
        s_p = *p;
        Hit_ClampParams(&s_p);
        Hit_WarmupRecompute();      /* s_bl 可能变了 → 热身时长随之重算 */
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
