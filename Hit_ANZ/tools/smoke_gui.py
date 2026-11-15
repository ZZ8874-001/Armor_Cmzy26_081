# -*- coding: utf-8 -*-
"""GUI 冒烟测试（offscreen，无需显示器/硬件）：

    py -3 tools/smoke_gui.py            # 只跑断言
    py -3 tools/smoke_gui.py --shot test/tab2.png   # 额外把「落点分析」页截屏

启动 MainWindow → 回放 test/sample_capture.bin → 断言帧入库、曲线上屏、
CRC 计数正确；再断言双页签、未标定提示、合成标定集后定位成功与历史点。
用于改 UI/分析层后的快速回归。
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import time
from pathlib import Path
from types import SimpleNamespace

import numpy as np

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
_SMOKE_CFG = Path(tempfile.gettempdir()) / "hit_anz_smoke_config.json"
try:
    _SMOKE_CFG.unlink()
except OSError:
    pass
# 隔离真实标定集：calib_path 指向不存在的文件，避免命中 Hit_ANZ/calib/transfer.npz
_SMOKE_CFG.write_text(json.dumps({"calib_path": str(
    Path(tempfile.gettempdir()) / "hit_anz_smoke_no_calib.npz")}),
    encoding="utf-8")
os.environ["HIT_ANZ_CONFIG"] = str(_SMOKE_CFG)   # 不污染用户的 config.json

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

from PySide6.QtCore import QPointF  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402

from app import analyzer  # noqa: E402
from app.main_window import MainWindow  # noqa: E402
from app.sources import FileReplayWorker, SerialWorker  # noqa: E402


def pump(app: QApplication, seconds: float) -> None:
    deadline = time.time() + seconds
    while time.time() < deadline:
        app.processEvents()
        time.sleep(0.005)


def replay(window: MainWindow, app: QApplication, capture: Path) -> None:
    """回放 capture 并等回放线程结束（回放块 1024 B < 单帧 2100 B）。"""
    window._start_worker(FileReplayWorker(capture))
    deadline = time.time() + 15
    while window.worker is not None and time.time() < deadline:
        app.processEvents()
        time.sleep(0.005)
    pump(app, 0.1)


def main() -> None:
    capture = ROOT / "test" / "sample_capture.bin"
    if not capture.exists():
        sys.exit(f"缺少 {capture}，先运行 py -3 tools/gen_sample_capture.py")

    shot = None
    if "--shot" in sys.argv:
        idx = sys.argv.index("--shot")
        shot = Path(sys.argv[idx + 1]) if idx + 1 < len(sys.argv) \
            else ROOT / "test" / "tab2.png"
        if not shot.is_absolute():
            shot = ROOT / shot

    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()

    replay(window, app, capture)

    stats = window.parser.stats
    assert len(window.store.frames) == 3, f"帧数 {len(window.store.frames)}"
    assert stats.crc_errors == 2, f"CRC 错 {stats.crc_errors}"
    assert window.store.frames[-1].type == 2, "末帧应为相关度拒绝"

    frame = window.store.frames[-1]
    x, y = window.plot.curves[0].getData()
    assert y is not None and len(y) == frame.n, "曲线未上屏"
    assert window.frame_combo.count() == 3, f"帧历史 {window.frame_combo.count()}"

    # ---- 双页签 + 未标定路径 ----
    assert window.tabs.count() == 2, f"页签数 {window.tabs.count()}"
    window.tabs.setCurrentIndex(1)
    pump(app, 0.05)
    assert len(window.top_view.history) == 0, "未标定时不应有历史点"
    loc = window._last_loc
    assert loc is not None and not loc.success, "未标定时应定位失败"
    assert "标定" in loc.note, loc.note
    assert loc.centroid is not None, "质心对照应始终可用"
    assert window.feature_table.item(0, 2).text() != "—", "特征表未填充"
    assert "未标定" in window.calib_status.text(), window.calib_status.text()

    # ---- 合成 2×2 标定集 → 定位成功 + 历史点 ----
    sig = analyzer.tap_signature(window.store.frames[0])
    acc = analyzer.make_calib_accumulator(
        2, 2, 1, window.settings.ab_mm, window.settings.ad_mm, n=128)
    for i in range(2):
        for j in range(2):
            acc.add(i, j, sig)
    calib_file = Path(tempfile.gettempdir()) / "hit_anz_smoke_calib.npz"
    analyzer.save_calib(analyzer.calib_finalize(acc), calib_file)
    window.settings.calib_path = str(calib_file)
    window._reload_calib(quiet=True)
    assert window._calib_data is not None
    assert window._calib_data.n_valid_points == 4
    assert window._last_loc is not None and window._last_loc.success, \
        window._last_loc.note
    assert window._last_loc.n_band > 0

    replay(window, app, capture)
    assert len(window.store.frames) == 3
    assert window._last_loc is not None and window._last_loc.success
    assert len(window.top_view.history) == 3, \
        f"历史点 {len(window.top_view.history)}"

    if shot is not None:
        window.tabs.setCurrentIndex(1)
        pump(app, 0.2)
        shot.parent.mkdir(parents=True, exist_ok=True)
        window.grab().save(str(shot))
        print(f"截图已保存：{shot}")

    # ---- 标定工作流（按钮驱动：开始 → 跳过/重敲/取消 → 采集 → 完成保存） ----
    window._rows_spin.setValue(2)
    window._cols_spin.setValue(2)
    window._taps_spin.setValue(1)
    pump(app, 0.05)

    window._on_calib_start()
    assert window._calib_acc is not None
    assert window.top_view.tap_mark.isVisible(), "未高亮待敲点"
    window._on_calib_skip()
    acc = window._calib_acc
    assert acc.is_point_skipped(0, 0), "跳过未生效"
    assert acc.next_point() == (0, 1)
    window._on_calib_retap()
    assert acc.count(0, 1) == 0
    window._on_calib_cancel()
    assert window._calib_acc is None
    assert not window.top_view.tap_mark.isVisible()

    calib_file2 = Path(tempfile.gettempdir()) / "hit_anz_smoke_calib2.npz"
    calib_file2.unlink(missing_ok=True)
    window.settings.calib_path = str(calib_file2)
    window._on_calib_start()
    for _ in range(4):
        window._on_live_frame(window.store.frames[0])
    assert window._calib_acc.is_complete(), "4 次敲击后应全部完成"
    window._on_calib_finish()
    assert window._calib_acc is None
    assert calib_file2.exists(), "标定集未写入"
    assert window._calib_data is not None
    assert window._calib_data.n_valid_points == 4
    assert "已标定 4 点" in window.calib_status.text(), window.calib_status.text()

    # ---- 热图方向回归：亮斑压红点、行索引沿 y 向上（防 [::-1] 镜像回归） ----
    window.tabs.setCurrentIndex(1)
    pump(app, 0.15)

    def fake_loc(iy: int, ix: int) -> SimpleNamespace:
        grid = np.full((6, 6), 1.0)
        grid[iy, ix] = 0.0
        x = (ix + 0.5) / 6.0 * window.settings.ab_mm
        y = (iy + 0.5) / 6.0 * window.settings.ad_mm
        return SimpleNamespace(success=True, resid_map=grid,
                               extent=(0.0, float(window.settings.ab_mm),
                                       0.0, float(window.settings.ad_mm)),
                               x=x, y=y, x_raw=x, y_raw=y,
                               out_of_bounds=False)

    def bright_center():
        pix = window.top_view.grab().toImage()
        assert pix.height() > 0, "top_view 抓图为空"
        lum = np.zeros((pix.height(), pix.width()))
        for yy in range(pix.height()):
            for xx in range(pix.width()):
                c = pix.pixelColor(xx, yy)
                lum[yy, xx] = 0.299*c.red() + 0.587*c.green() + 0.114*c.blue()
        # 纯黄像元（残差最小格）；阈值挤掉近白的板框线（213.6 < 215.4*0.995）
        ys, xs = np.nonzero(lum >= lum.max() * 0.995)
        assert xs.size > 0, "未找到热图亮斑（图像未上屏？）"
        return float(xs.mean()), float(ys.mean())

    # 亮斑质心必须在红点正下方（否则热图被 y 镜像）
    loc = fake_loc(1, 1)
    window.top_view.show_hit(window.settings, loc)
    pump(app, 0.15)
    bx1, by1 = bright_center()
    wp = window.top_view.glw.mapFromScene(
        window.top_view.final_mark.mapToScene(QPointF(loc.x, loc.y)))
    assert 0 <= wp.x() <= window.top_view.width(), "红点像素映射越界（布局未就绪？）"
    assert abs(bx1 - wp.x()) < 40 and abs(by1 - wp.y()) < 40, \
        f"热图亮斑({bx1:.0f},{by1:.0f})未压在红点({wp.x()},{wp.y()})下——疑似 y 镜像"

    # 行索引沿 y 向上、列索引沿 x 向右
    window.top_view.show_hit(window.settings, fake_loc(4, 4))
    pump(app, 0.15)
    bx2, by2 = bright_center()
    assert by1 > by2 + 10 and bx1 < bx2 - 10, \
        f"热图方向异常：(1,1)@({bx1:.0f},{by1:.0f}) vs (4,4)@({bx2:.0f},{by2:.0f})"

    # ---- HCMD 命令 UI：嵌套选项卡 + 门控 + ACK 路由 ----
    assert window.analysis_tabs.count() == 3, \
        f"嵌套选项卡数 {window.analysis_tabs.count()}"
    titles = [window.analysis_tabs.tabText(i)
              for i in range(window.analysis_tabs.count())]
    assert titles == ["定位标定", "击打参数", "参照替换"], titles
    assert not window._btn_param_write.isEnabled(), "回放模式下参数写入按钮应禁用"
    assert not window._btn_ref_write.isEnabled(), "回放模式下参照按钮应禁用"
    assert window._serial_ready is False
    assert "n=128" in window.ref_frame_label.text(), window.ref_frame_label.text()
    assert "trig=" in window.ref_frame_label.text(), window.ref_frame_label.text()

    # 假连接状态走真实 ACK 路由（串口对象不启动、不发任何字节）
    window.worker = SerialWorker("COM_DOES_NOT_EXIST", 921600)
    window._serial_ready = True
    window._update_command_buttons()
    try:
        assert window._btn_param_write.isEnabled(), "串口模式下按钮应使能"
        window._handle_cmd_line(
            "[CMD] params P01=14 P03=5 P04=30000 P05=250 P26=80 P27=700")
        assert window._param_spins["P01"].value() == 14
        assert window._param_spins["P04"].value() == 30000
        assert window._param_spins["P27"].value() == 700
        assert "params" in window.params_cmd_status.text()
        window._handle_cmd_line("[CMD] err set-params P05=5 range=50..1000")
        assert "err" in window.params_cmd_status.text(), \
            window.params_cmd_status.text()
        window._handle_cmd_line("[CMD] ok save seq=1 err=0")
        assert "seq=1" in window.params_save_label.text(), \
            window.params_save_label.text()
        window._handle_cmd_line("[CMD] ok ref n=128 flash=0")
        assert "n=128" in window.ref_cmd_status.text(), window.ref_cmd_status.text()

        # 链发存档：参数同步 ok 后自动发 save
        calls: list = []
        window._send_command = lambda verb, data: calls.append(verb) or True
        window._cmd_chain_save = True
        window._handle_cmd_line(
            "[CMD] ok set-params P01=13 P03=4 P04=25000 P05=200 P26=64 P27=609")
        assert calls == ["save"], calls
    finally:
        window._cmd_chain_save = False
        window._cmd_inflight = None
        window.worker = None
        window._serial_ready = False
        window._update_command_buttons()
        assert not window._btn_param_write.isEnabled(), "断开后按钮应回到禁用"

    # ---- 通道映射改动即时生效（同时使落点分析降级而非崩溃） ----
    window._map_combos["1"].setCurrentIndex(2)   # ch1 → c
    pump(app, 0.05)
    assert window.settings.chan_map["1"] == "c"
    assert window._last_loc is not None and not window._last_loc.success
    assert "映射" in window._last_loc.note, window._last_loc.note

    calib_file.unlink(missing_ok=True)
    calib_file2.unlink(missing_ok=True)
    window.close()
    print("GUI smoke OK：3 帧入库、2 CRC 错、曲线上屏、双页签、"
          "未标定提示、合成标定定位、历史点、热图方向、标定工作流、"
          "嵌套选项卡与 HCMD 命令 UI、映射联动正常")


if __name__ == "__main__":
    main()
