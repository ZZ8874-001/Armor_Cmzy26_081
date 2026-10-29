# USART1 冻结窗波形数据帧格式

版本 **v1.0**（2026-10-28）。权威实现：`App/app/main_app.c` 的 `Wave_BuildAndSend`（组帧 + CRC）
与 `App/detect/hit_detect.c` 的 `Hit_ConfirmRun`（快照）/`HitDetect_GetWaveTx`（导出）。
本文件与代码不一致时以代码为准，并回来修正本文件。

## 1. 概述

| 项 | 值 |
|---|---|
| 物理层 | USART1，PA9 TX / PA10 RX（连接器 U18），**921600 8N1**，固件只用 TX |
| 波特率误差 | 80 MHz PCLK / 921600 → BRR=87 → 实际 **919540 Bd（−0.22%）**，USB-UART 适配器均可容忍 |
| 发送时机 | **每次冻结窗确认完成后，一确认一帧**：接受、相关度拒绝、校准合法性拒绝三种结果都发 |
| 不发帧的情况 | 冻结异常中止（`Hit_ConfirmAbort`）→ 上位机由 `trig_id` 跳号发现 |
| 搬运方式 | USART1 TX DMA（**DMA2_Channel6**，NORMAL 模式，BYTE/BYTE），软件只组帧 + 启动 |
| 帧长 | `48 + 16n + 4` 字节；默认 n=128 → **2100 B ≈ 22.8 ms @921600** |
| 并发策略 | 深度 1：TX 在飞时快照保留、下一主循环趟重试（**绝不覆写 DMA 在飞缓冲**）；仅 DMA 启动失败才丢弃（计数 `s_wave_drop`）。P05 不应期 ≥200 ms ≫ 22.8 ms，正常无重叠 |
| 与文本日志共存 | 同一 TX 上文本行与二进制帧交错；解析器按魔数扫描 + 长度 + CRC 区分，无需流完整性 |

帧周期约束：一次击打一次触发一次确认，确认距下一次最早触发 ≥ P05 不应期（当前 200 ms），
DMA 一帧占 22.8 ms，带宽占用约 11%。

## 2. 帧格式

全字段**小端**；波形载荷为样本主序 `[sample][ch]`。

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | `magic` | ASCII `WAV1`（小端 u32 = 0x31564157） |
| 4 | 1 | `version` | 1 |
| 5 | 1 | `type` | 1=接受 2=相关度拒绝 3=校准合法性拒绝（见 §3） |
| 6 | 1 | `flags` | 位域，见 §3.2 |
| 7 | 1 | `reserved` | 0 |
| 8 | 2 | `win_len` = n | 窗长（样本数），默认 128；有效范围 1–128 |
| 10 | 4 | `ts_ms` | 触发时刻 `HAL_GetTick()`（ms，u32 约 49.7 天回绕） |
| 14 | 4 | `trig_id` | 总触发号（自开机累计，**含被拒与中止**） |
| 18 | 2 | `corr_q10` | 相关度 \|Pearson ρ\|×1024；0=不可用（热身/无参考/窗长失配） |
| 20 | 4 | `pk_idx` | 窗内 \|hp_sum\| 峰值索引（取首个最大值） |
| 24 | 4 | `peak_dif` | \|dif_sum\| 峰值（= 事件 `peak` 字段 = `[HIT]` 日志 `sum_peak=`） |
| 28 | 16 | `peak_ch[4]` | 各通道 \|hp\| 峰值（= 事件 `peak_ch[4]`） |
| 44 | 4 | `sample_hz` | 标称采样率 **3906**（实际 3906.25 = 8 MHz/2048），作图用 |
| 48 | 16n | `wave[n][4]` | 4 通道 hp 波形，`int32` 小端，样本主序 |
| 48+16n | 4 | `crc32` | 反射式 CRC-32，覆盖 `[0, 48+16n)`，见 §4 |

## 3. 字段语义

### 3.1 `type`

| 值 | 含义 | 对应文本日志 | 是否进状态机/CAN |
|---:|---|---|---|
| 1 | **接受**（相关度 ≥ P27，或判据让路：热身未满/无参考/本上电未训练/P27=0） | `[HIT]`（P18 bit0 开启时） | 是 |
| 2 | **相关度拒绝**（corr < P27，形状不像已知击打） | `[CONF] rej corr=` | 否 |
| 3 | **校准合法性拒绝**（击打本身被接受，但 \|hp_sum\| 峰值距窗首/尾 <5 样本，本次未训练参考、机会留给下一击） | `[CONF] cal-rej pk=`（另有 `[HIT]`，因为它被接受了） | 是 |

三种结果都来自一次完整确认，波形都是同一冻结窗（对 `type` 无差别）。

### 3.2 `flags`

| 位 | 含义 |
|---:|---|
| bit0 | 确认**时**参考有效（本次确认的 corr 有资格参与判拒的前提之一） |
| bit1 | 本次确认**之前**本上电已训练 |
| bit2 | **本次确认完成了训练**（该帧的冻结窗已固化为参考并写 Flash；至多出现一次/上电） |
| bit3 | 基线 EMA 热身已满（5τ；未满时判据不判拒、不训练） |

