/**
 ******************************************************************************
 * @file    ref_store.c
 * @brief   击打参考样本的 Flash 持久化实现（末页 2KB，见 ref_store.h 的调用约束）。
 *
 * 页布局（528B，8 字节对齐以适配 L4 的双字编程）：
 *   magic(4) | version(2) | n(2) | sum[128]×4 | crc32(4) | rsvd(4)
 * crc32 覆盖 magic..sum 末字节（520B），软件实现（本工程 CRC 外设未使能）。
 ******************************************************************************
 */
#include "bsp/ref_store.h"
#include "board.h"

#include <stddef.h>
#include <string.h>

/* ==================== 存储介质定位 ==================== */
/* STM32L432KBU6：128KB Flash，页大小 2KB（共 64 页，页号 0~63）。
 * 应用镜像 ≈48KB，末页 0x0801F800 专供参考样本——其它持久化（Cal_Save 等）勿占用。
 * 地址硬编码而不用 stm32l4xx.h 的 FLASH_SIZE 宏：该宏需读系统区寄存器
 * 0x1FFF75E0，且链接脚本的 FLASH 长度是 CubeMX 的通用 256K 默认值并不准。 */
#define REF_FLASH_BASE   0x08000000UL
#define REF_FLASH_SIZE   (128UL * 1024UL)
#define REF_PAGE_SIZE    0x800UL
#define REF_STORE_ADDR   (REF_FLASH_BASE + REF_FLASH_SIZE - REF_PAGE_SIZE)    /* 0x0801F800 */
#define REF_STORE_PAGE   ((REF_STORE_ADDR - REF_FLASH_BASE) / REF_PAGE_SIZE)  /* 63 */
#define REF_STORE_DW     (sizeof(ref_store_page_t) / 8u)                      /* 编程双字数 */

#define REF_MAGIC        0x52454631UL   /* 'REF1' */
#define REF_VERSION      1u

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t n;                        /* 有效样本数 1..REF_STORE_MAX */
    int32_t  sum[REF_STORE_MAX];       /* 合力 hit_sum 冻结窗（未用部分零填充 → CRC 确定） */
    uint32_t crc;                      /* 覆盖 magic..sum 末字节 */
    uint32_t rsvd;                     /* 补齐 8 字节边界，恒 0 */
} ref_store_page_t;                    /* 528B */

_Static_assert(REF_STORE_PAGE == 63u, "ref_store 页号与器件不符");
_Static_assert(sizeof(ref_store_page_t) == 528u, "ref_store 页布局长度变了，需同步 dw 数");

/* ==================== 状态（调试器可直接 watch） ==================== */
static ref_store_page_t s_page;        /* Save 的构造缓冲（528B bss，避免占用 1KB 栈） */
static volatile uint32_t s_ref_err;    /* 诊断：0=无错 1=擦除失败 2=编程失败 3=解锁失败 */
static uint8_t s_erased;               /* 本页当前处于已擦状态（Save 前置条件） */

/* ==================== 内部函数 ==================== */

/* CRC32（反射式，多项式 0xEDB88320）。按位实现，省下 1KB 常量表。 */
static uint32_t Ref_Crc32(const uint8_t *p, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;

    while (len-- > 0u)
    {
        uint8_t b;

        crc ^= (uint32_t)(*p++);
        for (b = 0u; b < 8u; b++)
        {
            crc = ((crc & 1u) != 0u) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
        }
    }
    return ~crc;
}

/* 擦除末页（~25ms 阻塞）。失败只置诊断、不抛错——上层按"无有效参考"继续跑。 */
static void Ref_Erase(void)
{
    FLASH_EraseInitTypeDef e;
    uint32_t page_err = 0u;

    if (HAL_FLASH_Unlock() != HAL_OK) { s_ref_err = 3u; return; }
    e.TypeErase = FLASH_TYPEERASE_PAGES;
    e.Banks     = FLASH_BANK_1;
    e.Page      = (uint32_t)REF_STORE_PAGE;
    e.NbPages   = 1u;
    s_ref_err = 0u;
    if (HAL_FLASHEx_Erase(&e, &page_err) != HAL_OK)
    {
        s_ref_err = 1u;
    }
    (void)HAL_FLASH_Lock();
}

/* ==================== 对外接口 ==================== */

bool RefStore_Load(int32_t *sum, uint16_t *n)
{
    const ref_store_page_t *pg = (const ref_store_page_t *)REF_STORE_ADDR;
    uint16_t i;
    uint8_t  ok;

    if (sum == NULL || n == NULL) { return false; }

    ok = (pg->magic   == REF_MAGIC &&
          pg->version == REF_VERSION &&
          pg->n > 0u && pg->n <= REF_STORE_MAX) ? 1u : 0u;

    /* 先过魔数/长度再算 CRC：空页（全 0xFF）读出来就是无效，不必白跑 CRC */
    if (ok != 0u)
    {
        if (Ref_Crc32((const uint8_t *)pg, (uint32_t)offsetof(ref_store_page_t, crc)) != pg->crc)
        {
            ok = 0u;
        }
    }

    if (ok != 0u)
    {
        for (i = 0u; i < pg->n; i++) { sum[i] = pg->sum[i]; }
        *n = pg->n;
    }
    else
    {
        *n = 0u;
    }

    /* 方案 B（2026-10-17）：不擦页。旧内容保留在 Flash 里直到本上电训练写入
     * （RefStore_Save 内部先擦后写）。任何非写入时刻掉电都不丢参考——原"上电
     * 擦页、退出才回写"的设计在 [擦页, 回写] 窗口内掉电会清空跨断电参考。 */

    return (ok != 0u) ? true : false;
}

int RefStore_Save(const int32_t *sum, uint16_t n)
{
    uint32_t i;

    if (sum == NULL || n == 0u || n > REF_STORE_MAX) { return -1; }

    /* 先擦后写（方案 B：Load 不再擦页，页里是旧内容，每次写入都须先擦）。
     * 擦除失败 → 页保留旧内容 → 返回 -1，参考下次上电仍可载入。 */
    if (s_erased == 0u)
    {
        Ref_Erase();
        if (s_ref_err != 0u) { return -1; }
        s_erased = 1u;
    }

    memset(&s_page, 0, sizeof(s_page));
    s_page.magic   = REF_MAGIC;
    s_page.version = REF_VERSION;
    s_page.n       = n;
    for (i = 0u; i < n; i++) { s_page.sum[i] = sum[i]; }
    s_page.crc = Ref_Crc32((const uint8_t *)&s_page, (uint32_t)offsetof(ref_store_page_t, crc));

    if (HAL_FLASH_Unlock() != HAL_OK) { s_ref_err = 3u; return -1; }

    s_ref_err = 0u;
    for (i = 0u; i < (uint32_t)REF_STORE_DW; i++)
    {
        uint64_t dw;

        memcpy(&dw, (const uint8_t *)&s_page + i * 8u, sizeof(dw));
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                              REF_STORE_ADDR + i * 8u, dw) != HAL_OK)
        {
            s_ref_err = 2u;
            (void)HAL_FLASH_Lock();
            s_erased = 0u;      /* 页已半写：下次 Save 必须重擦，否则双写损坏内容 */
            return -1;
        }
    }
    (void)HAL_FLASH_Lock();

    s_erased = 0u;      /* 页已被本次内容占用，再次 Save 必须先擦 */
    return 0;
}

uint32_t RefStore_GetErr(void)
{
    return s_ref_err;
}
