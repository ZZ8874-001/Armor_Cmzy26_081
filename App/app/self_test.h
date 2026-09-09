/**
 ******************************************************************************
 * @file    self_test.h
 * @brief   非阻塞自检接口（设计文档 v1.5 第 7.6 节）。
 *
 * 流程（主循环每圈步进一次，单步 <1ms，总时长 ≈1.1s，超时 5s abort）：
 *   ST_REGS（寄存器回读校验）→ ST_DRDY（1s DRDY 频率）→ ST_STATS（256 帧
 *   噪声/基线统计）→ ST_TEMP（温度窗检）→ ST_DONE（诊断增量 + 结果固化）。
 * 运行期间自检独占帧消费并持续喂 HitDetect（检测管线不饿、环形不溢出）。
 * DAC 注入步骤不在本模块范围（设计文档 7.6 可选开关）。
 ******************************************************************************
 */
#ifndef APP_SELF_TEST_H
#define APP_SELF_TEST_H

#include <stdint.h>
#include <stdbool.h>
#include "detect/calibration.h"   /* hit_param_t */

/* 失败位图（self_test_t.fails） */
#define ST_FAIL_INIT     (1u << 0)   /* ADC 初始化失败（IsInitOk=0） */
#define ST_FAIL_REGS     (1u << 1)   /* 寄存器回读校验失败 */
#define ST_FAIL_DRDY     (1u << 2)   /* DRDY 频率出界（3906±20%） */
#define ST_FAIL_NOISE    (1u << 3)   /* 噪声/基线统计不过 */
#define ST_FAIL_TEMP     (1u << 4)   /* TEMP 出窗 */
#define ST_FAIL_DIAG     (1u << 5)   /* 自检期间 spi_err/drop 增量 */
#define ST_FAIL_TIMEOUT  (1u << 6)   /* 总时长超限 abort */

/* 总体结果 */
#define ST_RESULT_PASS    0u
#define ST_RESULT_FAIL    1u

typedef struct
{
    uint8_t  overall;         /* ST_RESULT_PASS / ST_RESULT_FAIL */
    uint8_t  fails;           /* ST_FAIL_* 位图 */
    uint32_t drdy_hz;         /* 实测 DRDY 频率 */
    int32_t  mean[4];         /* 4 通道均值 */
    int32_t  rms[4];          /* 4 通道噪声 RMS */
    uint16_t temp_mv[4];      /* 4 路 TEMP（mV） */
} self_test_t;

/* 初始化（载入参数副本），上电时调用一次。 */
void SelfTest_Init(const hit_param_t *p);

/* 发起自检；运行中返回 false（忽略重复触发）。 */
bool SelfTest_Request(void);

/* 主循环每圈调用：步进自检状态机（IDLE 时立即返回）。 */
void SelfTest_Poll(void);

bool SelfTest_IsRunning(void);

/* 取走最近一次完成的结果（一次性）；无新结果返回 false。 */
bool SelfTest_TakeResult(self_test_t *r);

#endif /* APP_SELF_TEST_H */