常见组合（读位不背数）：

- 热身未满期间的击打：bit3=0，bit0/bit1/bit2 一般都为 0（该窗不判拒、不训练）。
- 上电首记热身后接受的击打：bit3=1，bit2=1（本次训练）；bit0 取决于上电时 Flash 是否已有参考
  （Flash 空 → 0；有旧参考 → 1），bit1=0。
- 其后的接受：bit0=1、bit1=1、bit3=1、bit2=0。
- 峰值贴窗边未训练的接受：type=3，bit2=0、bit1=0，bit0/bit3 按上面规则。

### 3.3 波形数据

- `wave[i][ch]` = **hp**（`raw − EMA 基线`），与触发判据所用差分信号**无关**（未差分、未归一化）；
  单位 ADC counts（24 位有符号，前端增益由 ADS131M04 `GAIN1` 决定）。
- 样本 0 = 窗口最早点；`n−1` = 最后一个触发后样本。
- **触发样本索引 = `n − P22 − 1`**（默认 n=128、P22=64 → 索引 63；其前为 P21 预触发段）。
- `pk_idx` 与触发索引不要混淆：前者是窗内 `|hp_sum|` 的峰值位置，后者是触发条件成立的位置。
- 窗长/触发位置随参数 P21/P22 变化，解析器以 `win_len` 为准，不要硬编码 128。

### 3.4 `trig_id`

自开机起的总触发计数（`s_hit_trig_cnt`），**每进一次 CONFIRMING 就 +1**，与结果无关。
因此：

- 相邻帧 `trig_id` 通常差 1；**跳号** = 中间那些触发没有发帧：
  ① 冻结异常中止（`s_conf_abort_cnt`，固件不发帧的既定路径）；
  ② DMA 启动失败丢弃（`s_wave_drop`，正常为 0）。
- 跳号**不代表数据丢失**（中止本来就不送），判读见 §6。

## 4. CRC32

- 参数：反射式多项式 **0xEDB88320**，初值 0xFFFFFFFF，结果取反，输出小端 u32。
- 覆盖范围：`[0, 48+16n)`（头 + 载荷，不含 CRC 自身）。
- 与 Python `zlib.crc32(frame[:-4]) & 0xFFFFFFFF` **逐字节一致**。
- 固件：`main_app.c` 的 `Wave_Crc32`（位运算实现，≈2 ms/帧）。与 `bsp/ref_store.c`
  的 `Ref_Crc32` 同算法（后者用于 Flash 参考样本），按分层约定不共享符号。

## 5. 解析与重同步

推荐流程（任意位置开始接收都能同步）：

1. 在字节流中搜索 ASCII `WAV1`；
2. 读偏移 8 的 `win_len`，要求 `1 ≤ n ≤ 128`；
3. 校验 `version == 1`，且剩余字节 ≥ `48+16n+4`；
4. 计算 CRC32 与帧尾比对；
5. 通过 → 消费整帧，从帧尾继续搜索；任一校验失败 → 从魔数后 1 字节继续搜索。

文本日志不会输出 `WAV1` 字样；伪魔数只可能来自波形载荷内部，由 CRC 排除。
串口丢字节时解析器自动重新同步，但**被截断的帧会丢失**（该帧计数看不见，看 `trig_id` 跳号）。

## 6. 对账与诊断

固件内计数（Ozone 观测）满足：

```
s_hit_trig_cnt = s_hit_count(接受) + s_conf_reject_cnt(相关度拒) + s_conf_abort_cnt(中止)
```

线上帧数 = 确认数 − `s_wave_drop`（发送忙丢弃）。

| 现象 | 判读 |
|---|---|
| `trig_id` 跳 1 且无帧 | 中止（`s_conf_abort_cnt` 增）或 DMA 启动失败（`s_wave_drop` 增） |
| 收到 type=2 帧 | 相关度判拒，同帧前后有 `[CONF] rej corr=` 文本行 |
| 收到 type=3 帧 | 峰值贴窗边，训练机会保留；对应 `[CONF] cal-rej pk=` |
| type=1 且 flags bit2=1 | 该帧同时是本次上电的训练帧，对应 `[CONF] trained n=128 flash_err=0` |
| `[WAVE] drop type=… trig=…` 文本行 | 固件侧丢弃（DMA 启动失败，正常情况下不应出现） |
| `[WAVE] tx timeout, aborted` 文本行 | 看门狗强制中止：DMA 启动后 100 ms 完成回调仍未到（`s_wave_tx_stuck` 增）。正常路径永不出现——出现即说明 **USART1 全局中断没工作**（典型原因：CubeMX 重生成把 `NVIC.USART1_IRQn` / `USART1_IRQHandler` 弄丢了；NORMAL 模式 DMA 搬完只置 TCIE，完成回调必须在 USART1 中断里执行）。此时波形帧每帧超时丢弃、文本日志尚可恢复；修好中断后自动正常 |
| 第一次击打有帧、之后 `s_wave_tx_cnt` 恒 1 且串口全静默 | 同上：完成回调不执行 → `s_wave_tx_busy` 永远为 1。旧固件无看门狗时连文本通路一起被门控（整个串口静默）；看门狗上线后表现为每帧 `tx timeout` + `s_wave_tx_stuck` 递增 |

