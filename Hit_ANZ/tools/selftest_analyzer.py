# -*- coding: utf-8 -*-
"""分析层 CLI 自测（零 Qt，纯 numpy）：py -3 tools/selftest_analyzer.py

覆盖：特征提取（含 chan_map 置换）、加权质心、传递签名归一化/幅度不变性、
同点一致性、npz 存取、玩具模型反演精度、越界裁剪（Liang–Barsky）、各种守卫。
"""
from __future__ import annotations

import sys
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

from app import analyzer  # noqa: E402
from app.protocol import Frame  # noqa: E402

N = 128
AB = AD = 300.0
SENSORS = {"a": (0.0, 0.0), "b": (AB, 0.0),
           "c": (0.0, AD), "d": (AB, AD)}
IDENTITY = {"1": "a", "2": "b", "3": "c", "4": "d"}


def make_frame(wave, peak_ch=None) -> Frame:
    wave = np.asarray(wave, dtype=np.int32)
    if peak_ch is None:
        peak_ch = tuple(int(v) for v in np.max(np.abs(wave), axis=0))
    return Frame(type=1, flags=0, n=wave.shape[0], ts_ms=0, trig_id=1,
                 corr_q10=1024, pk_idx=63, peak_dif=0, peak_ch=peak_ch,
                 sample_hz=3906, wave=wave)


# ---------------------------------------------------------------------
# 特征
# ---------------------------------------------------------------------
def test_features() -> None:
    A = 1000.0
    frame = make_frame(np.full((N, 4), A))
    f = analyzer.extract_features(frame, IDENTITY)
    assert f.valid, f.note
    assert np.allclose(f.impulse, A * N * analyzer.DT)
    assert np.allclose(f.rms, A)
    assert np.allclose(f.peak, A)
    assert np.isclose(f.total_impulse, 4 * A * N * analyzer.DT)

    # 通道置换：ch1→b, ch2→a, ch3→d, ch4→c
    amps = (100.0, 200.0, 300.0, 400.0)
    wave = np.column_stack([np.full(N, a) for a in amps])
    f = analyzer.extract_features(make_frame(wave),
                                  {"1": "b", "2": "a", "3": "d", "4": "c"})
    assert f.valid
    assert np.isclose(f.impulse[0], 200.0 * N * analyzer.DT)   # a ← ch2
    assert np.isclose(f.impulse[1], 100.0 * N * analyzer.DT)   # b ← ch1
    assert np.isclose(f.impulse[2], 400.0 * N * analyzer.DT)   # c ← ch4
    assert np.isclose(f.impulse[3], 300.0 * N * analyzer.DT)   # d ← ch3
    assert np.isclose(f.peak[0], 200.0)

    bad = analyzer.extract_features(make_frame(wave),
                                    {"1": "a", "2": "a", "3": "c", "4": "d"})
    assert not bad.valid and "映射" in bad.note
    print("  features OK（冲量 = Σ|hp|·Δt、chan_map 置换、坏映射拒绝）")


def test_centroid() -> None:
    feat = analyzer.Features(
        valid=True,
        peak=np.array([1.0, 0.0, 0.0, 3.0]),
        impulse=np.array([1.0, 0.0, 0.0, 3.0]),
        rms=np.array([1.0, 0.0, 0.0, 3.0]))
    for which in analyzer.FEATURES:
        c = analyzer.weighted_centroid(feat, SENSORS, which)
        assert np.allclose(c, (225.0, 225.0)), (which, c)
    zero = analyzer.Features(valid=True)
    assert analyzer.weighted_centroid(zero, SENSORS, "impulse") is None
    print("  centroid OK（三种权重、全零权重返回 None）")


# ---------------------------------------------------------------------
# 签名
# ---------------------------------------------------------------------
def toy_signature(x: float, y: float, n: int = N,
                  c_mm_per_s: float = 1.5e6,
                  decay_mm: float = 120.0) -> np.ndarray:
    """玩具 LTI 模型：h_i(ω) = e^{-d_i/120}·e^{-jω d_i / 1.5e6}，d_i 单位 mm。"""
    freqs = np.fft.rfftfreq(n, d=analyzer.DT)
    sig = np.zeros((4, freqs.size), dtype=np.complex128)
    for ci, name in enumerate(analyzer.SENSOR_NAMES):
        sx, sy = SENSORS[name]
        d = max(20.0, float(np.hypot(x - sx, y - sy)))
        sig[ci] = np.exp(-d / decay_mm) * np.exp(
            -2j * np.pi * freqs * d / c_mm_per_s)
    return analyzer.unit_signature(sig)


