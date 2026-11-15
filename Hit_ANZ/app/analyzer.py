# -*- coding: utf-8 -*-
"""分析层：击打特征 + EMA 传递矩阵落点定位（零 Qt 依赖，可 CLI 单测）。

一、击打特征（每帧）
  峰值 peak    = 固件算好的各通道窗内 max|hp|（帧头 peak_ch，经 chan_map 重映射）
  冲量 impulse = Σ_t |hp| · Δt，单位「无量纲 counts·s」，Δt = 1/3906.25 s
  均方根 rms   = sqrt(mean(hp²))
  通道前端增益差异由 EMA 签名吸收，特征本身不再做增益归一。

二、EMA 落点（实验模态方法）
  把「冲击点 → 四路支撑力」当作 LTI 系统：标定时在板上网格逐点用冲击锤敲击，
  每击取 rfft 后逐频点做跨通道单位范数归一化（消去敲击力度差异、保留相位），
  同点多次敲击复平均得传递签名 h_k(ω)；实测时同样处理得 r(ω)，把网格签名
  复数双线性插值到细网格，残差场 e(x,y) = Σ_ω (1 − |h^H r|²) 的 argmin 即落点。
  力历史 ŝ(ω) = h_p^H·y(ω)（y = 未归一化实测谱）→ irfft 得相对力历史，
  其冲量/RMS 与已定的「无量纲」口径一致。

  数据限制（如实文档化）：固件每击一帧 n=128 @ 3906.25 Hz（32.8 ms），
  Δf ≈ 30.5 Hz、Nyquist ≈ 1953 Hz——高于 ~2 kHz 的板模态不可观测，定位主要
  依赖低频空间衰减与低阶模态；力历史是**相对量**（counts 量纲），不是牛顿。
  所有窗共享固件触发对齐（触发样本固定在索引 n−1−P22），通道间/帧间相位一致，
  这是传递签名可比的前提；标定与实测必须用同一 P21/P22。

三、越界安全网
  细网格覆盖整块板，结果天然在板内；仍保留「夹取 + 形心→原始估计点连线与
  板边界的交点（Liang–Barsky）」作为防御性代码，满足原始需求。

约定：本模块只接收/返回 NumPy 数组与普通数据类，不碰任何 Qt 对象。
"""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

FS_HZ = 3906.25                   # 固件采样率（8 MHz CLKIN / 2048）
DT = 1.0 / FS_HZ                  # 256 µs
MESH_STEP_MM = 2.0                # 细网格目标步长
MAX_MESH = 200                    # 单边网格点数上限（大板自动放大步长）
CALIB_VERSION = 1
SENSOR_NAMES = ("a", "b", "c", "d")
FEATURES = ("impulse", "peak", "rms")
_EPS = 1e-12


# =====================================================================
# 特征
# =====================================================================
@dataclass
class Features:
    """一帧的四传感器特征，数组按 SENSOR_NAMES 顺序 (a, b, c, d)。"""

    valid: bool = False
    note: str = ""
    peak: np.ndarray = field(default_factory=lambda: np.zeros(4))
    impulse: np.ndarray = field(default_factory=lambda: np.zeros(4))
    rms: np.ndarray = field(default_factory=lambda: np.zeros(4))

    @property
    def total_peak(self) -> float:
        return float(np.sum(self.peak))

    @property
    def total_impulse(self) -> float:
        return float(np.sum(self.impulse))

    @property
    def total_rms(self) -> float:
        return float(np.sum(self.rms))


def extract_features(frame, chan_map: dict) -> Features:
    """按 chan_map（ch1..ch4 → a/b/c/d）把通道量重排到传感器顺序。"""
    f = Features()
    wave = np.asarray(frame.wave, dtype=np.float64)
    if wave.ndim != 2 or wave.shape[1] != 4:
        f.note = "波形形状不是 (n, 4)"
        return f
    if not isinstance(chan_map, dict):
        f.note = "通道映射无效：ch1..ch4 必须是 a/b/c/d 的排列"
        return f
    mapped = [chan_map.get(ch) for ch in ("1", "2", "3", "4")]
    if sorted(map(str, mapped)) != list(SENSOR_NAMES):
        f.note = "通道映射无效：ch1..ch4 必须是 a/b/c/d 的排列"
        return f
    col = {sensor: int(ch) - 1 for ch, sensor in chan_map.items()
           if ch in ("1", "2", "3", "4")}
    peak_raw = np.asarray(getattr(frame, "peak_ch", (0, 0, 0, 0)), dtype=np.float64)
    if peak_raw.size != 4:
        peak_raw = np.max(np.abs(wave), axis=0)
    for idx, name in enumerate(SENSOR_NAMES):
        ch = col[name]
        seg = wave[:, ch]
        f.peak[idx] = peak_raw[ch]
        f.impulse[idx] = float(np.sum(np.abs(seg)) * DT)
        f.rms[idx] = float(np.sqrt(np.mean(seg * seg)))
    f.valid = True
    return f


