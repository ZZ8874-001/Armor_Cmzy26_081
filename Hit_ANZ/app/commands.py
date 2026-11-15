# -*- coding: utf-8 -*-
"""数据层：上位机 → 板子 HCMD 命令帧构造 + 板子 ACK 文本解析（零 Qt，可 CLI 单测）。

帧格式权威：Docs/USART1波形数据帧格式.md v1.1 HCMD 节。
CRC 与 WAV1 一致（zlib.crc32 反射 0xEDB88320）。

注意：PARAM_SPECS 的范围表与固件 host_cmd.c 的 HC_*_MIN/MAX 双端同步——
改任意一侧必须同时改另一侧，固件会以 `[CMD] err … range=…` 兜底拒绝。
"""
from __future__ import annotations

import re
import struct
import zlib
from dataclasses import dataclass
from typing import Mapping

import numpy as np

FRAME_MAGIC = b"HCMD"
FRAME_VERSION = 1
HDR_LEN = 8
MAX_PAYLOAD = 514

CMD_SET_PARAMS = 0x01
CMD_SAVE_PARAMS = 0x02
CMD_SET_REF = 0x03
CMD_GET_PARAMS = 0x04
CMD_SET_DEFAULTS = 0x05


@dataclass(frozen=True)
class ParamSpec:
    key: str          # "P01".."P27"（载荷顺序 = PARAM_SPECS 顺序）
    field: str        # hit_param_t 字段名（文档用）
    label: str        # 界面显示名
    unit: str
    lo: int
    hi: int
    step: int
    default: int


PARAM_SPECS: tuple[ParamSpec, ...] = (
    ParamSpec("P01", "s_bl", "基线 EMA 移位", "bit", 8, 16, 1, 13),
    ParamSpec("P03", "min_dur", "触发最小持续", "样本", 1, 64, 1, 4),
    ParamSpec("P04", "thr_hit", "触发阈值", "counts", 1000, 2_000_000, 500, 25000),
    ParamSpec("P05", "refractory_ms", "不应期", "ms", 50, 1000, 10, 200),
    ParamSpec("P26", "settle_depth", "沉降门控", "样本", 1, 512, 1, 64),
    ParamSpec("P27", "corr_min_q10", "相关度阈值", "q10", 0, 1024, 1, 609),
)
PARAM_SPECS_BY_KEY: dict[str, ParamSpec] = {s.key: s for s in PARAM_SPECS}


@dataclass
class CmdAck:
    """一条 `[CMD] …` ACK 文本行的解析结果。"""
    ok: bool
    verb: str                   # "set-params"/"save"/"ref"/"params"/"defaults"/"cmd"
    params: dict                # { "P01": 13, ... }
    text: str                   # 原始行


def crc32(data: bytes) -> int:
    """与固件 Wave/Ref/Cal/Hc CRC 逐字节一致（反射 0xEDB88320）。"""
    return zlib.crc32(data) & 0xFFFFFFFF


def build_frame(cmd: int, payload: bytes = b"") -> bytes:
    if not 0 <= cmd <= 0xFF:
        raise ValueError(f"cmd 越界：{cmd}")
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload 超长：{len(payload)} > {MAX_PAYLOAD}")
    head = struct.pack("<4sBBH", FRAME_MAGIC, FRAME_VERSION, cmd, len(payload))
    return head + payload + struct.pack("<I", crc32(head + payload))


def _pack_params(values: Mapping[str, int]) -> bytes:
    """按 PARAM_SPECS 顺序打包 <BBiHHH（12B）；缺键抛错，越界抛 ValueError。"""
    if len(values) != len(PARAM_SPECS) or any(s.key not in values
                                               for s in PARAM_SPECS):
        raise ValueError("参数集合必须恰好包含全部精选键 "
                         + " ".join(s.key for s in PARAM_SPECS))
    vals = []
    for spec in PARAM_SPECS:
        v = int(values[spec.key])
        if not spec.lo <= v <= spec.hi:
            raise ValueError(f"{spec.key}={v} 超出范围 {spec.lo}..{spec.hi}")
        vals.append(v)
    return struct.pack("<BBiHHH", *vals)


def build_set_params(values: Mapping[str, int]) -> bytes:
    return build_frame(CMD_SET_PARAMS, _pack_params(values))


def build_get_params() -> bytes:
    return build_frame(CMD_GET_PARAMS)


def build_save_params() -> bytes:
    return build_frame(CMD_SAVE_PARAMS)


def build_set_defaults() -> bytes:
    return build_frame(CMD_SET_DEFAULTS)


def build_set_ref(frame) -> bytes:
    """把一帧记录的波形打包为 SET_REFERENCE 载荷：u16 n + n×i32 合力窗。

    合力窗 = 四通道 hp 逐样本求和（wave 列序 ch1..ch4，不做 chan_map 重排）。
    int64 求和后 astype('<i4') 回绕，与固件 int32 累加语义一致（4×24bit 不会真溢出）。
    """
    n = int(getattr(frame, "n", 0))
    wave = getattr(frame, "wave", None)
    if not 1 <= n <= 128 or wave is None or wave.shape != (n, 4):
        raise ValueError(f"帧窗长无效：n={n}")
    sums = wave.astype(np.int64).sum(axis=1).astype("<i4")
    payload = struct.pack("<H", n) + sums.tobytes()
    return build_frame(CMD_SET_REF, payload)


_CMD_RE = re.compile(r"^\[CMD\]\s+(ok|err)\s+([a-z0-9\-]+)(.*)$")
_CMD_PARAMS_RE = re.compile(r"^\[CMD\]\s+params\s+(.*)$")   # GET_PARAMS 回执无 ok 前缀
_PARAM_TOKEN_RE = re.compile(r"P(\d{2})=(-?\d+)")


def parse_cmd_line(line: str) -> CmdAck | None:
    """解析板子 ACK 行；非 `[CMD]` 前缀或格式不符返回 None。"""
    text = line.strip()
    m = _CMD_RE.match(text)
    if m is None:
        m2 = _CMD_PARAMS_RE.match(text)
        if m2 is None:
            return None
        ok, verb = True, "params"
    else:
        ok = m.group(1) == "ok"
        verb = m.group(2)
    params = {}
    for pk, pv in _PARAM_TOKEN_RE.findall(text):
        params[f"P{pk}"] = int(pv)
    return CmdAck(ok=ok, verb=verb, params=params, text=text)


def parse_params_line(line: str) -> dict | None:
    """从任意含 Pxx=值 令牌的行提取参数；无令牌返回 None。"""
    ack = parse_cmd_line(line)
    if ack is not None and ack.params:
        return ack.params
    found = {f"P{pk}": int(pv) for pk, pv in _PARAM_TOKEN_RE.findall(line)}
    return found or None
