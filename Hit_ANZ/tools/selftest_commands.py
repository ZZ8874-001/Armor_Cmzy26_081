# -*- coding: utf-8 -*-
"""HCMD 命令帧自测（零 Qt、无硬件）：

    py -3 tools/selftest_commands.py

断言 PARAM_SPECS 表与固件 host_cmd.c 的 HC_* 范围同步、帧构造/CRC、
SET_REFERENCE 载荷（含 int32 回绕）、ACK 行解析、回放器拒发路径。
"""
from __future__ import annotations

import os
import struct
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

from app import commands  # noqa: E402  （导入不得依赖 QApplication）
from app.sources import FileReplayWorker, SerialWorker, SourceWorker  # noqa: E402


def main() -> None:
    # ---- 1. PARAM_SPECS 表与固件双端范围同步（改固件 HC_* 必须改这里） ----
    expect = {
        "P01": (8, 16, 1, 13),
        "P03": (1, 64, 1, 4),
        "P04": (1000, 2_000_000, 500, 25000),
        "P05": (50, 1000, 10, 200),
        "P26": (1, 512, 1, 64),
        "P27": (0, 1024, 1, 609),
    }
    assert [s.key for s in commands.PARAM_SPECS] == list(expect), \
        "PARAM_SPECS 顺序 = 载荷顺序，不得乱动"
    for spec in commands.PARAM_SPECS:
        lo, hi, step, dflt = expect[spec.key]
        assert (spec.lo, spec.hi, spec.step, spec.default) == (lo, hi, step, dflt), \
            f"{spec.key} 表值与固件 HC_* 范围不同步"
    print("  table OK（6 参数范围/步进/默认与固件同步）")

    # ---- 2. 帧构造与 CRC ----
    f = commands.build_get_params()
    assert len(f) == 12, f"GET_PARAMS 应为 12B：{len(f)}"
    assert f[:4] == b"HCMD" and f[4] == 1 and f[5] == commands.CMD_GET_PARAMS
    assert struct.unpack_from("<H", f, 6)[0] == 0
    assert struct.unpack_from("<I", f, 8)[0] == commands.crc32(f[:8])
    bad = bytearray(f)
    bad[2] ^= 0xFF                                   # 破坏 magic
    assert commands.crc32(bytes(bad[:8])) != commands.crc32(f[:8]), "CRC 未感知损坏"
    print("  frame OK（12B GET、magic/版本/cmd/len、CRC 感知损坏）")

    # ---- 3. SET_PARAMS 载荷布局 <BBiHHH ----
    vals = {s.key: s.default for s in commands.PARAM_SPECS}
    sp = commands.build_set_params(vals)
    assert len(sp) == 24, f"SET_PARAMS 应为 8+12+4=24B：{len(sp)}"
    unpacked = struct.unpack_from("<BBiHHH", sp, 8)
    assert unpacked == (13, 4, 25000, 200, 64, 609), f"载荷布局错误：{unpacked}"
    assert struct.unpack_from("<I", sp, 20)[0] == commands.crc32(sp[:20])
    print("  set_params OK（<BBiHHH 布局 + CRC）")

    # ---- 4. 越界拒绝 ----
    for key, bad_val in (("P01", 7), ("P04", 99), ("P27", 1025), ("P05", 2000)):
        v = dict(vals)
        v[key] = bad_val
        try:
            commands.build_set_params(v)
            raise AssertionError(f"{key}={bad_val} 应抛 ValueError")
        except ValueError:
            pass
    print("  range OK（越界值抛 ValueError）")

    # ---- 5. SET_REFERENCE 载荷 ----
    rng = np.random.default_rng(42)
    wave = rng.integers(-2**23, 2**23, size=(128, 4), dtype=np.int64)
    frame = SimpleNamespace(n=128, wave=wave.astype(np.int32))
    rf = commands.build_set_ref(frame)
    payload_len = struct.unpack_from("<H", rf, 6)[0]
    assert payload_len == 2 + 128 * 4, payload_len
    assert struct.unpack_from("<H", rf, 8)[0] == 128
    sums = np.frombuffer(rf[10:10 + 512], dtype="<i4")
    expect_sums = wave.sum(axis=1).astype("<i4")
    assert np.array_equal(sums, expect_sums), "求和窗与 wave.sum(axis=1) 不一致"
    assert struct.unpack_from("<I", rf, 8 + payload_len)[0] == \
        commands.crc32(rf[:8 + payload_len])
    # int32 回绕：人为放大到溢出域
    big = wave * 1000
    big_sums = big.sum(axis=1).astype("<i4")
    assert np.array_equal(
        np.frombuffer(
            commands.build_set_ref(SimpleNamespace(n=128, wave=big.astype(np.int32)))[10:522],
            dtype="<i4"), big_sums), "int32 回绕语义不一致"
    print("  set_ref OK（u16 n + 128×i32 求和窗、CRC、int32 回绕）")

    # ---- 6. ACK 行解析 ----
    good = [
        ("[CMD] ok set-params P01=13 P03=4 P04=25000 P05=200 P26=64 P27=609",
         True, "set-params", {"P01": 13, "P03": 4, "P04": 25000,
                              "P05": 200, "P26": 64, "P27": 609}),
        ("[CMD] ok save seq=1 err=0", True, "save", {}),
        ("[CMD] ok ref n=128 flash=0", True, "ref", {}),
        ("[CMD] params P01=13 P03=4 P04=25000 P05=200 P26=64 P27=609",
         True, "params", {"P01": 13, "P04": 25000}),
        ("[CMD] ok defaults P01=13 P03=4 P04=25000 P05=200 P26=64 P27=609",
         True, "defaults", {"P01": 13}),
        ("[CMD] err set-params P04=2500000 range=1000..2000000",
         False, "set-params", {}),
        ("[CMD] err save busy", False, "save", {}),
        ("[CMD] err ref len=64 win=128", False, "ref", {}),
        ("[CMD] err ref flat", False, "ref", {}),
        ("[CMD] err ref busy", False, "ref", {}),
        ("[CMD] err cmd=0x7F unknown", False, "cmd", {}),
    ]
    for line, ok, verb, some in good:
        ack = commands.parse_cmd_line(line)
        assert ack is not None, line
        assert ack.ok == ok and ack.verb == verb, line
        for k, v in some.items():
            assert ack.params.get(k) == v, f"{line}: {k}"
    for line in ("[CAL] defaults seq=0", "[HIT] sum_peak=1",
                 "[CMD] wharrgarbl", "", "普通文本行"):
        assert commands.parse_cmd_line(line) is None, line
    assert commands.parse_params_line("[CMD] err ref len=64 win=128") is None
    assert commands.parse_params_line("[CMD] params P04=30000") == {"P04": 30000}
    print("  ack OK（全部固件 ACK 形态 + 非 CMD 行拒绝）")

    # ---- 7. 发送路径门控（不启动线程、不建 QApplication 之外的资源） ----
    assert SourceWorker().send(b"x") is False, "基类应拒发"
    assert FileReplayWorker(Path(tempfile.gettempdir()) / "nope.bin").send(b"x") is False
    sw = SerialWorker("COM_DOES_NOT_EXIST", 921600)
    assert sw.send(b"x") is False, "未打开端口的 SerialWorker 应拒发"
    print("  gate OK（回放/未连接路径拒发）")

    print("commands selftest OK：参数表 / 帧构造 / 载荷布局 / 范围 / 求和窗 / ACK / 门控")


if __name__ == "__main__":
    main()
