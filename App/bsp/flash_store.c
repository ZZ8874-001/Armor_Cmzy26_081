/**
 ******************************************************************************
 * @file    flash_store.c
 * @brief   通用 Flash 擦/写原语（参数持久化等跨模块复用）。
 *
 * 主循环/初始化专用：擦除停摆 ~25ms，编程按 8 字节双字。调用者自行保证
 * 数据结构的对齐与落盘校验（本模块只做机械擦写，不做内容解释）。
 ******************************************************************************
 */
#include "bsp/flash_store.h"
#include "stm32l4xx_hal.h"
#include <string.h>

int Fs_ErasePage(uint32_t page_addr)
{
    FLASH_EraseInitTypeDef er;
    uint32_t perr = 0u;

    if (HAL_FLASH_Unlock() != HAL_OK) { return 3; }

    er.TypeErase = FLASH_TYPEERASE_PAGES;
    er.Banks     = FLASH_BANK_1;
    er.Page      = (page_addr - FLASH_BASE) / FLASH_PAGE_SIZE;
    er.NbPages   = 1u;
    if (HAL_FLASHEx_Erase(&er, &perr) != HAL_OK || perr != 0xFFFFFFFFu)
    {
        (void)HAL_FLASH_Lock();
        return 1;
    }

    (void)HAL_FLASH_Lock();
    return 0;
}

int Fs_ProgramDwords(uint32_t addr, const uint8_t *src, uint32_t len)
{
    uint32_t i;

    if (src == NULL || (len & 7u) != 0u) { return -1; }

    if (HAL_FLASH_Unlock() != HAL_OK) { return 3; }

    for (i = 0u; i < len; i += 8u)
    {
        uint64_t dw;

        memcpy(&dw, src + i, sizeof(dw));
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                              addr + i, dw) != HAL_OK)
        {
            (void)HAL_FLASH_Lock();
            return 2;
        }
    }

    (void)HAL_FLASH_Lock();
    return 0;
}
