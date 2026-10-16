/**
 ******************************************************************************
 * @file    ref_store.h
 * @brief   击打参考样本的 Flash 持久化（设备最后一页 2KB 专用页）。
 *
 * 用途：上电校准流程把"第一次正常击打"的锁帧合力样本存为参考，跨断电保留；
 * 后续击打与它做归一化互相关（Pearson），按相关度判真伪。
 *
 * 掉电安全策略（页内始终以"有效内容或全 0xFF"结束，绝不出现半写状态被误用）：
 *   上电  RefStore_Load()：校验 → 拷到 RAM → **立即擦页**（为本次上电腾出编程空间）
 *   训练  RefStore_Save()：把新样本编程进空页
 *   超时  RefStore_Save()：把 RAM 里的旧值回写
 * 于是每上电最多 1 次擦除 + 1 次编程，Flash 寿命无忧；擦除与编程之间的掉电
 * 等价于"Flash 为空"，下次上电走首次训练路径（用户可见后果 = 再紫闪一次）。
 *
 * 调用约束：
 *   - `RefStore_Load` 含页擦除（~25ms 阻塞），**只能在 App_Init 内、TIM2 启动前**调用；
 *   - `RefStore_Save` 为纯编程（~6ms 阻塞），只能在**主循环上下文**调用，禁止 ISR；
 *   - 阻塞期间 DRDY/DMA 仍会推进，主循环按 drop_cnt 自恢复（管线不依赖零丢帧）。
 ******************************************************************************
 */
#ifndef APP_BSP_REF_STORE_H
#define APP_BSP_REF_STORE_H

#include <stdint.h>
#include <stdbool.h>

/* 单次可存取的样本数上限（与 detect 层冻结窗上限同量级，当前锁帧窗 64） */
#define REF_STORE_MAX  128u

/* 上电载入：校验末页内容，有效则把样本拷入 sum（容量须 ≥ REF_STORE_MAX）
 * 并写回 *n；随后**无条件擦除该页**。返回 true = 拿到了有效参考。
 * sum/n 任一为 NULL 时直接返回 false 且不碰 Flash。 */
bool RefStore_Load(int32_t *sum, uint16_t *n);

/* 落盘（纯编程，页须处于已擦状态；异常调用序下会自动补擦一次）。
 * 返回 0 = 成功，-1 = 参数非法/擦除失败/编程失败。 */
int  RefStore_Save(const int32_t *sum, uint16_t n);

/* 诊断标量（Ozone 直接 watch）：0=无错，1=擦除失败，2=编程失败，3=解锁失败 */
uint32_t RefStore_GetErr(void);

#endif /* APP_BSP_REF_STORE_H */
