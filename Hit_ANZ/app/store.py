# -*- coding: utf-8 -*-
"""数据层：帧存储 + trig_id 跳号检测 + 原始流缓冲（零 Qt 依赖）。

当前固件按击打发送冻结窗帧（非连续流），所以这里是**帧存储器**而非连续采样
环形缓冲：容量有界的最近 N 帧 + 原始字节环形缓冲（供导出 .bin 复查）。
将来固件若加连续原始帧模式，再在此挂接连续环形缓冲（NumPy 取模覆盖）。
"""
from __future__ import annotations

from collections import deque
from dataclasses import dataclass

from .protocol import Frame

MAX_FRAMES = 200                 # 最近帧数上限（≈400KB 内存）
MAX_RAW = 64 * 1024 * 1024       # 原始流缓冲上限 64MB


@dataclass
class GapEvent:
    prev_trig: int
    cur_trig: int
    missing: int                 # 缺失帧数（cur−prev−1；重启时为负差值）
    restart: bool = False        # True = trig_id 回退，判为下位机重新上电

    def message(self) -> str:
        if self.restart:
            return (f"⚠ 下位机重新计数：trig_id {self.prev_trig} → "
                    f"{self.cur_trig}（疑似重新上电）")
        return (f"⚠ trig_id 跳号：{self.prev_trig} → {self.cur_trig}"
                f"（缺 {self.missing} 帧：冻结中止或固件 DMA 丢弃，见协议文档 §6）")


class FrameStore:
    def __init__(self) -> None:
        self.frames: deque[Frame] = deque(maxlen=MAX_FRAMES)
        self.raw = bytearray()
        self.raw_truncated = False
        self._last_trig: int | None = None
        self._seq = 0

    # ------------------------------------------------------------------
    def clear(self) -> None:
        self.frames.clear()
        self.raw.clear()
        self.raw_truncated = False
        self._last_trig = None
        self._seq = 0

    # ------------------------------------------------------------------
    def add_raw(self, chunk: bytes) -> None:
        self.raw += chunk
        if len(self.raw) > MAX_RAW:
            del self.raw[: len(self.raw) - MAX_RAW]
            self.raw_truncated = True

    def add_frame(self, frame: Frame) -> GapEvent | None:
        """存入一帧，返回跳号事件（连续则 None）。"""
        frame.seq = self._seq
        self._seq += 1
        gap = None
        if self._last_trig is not None and frame.trig_id != self._last_trig + 1:
            gap = GapEvent(prev_trig=self._last_trig, cur_trig=frame.trig_id,
                           missing=frame.trig_id - self._last_trig - 1,
                           restart=frame.trig_id <= self._last_trig)
        self._last_trig = frame.trig_id
        self.frames.append(frame)
        return gap

    # ------------------------------------------------------------------
    def latest(self) -> Frame | None:
        return self.frames[-1] if self.frames else None

    def at(self, index: int) -> Frame | None:
        """按接收序号 seq 取帧（deque 可能已滚掉旧帧）。"""
        for f in reversed(self.frames):
            if f.seq == index:
                return f
        return None