def toy_frame(x: float, y: float, amp: float = 5e4, n: int = N) -> Frame:
    sig = toy_signature(x, y, n)
    freqs = np.fft.rfftfreq(n, d=analyzer.DT)
    weight = amp / (1.0 + freqs / 400.0)
    wave = np.fft.irfft(sig.T * weight[:, None], n=n, axis=0)
    return make_frame(np.rint(wave).astype(np.int32))


def test_signature() -> None:
    f1 = toy_frame(75.0, 120.0)
    s1 = analyzer.tap_signature(f1)
    norm = np.sum(np.abs(s1) ** 2, axis=0)
    nz = norm > 1e-9
    assert np.allclose(norm[nz], 1.0, atol=1e-9), "签名未单位化"

    # 幅度 ×3：归一化签名不变（力度不影响落点）
    s2 = analyzer.tap_signature(make_frame(f1.wave * 3))
    assert np.allclose(s1, s2, atol=1e-9)

    # 同点一致性
    assert analyzer.signature_quality([s1, s2]) > 0.9999
    rng = np.random.default_rng(0)
    noisy = analyzer.unit_signature(
        s1 + 0.05 * (rng.standard_normal(s1.shape)
                     + 1j * rng.standard_normal(s1.shape)))
    q = analyzer.signature_quality([s1, noisy])
    assert 0.8 < q < 1.0, q
    assert analyzer.signature_quality([s1, -s1]) < 1e-9    # 反相 → 不一致
    assert analyzer.signature_quality([]) == 0.0

    # 全零波形 → 无效
    try:
        analyzer.tap_signature(make_frame(np.zeros((N, 4))))
    except ValueError:
        pass
    else:
        raise AssertionError("全零波形应抛 ValueError")
    print("  signature OK（单位范数、幅度不变、一致性指标、全零拒绝）")


# ---------------------------------------------------------------------
# 标定集 / 反演
# ---------------------------------------------------------------------
def build_calib(rows: int = 5, cols: int = 5, taps: int = 2):
    acc = analyzer.make_calib_accumulator(rows, cols, taps, AB, AD, n=N)
    for i in range(rows):
        for j in range(cols):
            x, y = acc.point_xy(i, j)
            sig = toy_signature(x, y)
            for _ in range(taps):
                acc.add(i, j, sig)
    return acc, analyzer.calib_finalize(acc)


