/**
 ******************************************************************************
 * @file    board_variant.h
 * @brief   硬件/传感器变体的编译期选择（v1.0.0 发布矩阵）。
 *
 * 变体由构建系统注入（app.mk 的 ARMOR_VARIANT，经 VAR_DEFS 转成 -D）：
 *   082_500g : -DARMOR_HW_082 -DARMOR_SENSOR_500G   （HSI16 /2，PGA ×1）
 *   083_500g : -DARMOR_HW_083 -DARMOR_SENSOR_500G   （HSE 8MHz，PGA ×1）
 *   083_2kg  : -DARMOR_HW_083 -DARMOR_SENSOR_2KG   （HSE 8MHz，PGA ×4，默认）
 *
 * 本头文件只做编译期常量与组合校验，不引入任何依赖；Core/Src/main.c 与
 * Core/Src/stm32l4xx_hal_msp.c 的时钟守卫也消费这里的 ARMOR_HW_082。
 * 注意：Core 侧守卫会被 CubeMX 重生成覆盖——见 CLAUDE.md 再生成清单第 ④ 条。
 ******************************************************************************
 */
#ifndef APP_BOARD_VARIANT_H
#define APP_BOARD_VARIANT_H

/* ---- 组合校验（编译期拦截非法矩阵） ---- */
#if !defined(ARMOR_HW_082) && !defined(ARMOR_HW_083)
#error "必须定义 ARMOR_HW_082 或 ARMOR_HW_083（用 make ARMOR_VARIANT=... 或 app.mk 默认）"
#endif
#if defined(ARMOR_HW_082) && defined(ARMOR_HW_083)
#error "ARMOR_HW_082 与 ARMOR_HW_083 互斥"
#endif
#if !defined(ARMOR_SENSOR_500G) && !defined(ARMOR_SENSOR_2KG)
#error "必须定义 ARMOR_SENSOR_500G 或 ARMOR_SENSOR_2KG"
#endif
#if defined(ARMOR_SENSOR_500G) && defined(ARMOR_SENSOR_2KG)
#error "ARMOR_SENSOR_500G 与 ARMOR_SENSOR_2KG 互斥"
#endif
#if defined(ARMOR_HW_082) && defined(ARMOR_SENSOR_2KG)
#error "082 硬件仅适配 500g 传感器（2kg 请用 083 变体）"
#endif

/* ---- ADC 前端增益（ADS131M04 GAIN1，地址 0x04，每通道 4bit：000=×1..011=×8） ----
 * 500g 传感器用 ×1；2kg 传感器灵敏度 ≈1/4，用 ×4 补偿 → 同力度 counts 量级一致。 */
#if defined(ARMOR_SENSOR_2KG)
#define ADS_GAIN1_VALUE   0x2222u
#define ADS_PGA_GAIN      4u
#else
#define ADS_GAIN1_VALUE   0x0000u
#define ADS_PGA_GAIN      1u
#endif

/* ---- 标识串（启动横幅/发布说明用；变体无法从 CAN/状态帧读出） ---- */
#if defined(ARMOR_HW_082)
#define ARMOR_HW_STR      "082"
#else
#define ARMOR_HW_STR      "083"
#endif

#if defined(ARMOR_SENSOR_2KG)
#define ARMOR_SENSOR_STR  "2kg"
#else
#define ARMOR_SENSOR_STR  "500g"
#endif

#define ARMOR_VARIANT_STR ARMOR_HW_STR "_" ARMOR_SENSOR_STR

/* 时钟来源（与 Core 侧守卫一致）：082 = HSI16/2，083 = HSE 8MHz */
#if defined(ARMOR_HW_082)
#define ARMOR_CLK_STR     "HSI16/2"
#else
#define ARMOR_CLK_STR     "HSE8"
#endif

#endif /* APP_BOARD_VARIANT_H */