帧与文本日志的对应：`[HIT]` 行的 `sum_peak=` = 帧的 `peak_dif`、`corr=` = `corr_q10`。

## 7. Ozone 调试变量

| 变量 | 层 | 含义 |
|---|---|---|
| `s_wave_tx_pending` | hit_detect.c | 1=有未取走的确认快照（深度 1，主循环下一趟即取走） |
| `s_wave_tx_info` | hit_detect.c | 最近一次确认的元数据快照（type/flags/trig_id/corr/pk_idx/peak_*） |
| `s_wave_tx_busy` | app_log.c | 1=DMA 在飞（FlushSmall 门控 + 主循环取快照的忙判据） |
| `s_wave_tx_cnt` | app_log.c | 累计成功启动的帧数（应 = 上位机收帧数） |
| `s_wave_drop` | app_log.c | DMA 启动失败丢弃的帧数（应为 0） |
| `s_wave_tx_start_ms` | app_log.c | 本次 DMA 启动时刻（`HAL_GetTick()`，看门狗计时） |
| `s_wave_tx_stuck` | app_log.c | 看门狗强制中止次数（应为 0；非 0 = USART1 完成中断失效，见 §6 末两行） |

## 8. Python 参考解析脚本

依赖：`numpy`。用法：串口工具以原始二进制记录（PuTTY *All session output* 或 pyserial 读块落盘），
然后：

```bash
python parse_wave.py capture.bin            # 逐帧摘要（含 trig_id 跳号提示）
python parse_wave.py capture.bin -o out     # 每帧存 out/wav_<trig_id>.npz
```

```python
#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""USART1 'WAV1' 波形帧解析参考实现（配 Docs/USART1波形数据帧格式.md）。"""
import argparse
import pathlib
import struct
import zlib

import numpy as np

MAGIC = b"WAV1"
HDR = 48
MAX_N = 128
VERSION = 1


def parse_frames(buf):
    frames = []
    pos = 0
    while True:
        i = buf.find(MAGIC, pos)
        if i < 0 or i + HDR > len(buf):
            break
        ver, ftype, flags, _rsv = struct.unpack_from("<BBBB", buf, i + 4)
        n, ts_ms, trig_id, corr_q10, pk_idx, peak_dif = struct.unpack_from(
            "<HIIHII", buf, i + 8)
        peak_ch = struct.unpack_from("<4I", buf, i + 28)
        (sample_hz,) = struct.unpack_from("<I", buf, i + 44)

        end = i + HDR + 16 * n + 4
        if ver != VERSION or not (1 <= n <= MAX_N) or end > len(buf):
            pos = i + 1
            continue
        (crc_rx,) = struct.unpack_from("<I", buf, end - 4)
        if crc_rx != zlib.crc32(buf[i:end - 4]) & 0xFFFFFFFF:
            pos = i + 1
            continue

        wave = np.frombuffer(buf, dtype="<i4", count=n * 4,
                             offset=i + HDR).reshape(n, 4)
        frames.append(dict(version=ver, type=ftype, flags=flags, n=n,
                           ts_ms=ts_ms, trig_id=trig_id, corr_q10=corr_q10,
                           pk_idx=pk_idx, peak_dif=peak_dif, peak_ch=peak_ch,
                           sample_hz=sample_hz, wave=wave))
        pos = end
    return frames


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture", type=pathlib.Path)
    ap.add_argument("-o", "--outdir", type=pathlib.Path)
    args = ap.parse_args()

    frames = parse_frames(args.capture.read_bytes())
    prev = None
    for f in frames:
        gap = "" if prev is None or f["trig_id"] == prev + 1 else f"  <-- gap, prev={prev}"
        print(f"trig={f['trig_id']:6d} type={f['type']} flags=0x{f['flags']:02x} "
              f"n={f['n']:3d} corr={f['corr_q10']:4d} pk={f['pk_idx']:3d} "
              f"peak_dif={f['peak_dif']:8d} ts={f['ts_ms']}{gap}")
        prev = f["trig_id"]
        if args.outdir:
            args.outdir.mkdir(parents=True, exist_ok=True)
            np.savez_compressed(
                args.outdir / f"wav_{f['trig_id']:06d}.npz",
                wave=f["wave"], sample_hz=f["sample_hz"], type=f["type"],
                flags=f["flags"], corr_q10=f["corr_q10"], pk_idx=f["pk_idx"],
                peak_dif=f["peak_dif"], peak_ch=f["peak_ch"],
                ts_ms=f["ts_ms"], trig_id=f["trig_id"])
    print(f"共解析 {len(frames)} 帧")


if __name__ == "__main__":
    main()
```

## 9. 变更记录

| 版本 | 日期 | 说明 |
|---|---|---|
| v1.0 | 2026-10-28 | 初版：确认后逐帧上传 4 通道 hp 波形；USART1 115200→921600，TX DMA2_Channel6 |