def weighted_centroid(features: Features, sensors: dict, which: str = "impulse"):
    """加权质心（对照用）：权重 = 所选特征，位置 = 传感器坐标。"""
    if features is None or not features.valid:
        return None
    key = which if which in FEATURES else "impulse"
    weights = np.abs(np.asarray(getattr(features, key), dtype=np.float64))
    total = float(np.sum(weights))
    if total <= _EPS:
        return None
    pts = np.array([sensors.get(name, (0.0, 0.0)) for name in SENSOR_NAMES],
                   dtype=np.float64)
    x = float(np.sum(weights * pts[:, 0]) / total)
    y = float(np.sum(weights * pts[:, 1]) / total)
    return x, y


# =====================================================================
# 传递签名 / 标定
# =====================================================================
def unit_signature(sig) -> np.ndarray:
    """逐频点跨通道单位范数归一化；零能量频点置 0。输入/输出 (4, K) 复数组。"""
    arr = np.asarray(sig, dtype=np.complex128)
    if arr.ndim != 2 or arr.shape[0] != 4:
        raise ValueError("签名形状必须是 (4, K)")
    nrm = np.sqrt(np.sum(np.abs(arr) ** 2, axis=0))
    out = np.zeros_like(arr)
    ok = nrm > _EPS
    out[:, ok] = arr[:, ok] / nrm[ok]
    return out


def tap_signature(frame) -> np.ndarray:
    """一次敲击的传递签名：rfft → 逐频点单位范数。全零波形视为无效。"""
    wave = np.asarray(frame.wave, dtype=np.float64)
    if wave.ndim != 2 or wave.shape[1] != 4:
        raise ValueError("波形形状不是 (n, 4)")
    if not np.any(np.abs(wave) > 0.0):
        raise ValueError("全零波形，无法提取传递签名")
    return unit_signature(np.fft.rfft(wave, axis=0).T)


def signature_quality(sigs) -> float:
    """一组签名的一致性：各次敲击与平均签名的平均 |⟨h̄, r_k⟩|² ∈ [0, 1]。"""
    lst = list(sigs)
    if not lst:
        return 0.0
    stack = np.stack(lst)
    mean = unit_signature(stack.sum(axis=0))
    dots = np.einsum("ck,tck->tk", np.conj(mean), stack)
    return float(np.mean(np.abs(dots) ** 2))


@dataclass
class CalibData:
    """网格化的传递签名数据集（a/b/c/d 由 xs/ys 张成，行 = y，列 = x）。"""

    xs: np.ndarray                        # (Cx,) mm
    ys: np.ndarray                        # (Cy,) mm
    sigs: np.ndarray                      # (Cy, Cx, 4, K) complex128，缺测点为 NaN
    taps: np.ndarray                      # (Cy, Cx) int，0 = 未采集
    quality: np.ndarray                   # (Cy, Cx) float，同点一致性
    fs_hz: float = FS_HZ
    n: int = 0                            # 窗长（与实测帧必须一致）
    ab_mm: float = 0.0
    ad_mm: float = 0.0
    version: int = CALIB_VERSION

    @property
    def n_valid_points(self) -> int:
        return int(np.count_nonzero(self.taps))

    @property
    def mean_quality(self) -> float:
        mask = self.taps > 0
        if not np.any(mask):
            return 0.0
        return float(np.mean(self.quality[mask]))


