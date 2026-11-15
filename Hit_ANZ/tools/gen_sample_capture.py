# -*- coding: utf-8 -*-
"""生成测试抓包 test/sample_capture.bin（无硬件验证 解析→绘图 全链路）。

内容按真机字节流形态排布（文本日志行与二进制帧交错）：

  文本 banner
  帧 trig=41  type=1 训练帧（flags=0x0C）
  文本 [CONF] trained / [HIT]
  帧 trig=42  type=1 接受帧（flags=0x0B）
  帧 trig=43  CRC 故意损坏
  帧 trig=44  只写前 900 字节（模拟串口丢字节截断）
  文本 [CONF] rej corr=163
  帧 trig=45  type=2 相关度拒绝（触发样本贴近窗尾，模拟异形敲击）

预期解析结果：有效帧 41/42/45，CRC 错误 2 次（43 损坏 + 44 截断），
trig_id 在 42→45 处跳号（缺 2 帧）。用法：py -3 tools/gen_sample_capture.py
"""
from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from app.protocol import MAGIC, VERSION  # noqa: E402  单一事实来源

SAMPLE_HZ = 3906
P22 = 64
TRUNC_BYTES = 900


def make_wave(n: int = 128, amps=(820000, 470000, 180000, 70000),
              signs=(1, -1, 1, -1), seed: int = 42) -> np.ndarray:
    """合成一次冲击：触发样本后衰减振荡 + 各通道幅值差（模拟近 a 角击打）。"""
    needle = n - 1 - P22
    t = np.arange(n) - needle
    ring = np.zeros(n, dtype=np.float64)
    m = t >= 0
    ring[m] = np.exp(-t[m] / 13.0) * np.sin(2.0 * np.pi * t[m] / 9.0)
    rng = np.random.default_rng(seed)
    chans = [np.round(ring * a * s + rng.normal(0.0, 1500.0, n))
             for a, s in zip(amps, signs)]
    return np.stack(chans, axis=1).astype(np.int32)


def build_frame(trig_id: int, ftype: int = 1, flags: int = 0x0B,
                corr_q10: int = 1024, ts_ms: int = 0,
                wave: np.ndarray | None = None,
                corrupt_crc: bool = False) -> bytes:
    """按固件帧布局组帧（Docs/USART1波形数据帧格式.md §2）。"""
    wave = make_wave() if wave is None else wave
    n = int(wave.shape[0])
    hp_sum = wave.sum(axis=1)
    pk_idx = int(np.argmax(np.abs(hp_sum)))
    peak_ch = tuple(int(np.max(np.abs(wave[:, ch]))) for ch in range(4))
    peak_dif = int(abs(hp_sum[pk_idx]))

    header = MAGIC + struct.pack(
        "<BBBBHIIHII4II",
        VERSION, ftype, flags, 0, n, ts_ms, trig_id, corr_q10,
        pk_idx, peak_dif, *peak_ch, SAMPLE_HZ)
    body = header + wave.astype("<i4").tobytes()
    crc = zlib.crc32(body) & 0xFFFFFFFF
    if corrupt_crc:
        crc ^= 0xDEADBEEF
    return body + struct.pack("<I", crc)


def text(line: str) -> bytes:
    return (line + "\r\n").encode("ascii")


def build_capture() -> bytes:
    parts = [
        text("App skeleton OK (v0.1)"),
        build_frame(41, ftype=1, flags=0x0C, corr_q10=1024, ts_ms=12000),
        text("[CONF] trained n=128 flash_err=0"),
        text("[HIT] sum_peak=412000 p0=820000 p1=470000 p2=180000 p3=70000 "
             "I=87% corr=1024"),
        build_frame(42, ftype=1, flags=0x0B, corr_q10=1019, ts_ms=15400),
        build_frame(43, ftype=1, flags=0x0B, corr_q10=1004, ts_ms=17100,
                    corrupt_crc=True),
        build_frame(44, ftype=1, flags=0x0B, corr_q10=998,
                    ts_ms=18900)[:TRUNC_BYTES],
        text("[CONF] rej corr=163"),
        build_frame(45, ftype=2, flags=0x0B, corr_q10=163, ts_ms=20700,
                    wave=make_wave(amps=(300000, 260000, 240000, 210000),
                                   signs=(1, 1, -1, -1), seed=7)),
    ]
    return b"".join(parts)


def main() -> None:
    out = ROOT / "test" / "sample_capture.bin"
    out.parent.mkdir(parents=True, exist_ok=True)
    data = build_capture()
    out.write_bytes(data)
    print(f"已生成 {out}（{len(data)} 字节）")


if __name__ == "__main__":
    main()
