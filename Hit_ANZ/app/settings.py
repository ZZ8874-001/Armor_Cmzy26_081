# -*- coding: utf-8 -*-
"""设置层：UI 设置 + config.json 持久化（改动即保存，重启自动还原）。

设置项：
  - 串口（端口、波特率）
  - 击打面几何：AB / AD 长度（mm）
  - 传感器位置 a/b/c/d（mm）：默认跟随矩形四角，可脱钩手动摆放
  - 通道 → 传感器映射（ch1..ch4 → a/b/c/d）
  - P21/P22（触发样本标记位置，默认 64/64）
  - 落点分析：质心特征源、标定网格行/列、每点敲击次数、频带、标定集路径
  - 上次打开文件的目录
"""
from __future__ import annotations

import json
import os
from dataclasses import asdict, dataclass, field
from pathlib import Path

SENSOR_NAMES = ("a", "b", "c", "d")


def config_path() -> Path:
    """配置文件位置：默认 Hit_ANZ/config.json；测试可用 HIT_ANZ_CONFIG 覆盖。"""
    env = os.environ.get("HIT_ANZ_CONFIG")
    if env:
        return Path(env)
    return Path(__file__).resolve().parent.parent / "config.json"


@dataclass
class Settings:
    port: str = ""                  # 空 = 未选
    baudrate: int = 921600
    ab_mm: float = 300.0            # AB 边（横向）长度
    ad_mm: float = 300.0            # AD 边（纵向）长度
    follow_corners: bool = True     # 传感器位置跟随矩形四角
    # 传感器坐标（mm），原点 = a 角；x 沿 AB，y 沿 AD
    sensors: dict = field(default_factory=lambda: {
        "a": [0.0, 0.0], "b": [300.0, 0.0],
        "c": [0.0, 300.0], "d": [300.0, 300.0],
    })
    # 通道映射：键 "1".."4" → 传感器名
    chan_map: dict = field(default_factory=lambda: {
        "1": "a", "2": "b", "3": "c", "4": "d",
    })
    p21: int = 64                   # 预触发样本数（仅用于触发标记）
    p22: int = 64                   # 后触发样本数
    # ---- 落点分析 ----
    loc_feature: str = "impulse"    # 质心对照所用特征：impulse / peak / rms
    calib_path: str = ""            # 标定集 npz；空 = 默认 Hit_ANZ/calib/transfer.npz
    calib_rows: int = 5             # 标定网格行数（沿 AD）
    calib_cols: int = 5             # 标定网格列数（沿 AB）
    taps_per_point: int = 3         # 每个标定点敲击次数
    f_min_hz: float = 60.0          # EMA 反演频带低端
    f_max_hz: float = 1500.0        # EMA 反演频带高端
    last_open_dir: str = ""

    # ------------------------------------------------------------------
    def corner_positions(self) -> dict:
        ab, ad = float(self.ab_mm), float(self.ad_mm)
        return {"a": [0.0, 0.0], "b": [ab, 0.0],
                "c": [0.0, ad], "d": [ab, ad]}

    def sync_corners(self) -> None:
        """若勾选跟随四角，用 AB/AD 重算 a/b/c/d 坐标。"""
        if self.follow_corners:
            self.sensors = self.corner_positions()

    def sensor(self, name: str) -> tuple:
        xy = self.sensors.get(name, [0.0, 0.0])
        return float(xy[0]), float(xy[1])

    # ------------------------------------------------------------------
    @classmethod
    def load(cls, path: Path | None = None) -> "Settings":
        s = cls()
        try:
            data = json.loads(Path(path or config_path()).read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return s
        for key, value in data.items():
            if not hasattr(s, key):
                continue
            try:
                if key == "sensors" and isinstance(value, dict):
                    for name in SENSOR_NAMES:
                        xy = value.get(name)
                        if isinstance(xy, (list, tuple)) and len(xy) == 2:
                            s.sensors[name] = [float(xy[0]), float(xy[1])]
                elif key == "chan_map" and isinstance(value, dict):
                    for ch in ("1", "2", "3", "4"):
                        if value.get(ch) in SENSOR_NAMES:
                            s.chan_map[ch] = value[ch]
                else:
                    setattr(s, key, type(getattr(s, key))(value))
            except (TypeError, ValueError):
                continue            # 单项损坏 → 保留默认值
        if s.follow_corners:
            s.sync_corners()
        # 落点分析参数越界时回默认值（旧 config.json 缺键走默认，无需迁移）
        if s.loc_feature not in ("impulse", "peak", "rms"):
            s.loc_feature = "impulse"
        s.calib_rows = min(10, max(2, int(s.calib_rows)))
        s.calib_cols = min(10, max(2, int(s.calib_cols)))
        s.taps_per_point = min(10, max(1, int(s.taps_per_point)))
        if not (0.0 <= float(s.f_min_hz) < float(s.f_max_hz) <= 1953.0):
            s.f_min_hz, s.f_max_hz = 60.0, 1500.0
        return s

    def save(self, path: Path | None = None) -> None:
        self.sync_corners()
        try:
            Path(path or config_path()).write_text(
                json.dumps(asdict(self), ensure_ascii=False, indent=2),
                encoding="utf-8")
        except OSError:
            pass                    # 配置写失败不影响运行