@dataclass
class CalibAccumulator:
    """标定模式下的逐点采集器（主线程使用，非线程安全）。"""

    xs: np.ndarray
    ys: np.ndarray
    taps_per_point: int
    fs_hz: float
    n: int
    ab_mm: float
    ad_mm: float
    _taps: dict = field(default_factory=dict)      # (i, j) -> [签名, ...]
    skipped: set = field(default_factory=set)       # 主动跳过的点

    @property
    def rows(self) -> int:
        return int(self.ys.size)

    @property
    def cols(self) -> int:
        return int(self.xs.size)

    def point_xy(self, i: int, j: int) -> tuple:
        return float(self.xs[j]), float(self.ys[i])

    def count(self, i: int, j: int) -> int:
        return len(self._taps.get((i, j), ()))

    def is_point_done(self, i: int, j: int) -> bool:
        return self.count(i, j) >= self.taps_per_point

    def is_point_skipped(self, i: int, j: int) -> bool:
        return (i, j) in self.skipped

    def is_complete(self) -> bool:
        for i in range(self.rows):
            for j in range(self.cols):
                if not self.is_point_done(i, j) and not self.is_point_skipped(i, j):
                    return False
        return True

    def next_point(self):
        """行优先的第一个「未完成且未跳过」点；全部完成返回 None。"""
        for i in range(self.rows):
            for j in range(self.cols):
                if not self.is_point_done(i, j) and not self.is_point_skipped(i, j):
                    return i, j
        return None

    def add(self, i: int, j: int, sig) -> int:
        """登记一次敲击，返回该点累计次数。"""
        if not (0 <= i < self.rows and 0 <= j < self.cols):
            raise ValueError("标定点行列越界")
        if self.is_point_done(i, j):
            raise ValueError("该点采集次数已满")
        s = unit_signature(sig)
        if s.shape[1] != self.n // 2 + 1:
            raise ValueError(f"签名频点数 {s.shape[1]} 与窗长 {self.n} 不匹配")
        self._taps.setdefault((i, j), []).append(s)
        self.skipped.discard((i, j))
        return self.count(i, j)

    def retap(self, i: int, j: int) -> None:
        """清空某点已采集的敲击（重敲）。"""
        self._taps.pop((i, j), None)
        self.skipped.discard((i, j))

    def skip(self, i: int, j: int) -> None:
        self._taps.pop((i, j), None)
        self.skipped.add((i, j))

    def point_quality(self, i: int, j: int) -> float:
        return signature_quality(self._taps.get((i, j), []))


def make_calib_accumulator(rows: int, cols: int, taps_per_point: int,
                           ab_mm: float, ad_mm: float,
                           n: int = 128, fs_hz: float = FS_HZ) -> CalibAccumulator:
    """按「整板张成」的规则网格建采集器：四角即板四角。"""
    rows = int(np.clip(int(rows), 2, 10))
    cols = int(np.clip(int(cols), 2, 10))
    taps = int(np.clip(int(taps_per_point), 1, 10))
    ab = float(ab_mm)
    ad = float(ad_mm)
    if ab <= 0.0 or ad <= 0.0:
        raise ValueError("板面尺寸必须为正")
    return CalibAccumulator(
        xs=np.linspace(0.0, ab, cols),
        ys=np.linspace(0.0, ad, rows),
        taps_per_point=taps, fs_hz=float(fs_hz), n=int(n),
        ab_mm=ab, ad_mm=ad)


def calib_add_tap(acc: CalibAccumulator, i: int, j: int, frame) -> int:
    """便捷入口：从一帧提取签名并登记到 (i, j)。"""
    return acc.add(i, j, tap_signature(frame))


def calib_finalize(acc: CalibAccumulator) -> CalibData:
    """复数平均各点敲击 → 标定数据集（未采集/跳过的点为 NaN 且 taps=0）。"""
    K = acc.n // 2 + 1
    sigs = np.full((acc.rows, acc.cols, 4, K), np.nan, dtype=np.complex128)
    taps = np.zeros((acc.rows, acc.cols), dtype=np.int32)
    quality = np.zeros((acc.rows, acc.cols), dtype=np.float64)
    for (i, j), lst in acc._taps.items():
        if not lst:
            continue
        stack = np.stack(lst)
        mean = unit_signature(stack.sum(axis=0))
        sigs[i, j] = mean
        taps[i, j] = len(lst)
        quality[i, j] = signature_quality(lst)
    return CalibData(xs=np.array(acc.xs, dtype=np.float64),
                     ys=np.array(acc.ys, dtype=np.float64),
                     sigs=sigs, taps=taps, quality=quality,
                     fs_hz=float(acc.fs_hz), n=int(acc.n),
                     ab_mm=float(acc.ab_mm), ad_mm=float(acc.ad_mm))