def test_calib_io() -> None:
    acc, calib = build_calib(rows=2, cols=3, taps=2)
    assert calib.sigs.shape == (2, 3, 4, N // 2 + 1)
    assert calib.n_valid_points == 6
    assert abs(calib.mean_quality - 1.0) < 1e-9
    assert acc.point_quality(0, 0) > 0.9999

    path = Path(tempfile.gettempdir()) / "hit_anz_selftest_calib.npz"
    analyzer.save_calib(calib, path)
    loaded = analyzer.load_calib(path)
    assert loaded.sigs.shape == calib.sigs.shape
    assert np.allclose(loaded.xs, calib.xs)
    assert np.allclose(loaded.ys, calib.ys)
    assert loaded.n == calib.n and loaded.n_valid_points == 6
    assert np.allclose(np.nan_to_num(loaded.sigs), np.nan_to_num(calib.sigs))

    try:
        analyzer.load_calib(path.with_name("hit_anz_no_such.npz"))
    except (OSError, ValueError):
        pass
    else:
        raise AssertionError("缺失标定集应报错")
    path.unlink(missing_ok=True)
    print("  calib_io OK（形状/点数/质量和、npz 往返、缺失文件）")


def test_localize() -> None:
    _, calib = build_calib(rows=5, cols=5, taps=2)
    params = analyzer.LocParams(chan_map=IDENTITY, sensors=SENSORS,
                                ab_mm=AB, ad_mm=AD)
    truth = (75.0, 120.0)
    frame = toy_frame(*truth)
    loc = analyzer.localize(frame, calib, params)
    assert loc.success, loc.note
    err = float(np.hypot(loc.x - truth[0], loc.y - truth[1]))
    assert err < 5.0, f"落点误差 {err:.1f} mm"
    assert loc.n_band > 20, loc.n_band
    assert loc.resid_min < 0.05 * loc.n_band, loc.resid_min
    assert 0.9 < loc.confidence <= 1.0, loc.confidence

    # 热图：真实点邻域明显优于远端
    x0, x1, y0, y1 = loc.extent
    ny, nx = loc.resid_map.shape
    ix = int(round((truth[0] - x0) / (x1 - x0) * (nx - 1)))
    iy = int(round((truth[1] - y0) / (y1 - y0) * (ny - 1)))
    near = float(loc.resid_map[iy, ix])
    far = float(loc.resid_map[-1, -1])
    assert near <= loc.resid_min + 0.05 * loc.n_band, (near, loc.resid_min)
    assert far > near + 0.1 * loc.n_band, (far, near)

    # 质心（三种特征）都可用
    for which in analyzer.FEATURES:
        assert analyzer.weighted_centroid(loc.features, SENSORS, which) is not None

    # 相对力历史
    assert loc.s_hat is not None and len(loc.s_hat) == N
    assert loc.force_impulse > 0.0 and loc.force_rms > 0.0
    assert np.all(np.isfinite(loc.s_hat))

    # 落点应在板内且与网格坐标一致
    assert 0.0 <= loc.x <= AB and 0.0 <= loc.y <= AD
    assert not loc.out_of_bounds
    print(f"  localize OK（{truth} → ({loc.x:.1f}, {loc.y:.1f})，"
          f"误差 {err:.1f} mm，置信度 {loc.confidence:.0%}）")


def test_boundary() -> None:
    pt = analyzer.clip_segment_to_rect((150.0, 150.0), (900.0, 100.0),
                                       0.0, 0.0, AB, AD)
    assert np.allclose(pt, (300.0, 140.0)), pt
    pt = analyzer.clip_segment_to_rect((150.0, 150.0), (150.0, 900.0),
                                       0.0, 0.0, AB, AD)
    assert np.allclose(pt, (150.0, 300.0)), pt
    pt = analyzer.clip_segment_to_rect((150.0, 150.0), (200.0, 200.0),
                                       0.0, 0.0, AB, AD)
    assert np.allclose(pt, (200.0, 200.0)), pt
    pt = analyzer.clip_segment_to_rect((-50.0, 150.0), (500.0, 150.0),
                                       0.0, 0.0, AB, AD)
    assert np.allclose(pt, (300.0, 150.0)), pt
    assert analyzer.clip_segment_to_rect((-50.0, -50.0), (-10.0, -10.0),
                                         0.0, 0.0, AB, AD) is None
    print("  boundary OK（斜穿出 x=AB、竖穿出 y=AD、内部不变、全在外 None）")


def test_guards() -> None:
    _, calib = build_calib(rows=3, cols=3, taps=1)
    params = analyzer.LocParams(chan_map=IDENTITY, sensors=SENSORS,
                                ab_mm=AB, ad_mm=AD)
    frame = toy_frame(150.0, 150.0)

    loc = analyzer.localize(frame, None, params)
    assert not loc.success and "标定" in loc.note

    calib.ab_mm = 200.0
    loc = analyzer.localize(frame, calib, params)
    assert not loc.success and "几何" in loc.note
    calib.ab_mm = AB

    loc = analyzer.localize(make_frame(np.zeros((N, 4))), calib, params)
    assert not loc.success and "零" in loc.note

    bad_map = analyzer.LocParams(chan_map={"1": "a", "2": "a", "3": "c",
                                           "4": "d"},
                                 sensors=SENSORS, ab_mm=AB, ad_mm=AD)
    loc = analyzer.localize(frame, calib, bad_map)
    assert not loc.success and "映射" in loc.note

    acc = analyzer.make_calib_accumulator(2, 2, 1, AB, AD, n=N)
    empty = analyzer.calib_finalize(acc)
    loc = analyzer.localize(frame, empty, params)
    assert not loc.success and "标定" in loc.note

    bad_band = analyzer.LocParams(chan_map=IDENTITY, sensors=SENSORS,
                                  ab_mm=AB, ad_mm=AD,
                                  f_min_hz=1500.0, f_max_hz=60.0)
    loc = analyzer.localize(frame, calib, bad_band)
    assert not loc.success and "频带" in loc.note

    # 跳过/重敲语义
    acc2 = analyzer.make_calib_accumulator(2, 2, 2, AB, AD, n=N)
    sig = toy_signature(150.0, 150.0)
    acc2.add(0, 0, sig)
    acc2.retap(0, 0)
    assert acc2.count(0, 0) == 0
    acc2.skip(0, 0)
    assert acc2.is_point_skipped(0, 0)
    assert acc2.next_point() == (0, 1)
    print("  guards OK（未标定/几何不符/全零/坏映射/空集/坏频带/跳过重敲）")


def main() -> None:
    test_features()
    test_centroid()
    test_signature()
    test_calib_io()
    test_localize()
    test_boundary()
    test_guards()
    print("analyzer selftest OK：特征 / 质心 / 签名 / 标定集 / 反演 / 越界 / 守卫")


if __name__ == "__main__":
    main()
