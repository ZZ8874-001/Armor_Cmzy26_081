# -*- coding: utf-8 -*-
"""展示层：击打面俯视图——几何、EMA 残差热图、落点与历史（pyqtgraph）。

坐标约定与 settings 一致：原点 = a 角，x 沿 AB，y 沿 AD，单位 mm。
图层（自下而上）：残差热图 → 板矩形/传感器四边形/形心 → 标定高亮点 →
历史散点 → 原始估计点（越界时）+ 越界线 → 最终落点。
"""
from __future__ import annotations

from collections import deque

import numpy as np
import pyqtgraph as pg
from PySide6.QtCore import QRectF, Qt
from PySide6.QtWidgets import QVBoxLayout, QWidget

from .settings import SENSOR_NAMES

_CORNER_ORDER = ("a", "b", "d", "c")     # 绕板一周（a→b→d→c→a）
_HISTORY_MAX = 50


class TopView(QWidget):
    PLATE_PEN = pg.mkPen("#cfd8dc", width=2)
    QUAD_PEN = pg.mkPen("#607d8b", width=1, style=Qt.PenStyle.DashLine)
    CENTER_PEN = pg.mkPen("#78909c", width=1, style=Qt.PenStyle.DotLine)
    RAW_PEN = pg.mkPen("#ef5350", width=2, style=Qt.PenStyle.DashLine)
    OOB_PEN = pg.mkPen("#ff1744", width=1, style=Qt.PenStyle.DashDotLine)
    TAP_PEN = pg.mkPen("#ffd54f", width=2)
    FINAL_PEN = pg.mkPen("#ff1744", width=1)
    FINAL_BRUSH = pg.mkBrush("#ff1744")

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)

        self.glw = pg.GraphicsLayoutWidget()
        layout.addWidget(self.glw)

        self.plot = self.glw.addPlot()
        self.plot.setAspectLocked(True, ratio=1)
        self.plot.showGrid(x=True, y=True, alpha=0.15)
        self.plot.setMenuEnabled(False)
        self.plot.setLabel("bottom", "x 沿 AB（mm）", size="9pt")
        self.plot.setLabel("left", "y 沿 AD（mm）", size="9pt")

        # 残差热图：低残差 = 高置信度 → 反向 viridis（亮黄 = 更像落点）
        self._cmap = pg.colormap.get("viridis")
        self._cmap.reverse()
        self.img = pg.ImageItem(axisOrder="row-major")
        self.img.setColorMap(self._cmap)
        self.img.setVisible(False)
        self.plot.addItem(self.img)

        self.plate = self.plot.plot(pen=self.PLATE_PEN)
        self.quad = self.plot.plot(pen=self.QUAD_PEN)
        self.center_mark = self.plot.plot(pen=self.CENTER_PEN, symbol="+",
                                          symbolSize=10)
        self.sensors = pg.ScatterPlotItem(
            size=9, brush=pg.mkBrush("#29b6f6"), pen=pg.mkPen("#29b6f6"))
        self.plot.addItem(self.sensors)
        self.sensor_labels = []
        for _ in SENSOR_NAMES:
            label = pg.TextItem("", color="#b0bec5", anchor=(0.5, 1.0))
            self.plot.addItem(label)
            self.sensor_labels.append(label)

        self.history_scatter = pg.ScatterPlotItem(size=7)
        self.plot.addItem(self.history_scatter)
        self._history = deque(maxlen=_HISTORY_MAX)

        self.raw_mark = pg.ScatterPlotItem(
            size=12, symbol="o", pen=self.RAW_PEN, brush=None)
        self.final_mark = pg.ScatterPlotItem(
            size=9, symbol="o", pen=self.FINAL_PEN, brush=self.FINAL_BRUSH)
        self.oob_line = self.plot.plot(pen=self.OOB_PEN)
        self.tap_mark = pg.ScatterPlotItem(
            size=18, symbol="o", pen=self.TAP_PEN, brush=None)
        self.tap_label = pg.TextItem("", color="#ffd54f", anchor=(0.5, -0.6))
        for item in (self.raw_mark, self.final_mark, self.tap_mark):
            self.plot.addItem(item)
            item.setVisible(False)
        self.plot.addItem(self.tap_label)
        self.tap_label.setVisible(False)

        self._geometry_key = None
        self.set_geometry_from_values(
            300.0, 300.0, {"a": (0.0, 0.0), "b": (300.0, 0.0),
                           "c": (0.0, 300.0), "d": (300.0, 300.0)})

    # ------------------------------------------------------------------
    @property
    def history(self):
        return self._history

    def set_geometry(self, settings) -> None:
        """按设置更新板面/传感器；几何未变时直接返回（避免重置用户缩放）。"""
        self.set_geometry_from_values(
            float(settings.ab_mm), float(settings.ad_mm),
            {name: settings.sensor(name) for name in SENSOR_NAMES})

    def set_geometry_from_values(self, ab_mm: float, ad_mm: float,
                                 sensors: dict) -> None:
        ab, ad = float(ab_mm), float(ad_mm)
        pts = {name: tuple(float(v) for v in sensors.get(name, (0.0, 0.0)))
               for name in SENSOR_NAMES}
        key = (ab, ad, tuple(pts[name] for name in SENSOR_NAMES))
        if key == self._geometry_key:
            return
        self._geometry_key = key

        self.plate.setData([0.0, ab, ab, 0.0, 0.0],
                           [0.0, 0.0, ad, ad, 0.0])
        seq = [pts[name] for name in _CORNER_ORDER] + [pts[_CORNER_ORDER[0]]]
        self.quad.setData([p[0] for p in seq], [p[1] for p in seq])
        self.quad.setVisible(not self._is_rect(pts, ab, ad))
        self.sensors.setData([pts[name][0] for name in SENSOR_NAMES],
                             [pts[name][1] for name in SENSOR_NAMES])
        for label, name in zip(self.sensor_labels, SENSOR_NAMES):
            label.setText(name)
            label.setPos(*pts[name])
        self.center_mark.setData([ab / 2.0], [ad / 2.0])
        span_x = max(ab, 1.0)
        span_y = max(ad, 1.0)
        self.plot.setXRange(-0.08 * span_x, 1.08 * span_x, padding=0)
        self.plot.setYRange(-0.08 * span_y, 1.08 * span_y, padding=0)

    @staticmethod
    def _is_rect(pts: dict, ab: float, ad: float) -> bool:
        corners = {"a": (0.0, 0.0), "b": (ab, 0.0),
                   "c": (0.0, ad), "d": (ab, ad)}
        return all(abs(pts[name][0] - xy[0]) < 0.5
                   and abs(pts[name][1] - xy[1]) < 0.5
                   for name, xy in corners.items())

    # ------------------------------------------------------------------
    def show_hit(self, settings, loc) -> None:
        """显示一帧的落点结果；loc 无效时只清掉本帧标记（历史保留）。"""
        if loc is None or not getattr(loc, "success", False) \
                or loc.resid_map is None:
            self.clear_hit()
            return

        arr = np.array(loc.resid_map, dtype=np.float64)
        arr[~np.isfinite(arr)] = np.nan
        # resid_map 行 = mesh_y（行 0 在板下边 y=0），ImageItem 行 0 渲染在
        # setRect 的 y 最小边，与 y-up 视图一致——勿加 [::-1]，否则热图沿 y 镜像
        finite = arr[np.isfinite(arr)]
        if finite.size:
            vmax = float(np.max(finite))
            self.img.setImage(arr, autoLevels=False,
                              levels=(0.0, vmax if vmax > 0.0 else 1.0))
            x0, x1, y0, y1 = loc.extent
            self.img.setRect(QRectF(x0, y0, max(x1 - x0, 1e-3),
                                    max(y1 - y0, 1e-3)))
            self.img.setVisible(True)
        else:
            self.img.setVisible(False)

        self.final_mark.setData([loc.x], [loc.y])
        self.final_mark.setVisible(True)
        if loc.out_of_bounds:
            self.raw_mark.setData([loc.x_raw], [loc.y_raw])
            self.raw_mark.setVisible(True)
            x0, x1, y0, y1 = loc.extent
            self.oob_line.setData(
                [(x0 + x1) / 2.0, loc.x_raw], [(y0 + y1) / 2.0, loc.y_raw])
        else:
            self.raw_mark.setVisible(False)
            self.oob_line.setData([], [])

    def clear_hit(self) -> None:
        self.img.setVisible(False)
        self.raw_mark.setVisible(False)
        self.final_mark.setVisible(False)
        self.oob_line.setData([], [])

    def append_history(self, x: float, y: float) -> None:
        self._history.append((float(x), float(y)))
        self._rebuild_history()

    def _rebuild_history(self) -> None:
        n = len(self._history)
        spots = []
        for idx, (x, y) in enumerate(self._history):
            alpha = 40 + int(180 * (idx + 1) / n)
            spots.append({"pos": (x, y),
                          "brush": pg.mkBrush(255, 167, 38, alpha),
                          "pen": None,
                          "size": 7})
        self.history_scatter.setData(spots)

    def clear(self) -> None:
        self.clear_hit()
        self.clear_highlight()
        self._history.clear()
        self.history_scatter.setData([])

    # ------------------------------------------------------------------
    def highlight_tap_point(self, xy, text: str = "") -> None:
        self.tap_mark.setData([xy[0]], [xy[1]])
        self.tap_mark.setVisible(True)
        self.tap_label.setText(text)
        self.tap_label.setPos(xy[0], xy[1])
        self.tap_label.setVisible(bool(text))

    def clear_highlight(self) -> None:
        self.tap_mark.setVisible(False)
        self.tap_label.setVisible(False)