def save_calib(calib: CalibData, path) -> None:
    """保存为 npz（压缩）。目录不存在时自动创建。"""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(
        path,
        version=np.int32(calib.version),
        xs=np.asarray(calib.xs, dtype=np.float64),
        ys=np.asarray(calib.ys, dtype=np.float64),
        sigs=np.asarray(calib.sigs, dtype=np.complex128),
        taps=np.asarray(calib.taps, dtype=np.int32),
        quality=np.asarray(calib.quality, dtype=np.float64),
        fs_hz=np.float64(calib.fs_hz),
        n=np.int32(calib.n),
        ab_mm=np.float64(calib.ab_mm),
        ad_mm=np.float64(calib.ad_mm),
    )


def load_calib(path) -> CalibData:
    """读取并校验标定集；任何损坏都抛 ValueError（UI 层捕获后提示）。"""
    with np.load(Path(path), allow_pickle=False) as z:
        try:
            version = int(z["version"])
            xs = np.asarray(z["xs"], dtype=np.float64)
            ys = np.asarray(z["ys"], dtype=np.float64)
            sigs = np.asarray(z["sigs"], dtype=np.complex128)
            taps = np.asarray(z["taps"], dtype=np.int32)
            quality = np.asarray(z["quality"], dtype=np.float64)
            fs_hz = float(z["fs_hz"])
            n = int(z["n"])
            ab_mm = float(z["ab_mm"])
            ad_mm = float(z["ad_mm"])
        except KeyError as exc:
            raise ValueError(f"缺少字段 {exc}") from exc
    if version != CALIB_VERSION:
        raise ValueError(f"版本 {version} 不受支持（当前 {CALIB_VERSION}）")
    if xs.ndim != 1 or ys.ndim != 1 or xs.size < 2 or ys.size < 2:
        raise ValueError("网格坐标无效")
    if sigs.shape != (ys.size, xs.size, 4, n // 2 + 1) or n <= 0:
        raise ValueError(f"签名尺寸 {sigs.shape} 与网格/窗长不一致")
    if taps.shape != (ys.size, xs.size) or quality.shape != (ys.size, xs.size):
        raise ValueError("taps/quality 尺寸与网格不一致")
    return CalibData(xs=xs, ys=ys, sigs=sigs, taps=taps, quality=quality,
                     fs_hz=fs_hz, n=n, ab_mm=ab_mm, ad_mm=ad_mm,
                     version=version)


# =====================================================================
# 落点反演
# =====================================================================
@dataclass
class LocParams:
    chan_map: dict = field(default_factory=lambda: {"1": "a", "2": "b",
                                                    "3": "c", "4": "d"})
    sensors: dict = field(default_factory=lambda: {
        "a": (0.0, 0.0), "b": (300.0, 0.0),
        "c": (0.0, 300.0), "d": (300.0, 300.0)})
    ab_mm: float = 300.0
    ad_mm: float = 300.0
    feature: str = "impulse"
    f_min_hz: float = 60.0
    f_max_hz: float = 1500.0
    fs_hz: float = FS_HZ


@dataclass
class LocResult:
    """一次落点反演的全部结果；success=False 时 UI 只显示 note。"""

    success: bool = False
    note: str = ""
    x: float = 0.0                    # 最终落点（板内，越界时 = 边界交点）
    y: float = 0.0
    x_raw: float = 0.0                # 反演原始落点
    y_raw: float = 0.0
    out_of_bounds: bool = False
    boundary: tuple | None = None     # 越界时的边界交点
    resid_min: float = 0.0
    resid_map: np.ndarray | None = None
    extent: tuple = (0.0, 0.0, 0.0, 0.0)   # x0, x1, y0, y1（热图坐标）
    n_band: int = 0                   # 频带内频点数
    centroid: tuple | None = None     # 加权质心（对照）
    s_hat: np.ndarray | None = None   # 相对力历史 (n,)
    force_impulse: float = 0.0        # Σ|ŝ|·Δt，counts·s
    force_rms: float = 0.0
    features: Features = field(default_factory=Features)

    @property
    def confidence(self) -> float:
        """带内平均匹配度 1 − e_min/K' ∈ [0, 1]（标定/几何一致时才有意义）。"""
        if not self.success or self.n_band <= 0:
            return 0.0
        return float(max(0.0, 1.0 - self.resid_min / self.n_band))


def params_from_settings(settings) -> LocParams:
    return LocParams(
        chan_map=dict(settings.chan_map),
        sensors={name: settings.sensor(name) for name in SENSOR_NAMES},
        ab_mm=float(settings.ab_mm), ad_mm=float(settings.ad_mm),
        feature=getattr(settings, "loc_feature", "impulse"),
        f_min_hz=float(getattr(settings, "f_min_hz", 60.0)),
        f_max_hz=float(getattr(settings, "f_max_hz", 1500.0)),
    )


def clip_segment_to_rect(p0, p1, x_min: float, y_min: float,
                         x_max: float, y_max: float):
    """Liang–Barsky：返回线段 p0→p1 落在矩形内的出口点；全在外返回 None。"""
    x0, y0 = float(p0[0]), float(p0[1])
    dx = float(p1[0]) - x0
    dy = float(p1[1]) - y0
    t0, t1 = 0.0, 1.0
    for p, q in ((-dx, x0 - x_min), (dx, x_max - x0),
                 (-dy, y0 - y_min), (dy, y_max - y0)):
        if abs(p) < 1e-12:
            if q < 0.0:
                return None
            continue
        r = q / p
        if p < 0.0:
            t0 = max(t0, r)
        else:
            t1 = min(t1, r)
        if t0 > t1:
            return None
    if t1 < 0.0:
        return None
    return x0 + t1 * dx, y0 + t1 * dy


def _frac_index(axis_vals, x):
    """把坐标 x 映射到 axis_vals 的「左邻索引 + 小数比例」，两端夹取不外推。"""
    a = np.asarray(axis_vals, dtype=np.float64)
    scalar = np.ndim(x) == 0
    xa = np.asarray(x, dtype=np.float64)
    if a.size < 2:
        i = np.zeros(xa.shape, dtype=int)
        t = np.zeros(xa.shape, dtype=np.float64)
    else:
        i = np.clip(np.searchsorted(a, xa, side="right") - 1, 0, a.size - 2)
        span = a[i + 1] - a[i]
        good = np.abs(span) > _EPS
        t = np.where(good, (xa - a[i]) / np.where(good, span, 1.0), 0.0)
        t = np.clip(t, 0.0, 1.0)
    if scalar:
        return int(i), float(t)
    return i, t


def localize(frame, calib: CalibData | None, params: LocParams) -> LocResult:
    """单帧落点反演（特征 + 质心 + EMA 落点 + 相对力历史）。"""
    res = LocResult()
    res.features = extract_features(frame, params.chan_map)
    res.centroid = weighted_centroid(res.features, params.sensors, params.feature)
    if not res.features.valid:
        res.note = res.features.note
        return res
    if calib is None or calib.n_valid_points == 0:
        res.note = "未标定：先在「落点分析」页签完成标定（或载入标定集）"
        return res
    if (abs(calib.ab_mm - params.ab_mm) > 1.0
            or abs(calib.ad_mm - params.ad_mm) > 1.0):
        res.note = (f"标定集几何 {calib.ab_mm:.0f}×{calib.ad_mm:.0f} mm "
                    f"与当前几何不一致，请重新标定")
        return res

    wave = np.asarray(frame.wave, dtype=np.float64)
    n = int(wave.shape[0]) if wave.ndim == 2 else 0
    if n < 8:
        res.note = "波形窗长无效"
        return res
    if calib.n and calib.n != n:
        res.note = f"标定窗长 {calib.n} ≠ 本帧 {n}，请重新标定"
        return res
    if not np.any(np.abs(wave) > 0.0):
        res.note = "全零波形"
        return res
    if not (0.0 < params.f_min_hz < params.f_max_hz):
        res.note = "频带设置无效（需 0 < f_min < f_max）"
        return res

    # ---- 实测签名（与标定同一处理） ----
    F = np.fft.rfft(wave, axis=0).T                    # (4, K)
    freqs = np.fft.rfftfreq(n, d=1.0 / params.fs_hz)
    K = min(F.shape[1], calib.sigs.shape[-1])
    band = (freqs[:K] >= params.f_min_hz) & (freqs[:K] <= params.f_max_hz)
    band[0] = False                                    # 直流（含 EMA 残余）不用
    idx = np.flatnonzero(band)
    if idx.size == 0:
        res.note = "频带内无有效频点（检查频带设置与窗长）"
        return res
    y_band = F[:, idx]                                 # (4, K')
    r = unit_signature(y_band)
    if not np.any(np.abs(r) > 0.0):
        res.note = "波形谱能量为零"
        return res

    # ---- 复数双线性插值 + 残差场 ----
    sigs = calib.sigs[..., :K][..., idx]          # 只留频带内
    mask = calib.taps > 0
    if not np.any(mask):
        res.note = "标定集没有有效点"
        return res
    xs, ys = calib.xs, calib.ys
    nx = int(min(MAX_MESH, max(2, round(params.ab_mm / MESH_STEP_MM) + 1)))
    ny = int(min(MAX_MESH, max(2, round(params.ad_mm / MESH_STEP_MM) + 1)))
    mesh_x = np.linspace(0.0, params.ab_mm, nx)
    mesh_y = np.linspace(0.0, params.ad_mm, ny)
    Kp = int(idx.size)
    resid = np.full((ny, nx), np.inf, dtype=np.float64)
    best_e = np.inf
    best_iy = best_ix = 0
    best_h = None

    for iy, yv in enumerate(mesh_y):
        j0, ty = _frac_index(ys, yv)
        j1 = min(j0 + 1, ys.size - 1)
        ix0, tx = _frac_index(xs, mesh_x)
        ix1 = np.minimum(ix0 + 1, xs.size - 1)
        acc_h = np.zeros((nx, 4, Kp), dtype=np.complex128)
        acc_w = np.zeros(nx, dtype=np.float64)
        for wy, jj in ((1.0 - ty, j0), (ty, j1)):
            if wy == 0.0:
                continue
            for wx, ii in ((1.0 - tx, ix0), (tx, ix1)):
                w = wy * wx                    # (nx,) 或标量
                m = mask[jj, ii]               # (nx,) bool
                wm = w * m
                if not np.any(wm > 0.0):
                    continue
                acc_w += wm
                node = np.where(m[:, None, None], sigs[jj, ii], 0.0)
                acc_h += wm[:, None, None] * node
        ok = acc_w > 1e-9
        H = np.zeros_like(acc_h)
        H[ok] = acc_h[ok] / acc_w[ok][:, None, None]
        # 插值会削弱复矢量幅度（网格节点之间相位不一致），必须逐频点重新单位化，
        # 否则残差场会系统性地偏向标定网格节点，造成落点被吸到网格线上。
        nrm = np.sqrt(np.sum(np.abs(H) ** 2, axis=1, keepdims=True))  # (nx,1,K')
        good = nrm > 1e-9
        H = np.where(good, H / np.where(good, nrm, 1.0), 0.0)
        proj = np.einsum("nck,ck->nk", np.conj(H), r)
        e = Kp - np.sum(np.abs(proj) ** 2, axis=1)
        e[~ok] = np.inf
        resid[iy] = e
        jx = int(np.argmin(e))
        if e[jx] < best_e:
            best_e = float(e[jx])
            best_iy, best_ix = iy, jx
            best_h = H[jx].copy()

    if best_h is None:
        res.note = "标定网格没有可用的插值节点"
        return res

    res.x_raw = float(mesh_x[best_ix])
    res.y_raw = float(mesh_y[best_iy])
    res.resid_min = float(best_e)
    res.resid_map = resid
    res.extent = (0.0, float(params.ab_mm), 0.0, float(params.ad_mm))
    res.n_band = Kp

    # ---- 越界安全网 ----
    in_bounds = (0.0 <= res.x_raw <= params.ab_mm
                 and 0.0 <= res.y_raw <= params.ad_mm)
    if in_bounds:
        res.x, res.y = res.x_raw, res.y_raw
    else:
        res.out_of_bounds = True
        center = (params.ab_mm / 2.0, params.ad_mm / 2.0)
        pt = clip_segment_to_rect(center, (res.x_raw, res.y_raw),
                                  0.0, 0.0, params.ab_mm, params.ad_mm)
        if pt is None:
            pt = (min(max(res.x_raw, 0.0), params.ab_mm),
                  min(max(res.y_raw, 0.0), params.ad_mm))
        res.boundary = pt
        res.x, res.y = pt

    # ---- 相对力历史：ŝ(ω) = h_p^H · y(ω) ----
    s_band = np.einsum("ck,ck->k", np.conj(best_h), y_band)
    full = np.zeros(F.shape[1], dtype=np.complex128)
    full[idx] = s_band
    s_time = np.fft.irfft(full, n=n)
    res.s_hat = s_time
    res.force_impulse = float(np.sum(np.abs(s_time)) * DT)
    res.force_rms = float(np.sqrt(np.mean(s_time * s_time)))

    res.success = True
    if res.out_of_bounds:
        res.note = "估计点越界：已显示形心→估计点连线与边界的交点"
    return res
