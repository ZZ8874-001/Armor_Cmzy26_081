/**
 ******************************************************************************
 * @file    flash_store.h
 * @brief   通用 Flash 擦/写原语（参数持久化等跨模块复用）。
 *
 * 规则（CLAUDE.md Flash 布局 + ref_store 同款约束）：
 *  - 擦除 ~25ms、编程按 8 字节双字，期间取指停摆——只允许主循环/初始化调用，
 *    严禁中断上下文；
 *  - 页 63 归 ref_store（_Static_assert 钉死），本模块只管"给地址就擦写"；
 *  - 错误码与 RefStore_GetErr 语义一致：0 成功，1 擦除失败，2 编程失败，3 解锁失败。
 ******************************************************************************
 */
#ifndef APP_FLASH_STORE_H
#define APP_FLASH_STORE_H

#include <stdint.h>

/* 擦除 page_addr 所在 2KB 页（单 bank，页号按 Flash 基址自动换算）。 */
int Fs_ErasePage(uint32_t page_addr);

/* 向 addr 写入 len 字节（len 必须为 8 的倍数），自动解锁/上锁。 */
int Fs_ProgramDwords(uint32_t addr, const uint8_t *src, uint32_t len);

#endif /* APP_FLASH_STORE_H */
