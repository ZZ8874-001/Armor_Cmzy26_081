# -*- coding: utf-8 -*-
"""解析器 CLI 自测（零 Qt 依赖）：py -3 tools/selftest_parser.py

覆盖：整块 vs 随机分块 vs 逐字节喂入结果一致、CRC 错帧重同步、截断帧丢弃、
文本行分流、trig_id 跳号检测、peak_ch 与波形自洽。
"""
from __future__ import annotations

import random
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

from app.protocol import FrameParser, TYPE_ACCEPT, TYPE_CORR_REJECT  # noqa: E402
from app.store import FrameStore  # noqa: E402


def parse_all(data: bytes, chunk_sizes: list[int] | None):
    parser = FrameParser()
    store = FrameStore()
    lines: list[str] = []
    gaps = []

    if chunk_sizes is None:
        chunks = [data]
    else:
        chunks, pos, idx = [], 0, 0
        while pos < len(data):
            size = chunk_sizes[idx] if idx < len(chunk_sizes) else 4096
            idx += 1
            chunks.append(data[pos: pos + size])
            pos += size

    for chunk in chunks:
        frames, new_lines = parser.feed(chunk)
        lines.extend(new_lines)
        for frame in frames:
            gap = store.add_frame(frame)
            if gap is not None:
                gaps.append(gap)
    return parser, store, lines, gaps


def check(data: bytes, label: str, chunk_sizes: list[int] | None):
    parser, store, lines, gaps = parse_all(data, chunk_sizes)

    trig = [f.trig_id for f in store.frames]
    assert trig == [41, 42, 45], f"[{label}] trig_id {trig}"
    types = [f.type for f in store.frames]
    assert types == [TYPE_ACCEPT, TYPE_ACCEPT, TYPE_CORR_REJECT], f"[{label}] types {types}"
    assert store.frames[0].wave.shape == (128, 4), f"[{label}] 波形形状"
    assert parser.stats.crc_errors == 2, f"[{label}] crc_errors {parser.stats.crc_errors}"
    assert len(gaps) == 1, f"[{label}] 跳号事件 {gaps}"
    assert gaps[0].missing == 2 and not gaps[0].restart, f"[{label}] 跳号内容 {gaps[0]}"

    joined = "\n".join(lines)
    for needle in ("App skeleton OK", "[HIT]", "[CONF] trained"):
        assert needle in joined, f"[{label}] 缺少文本行 {needle!r}"
    # 坏帧（43/44）之后、重同步成功之前的文本行「[CONF] rej」按设计被静默丢弃
    assert "[CONF] rej" not in joined, f"[{label}] 重同步区间文本未被丢弃"
    assert parser.stats.text_lines == 3, f"[{label}] 文本行数 {parser.stats.text_lines}"

    # peak_ch 与波形自洽
    frame = store.frames[0]
    recomputed = tuple(int(np.max(np.abs(frame.wave[:, ch]))) for ch in range(4))
    assert recomputed == tuple(int(v) for v in frame.peak_ch), \
        f"[{label}] peak_ch {frame.peak_ch} vs {recomputed}"


def main() -> None:
    capture = ROOT / "test" / "sample_capture.bin"
    if not capture.exists():
        sys.exit(f"缺少 {capture}，先运行 py -3 tools/gen_sample_capture.py")
    data = capture.read_bytes()

    check(data, "整块", None)

    rnd = random.Random(1234)
    check(data, "随机分块", [rnd.randint(1, 337) for _ in range(len(data))])
    check(data, "逐字节", [1] * len(data))

    # 纯文本流不产生帧；无换行结尾的半行保留到下一次
    _p, store, lines, _gaps = parse_all(b"hello\r\nworld\r\nwor", None)
    assert not store.frames and lines == ["hello", "world"], lines
    _p, store, lines, _gaps = parse_all(b"hello\r\nworld\r\nend\n", None)
    assert not store.frames and lines == ["hello", "world", "end"], lines

    print("parser selftest OK：帧 41/42/45，CRC 错 2，跳号缺 2，三种分块一致")


if __name__ == "__main__":
    main()
