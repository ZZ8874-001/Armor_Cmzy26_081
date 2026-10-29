# -*- coding: utf-8 -*-
"""数据层：WAV1 冻结窗波形帧的流式解析器（零 Qt 依赖，可 CLI 单测）。

协议权威：``Docs/USART1波形数据帧格式.md``。帧 = ASCII ``WAV1`` + 48 B 头 +
n×4×int32 hp 波形 + CRC32，全小端；与固件文本日志行（ASCII，\\r\\n 结尾）
交错在同一 USART1 字节流上。

解析器按「魔数扫描 + 长度校验 + CRC 校验」增量重同步：任意位置开始接收都能
同步；文本行与帧混排自动分流；被截断的帧丢弃并继续找下一帧。算法见文档 §5。

校验失败后的重同步区间是二进制垃圾（坏帧本体），区间内字节**静默丢弃**、
不刷日志；静默丢弃最多一帧长度（2100 B），超过即恢复文本分流——单个坏帧
不会把其后的日志全部吞掉。代价：紧跟在坏帧后、恰好落在该区间内的文本行
会丢失（界面上另有 CRC 告警提示）。
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass

import numpy as np

MAGIC = b"WAV1"
HDR_LEN = 48          # 头长（含魔数）
MAX_N = 128           # win_len 上限
VERSION = 1           # 帧版本

# type 字节取值（文档 §3.1）
TYPE_ACCEPT = 1
TYPE_CORR_REJECT = 2
TYPE_CAL_REJECT = 3
TYPE_NAMES = {
    TYPE_ACCEPT: "接受",
    TYPE_CORR_REJECT: "相关度拒绝",
    TYPE_CAL_REJECT: "校准拒绝",
}

# flags 位（文档 §3.2）
FLAG_REF_VALID = 0x01        # 确认时参考有效
FLAG_TRAINED_BEFORE = 0x02   # 确认前本上电已训练
FLAG_TRAINED_NOW = 0x04      # 本次确认完成了训练
FLAG_WARMUP = 0x08           # 基线热身已满

# 文本行缓冲上限：超过即整块丢弃并记一条替换行（垃圾流兜底）
_TEXT_CAP = 64 * 1024
# 重同步区丢弃上限：一次 CRC/头校验失败后，最多静默丢弃一帧的字节数
# （帧内字节是二进制垃圾，混入日志只会刷屏；超过一帧长度即恢复文本分流，
#  避免单个坏帧把后续所有日志都吞掉）
_RESYNC_LIMIT = HDR_LEN + 16 * MAX_N + 4


@dataclass
class Frame:
    """一帧确认后的冻结窗波形 + 元数据。"""

    type: int
    flags: int
    n: int
    ts_ms: int
    trig_id: int
    corr_q10: int
    pk_idx: int
    peak_dif: int
    peak_ch: tuple
    sample_hz: int
    wave: np.ndarray              # (n, 4) int32，样本主序，列 = ch1..ch4
    seq: int = 0                  # 本程序接收序号（0 起），回看排序用

    @property
    def type_name(self) -> str:
        return TYPE_NAMES.get(self.type, f"未知({self.type})")

    def trig_index(self, p22: int) -> int:
        """触发样本在窗内索引 = n−P22−1（文档 §3.3），夹到有效范围。"""
        return max(0, min(self.n - 1, self.n - 1 - int(p22)))

    def summary(self) -> str:
        return (f"trig={self.trig_id} {self.type_name} corr={self.corr_q10} "
                f"pk={self.pk_idx} peak={self.peak_dif} flags=0x{self.flags:02x}")


@dataclass
class ParseStats:
    bytes_in: int = 0
    frames_total: int = 0
    text_lines: int = 0
    crc_errors: int = 0          # CRC 校验失败次数
    resyncs: int = 0             # 头非法/CRC 失败导致的逐字节重同步次数


class FrameParser:
    """增量解析：每次 feed() 返回本次新产出的 (frames, text_lines)。"""

    def __init__(self) -> None:
        self._buf = bytearray()
        self._text = bytearray()
        self._resync = False        # True = 上次帧校验失败，正跳过坏帧区间
        self._resync_skipped = 0
        self.stats = ParseStats()

    def reset(self) -> None:
        self._buf.clear()
        self._text.clear()
        self._resync = False
        self._resync_skipped = 0
        self.stats = ParseStats()

    # ------------------------------------------------------------------
    def _drop(self, n: int, lines: list[str]) -> None:
        """丢弃 n 字节；非重同步区间内按文本行分流，重同步区间内静默丢弃。"""
        if n <= 0:
            return
        chunk = bytes(self._buf[:n])
        del self._buf[:n]
        if self._resync:
            self._resync_skipped += n
            if self._resync_skipped >= _RESYNC_LIMIT:
                self._resync = False        # 已越过一帧长度：恢复文本分流
                self._resync_skipped = 0
            return
        self._emit_text(chunk, lines)

    # ------------------------------------------------------------------
    def feed(self, data: bytes):
        """喂入一段字节，返回 (frames: list[Frame], lines: list[str])。"""
        if data:
            self._buf += data
            self.stats.bytes_in += len(data)

        frames: list[Frame] = []
        lines: list[str] = []

        while True:
            idx = self._buf.find(MAGIC)
            if idx < 0:
                # 无完整魔数：只保留末尾「魔数前缀」的若干字节（可能被分块切断），
                # 其余全部当文本处理——普通文本不因等魔数而被拖延
                keep = 0
                for k in range(min(len(self._buf), len(MAGIC) - 1), 0, -1):
                    if self._buf[-k:] == MAGIC[:k]:
                        keep = k
                        break
                self._drop(len(self._buf) - keep, lines)
                break
            if idx > 0:
                self._drop(idx, lines)

            # 此时 _buf 以魔数开头
            if len(self._buf) < HDR_LEN:
                break
            ver = self._buf[4]
            (n,) = struct.unpack_from("<H", self._buf, 8)
            if ver != VERSION or not (1 <= n <= MAX_N):
                self.stats.resyncs += 1
                self._enter_resync()
                del self._buf[:1]          # 跳过魔数首字节，继续搜索
                continue
            total = HDR_LEN + 16 * n + 4
            if len(self._buf) < total:
                break                      # 等后续字节
            (crc_rx,) = struct.unpack_from("<I", self._buf, total - 4)
            if crc_rx != zlib.crc32(self._buf[: total - 4]) & 0xFFFFFFFF:
                self.stats.crc_errors += 1
                self.stats.resyncs += 1
                self._enter_resync()
                del self._buf[:1]
                continue

            frames.append(self._parse_frame(n))
            del self._buf[:total]
            self.stats.frames_total += 1
            self._resync = False           # 重新同步成功：恢复文本分流
            self._resync_skipped = 0

        return frames, lines

    def _enter_resync(self) -> None:
        if not self._resync:
            self._resync = True
            self._resync_skipped = 0

    # ------------------------------------------------------------------
    def _parse_frame(self, n: int) -> Frame:
        b = self._buf
        ver, ftype, flags, _rsv = struct.unpack_from("<BBBB", b, 4)
        (_n, ts_ms, trig_id, corr_q10, pk_idx, peak_dif) = struct.unpack_from(
            "<HIIHII", b, 8)
        peak_ch = struct.unpack_from("<4I", b, 28)
        (sample_hz,) = struct.unpack_from("<I", b, 44)
        wave = np.frombuffer(bytes(b[HDR_LEN: HDR_LEN + 16 * n]),
                             dtype="<i4").reshape(n, 4).copy()
        return Frame(type=ftype, flags=flags, n=n, ts_ms=ts_ms,
                     trig_id=trig_id, corr_q10=corr_q10, pk_idx=pk_idx,
                     peak_dif=peak_dif, peak_ch=peak_ch,
                     sample_hz=sample_hz, wave=wave)

    # ------------------------------------------------------------------
    def _emit_text(self, raw: bytes, lines: list[str]) -> None:
        """把魔数之前的字节按行切分（半行保留到下一次）。"""
        self._text += raw
        if len(self._text) > _TEXT_CAP:
            lines.append(f"（丢弃 {len(self._text)} 字节无法识别的数据）")
            self._text.clear()
            return
        while True:
            pos = self._text.find(b"\n")
            if pos < 0:
                break
            line = bytes(self._text[:pos]).rstrip(b"\r")
            del self._text[: pos + 1]
            if line:
                lines.append(line.decode("utf-8", errors="replace"))
                self.stats.text_lines += 1
