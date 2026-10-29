# -*- coding: utf-8 -*-
"""展示层：四通道冻结窗波形图（pyqtgraph，4 子图共享 X 轴）。

每个子图：hp 波形 + 零线 + 触发样本标记（黄虚线）+ 窗内峰值标记（灰点线）。
通道标签按「通道 → 传感器」映射显示（如 ch1→a）。X 轴联动缩放。
"""
from __future__ import annotations

import numpy as np
import pyqtgraph as pg
from PySide6.QtCore import Qt
from PySide6.QtWidgets import QVBoxLayout, QWidget


class WavePlot(QWidget):
    PENS = ("#29b6f6", "#ffa726", "#66bb6a", "#ef5350")
    TRIG_PEN = pg.mkPen("#ffb300", width=1, style=Qt.PenStyle.DashLine)
    PEAK_PEN = pg.mkPen("#b0bec5", width=1, style=Qt.PenStyle.DotLine)
    ZERO_PEN = pg.mkPen("#546e7a", width=1, style=Qt.PenStyle.DotLine)

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)

        self.glw = pg.GraphicsLayoutWidget()
        layout.addWidget(self.glw)

        self.plots: list[pg.PlotItem] = []
        self.curves: list[pg.PlotDataItem] = []
        self.trig_lines: list[pg.InfiniteLine] = []
        self.peak_lines: list[pg.InfiniteLine] = []

        for ch in range(4):
            p = self.glw.addPlot(row=ch, col=0)
            p.showGrid(x=True, y=True, alpha=0.15)
            p.setMenuEnabled(False)
            p.enableAutoRange(axis="x", enable=False)

            # 绘制顺序：参考线在下，波形在上
            p.addItem(pg.InfiniteLine(angle=0, pos=0, pen=self.ZERO_PEN))
            trig = pg.InfiniteLine(angle=90, movable=False, pen=self.TRIG_PEN)
            peak = pg.InfiniteLine(angle=90, movable=False, pen=self.PEAK_PEN)
            p.addItem(trig, ignoreBounds=True)
            p.addItem(peak, ignoreBounds=True)

            curve = p.plot(pen=pg.mkPen(self.PENS[ch], width=1))
            p.setLabel("left", f"ch{ch + 1}", size="9pt")
            if ch > 0:
                p.setXLink(self.plots[0])
                p.hideAxis("bottom")
            else:
                p.setLabel("bottom", "样本索引（采样率见帧头 sample_hz）", size="9pt")

            self.plots.append(p)
            self.curves.append(curve)
            self.trig_lines.append(trig)
            self.peak_lines.append(peak)

    # ------------------------------------------------------------------
    def show_frame(self, frame, p22: int, chan_map: dict) -> None:
        """显示一帧：波形 + 触发/峰值标记 + 通道映射标签。"""
        n = int(frame.n)
        x = np.arange(n)
        trig_idx = frame.trig_index(p22)
        pk_idx = max(0, min(n - 1, int(frame.pk_idx)))

        for ch in range(4):
            self.curves[ch].setData(x, frame.wave[:, ch].astype(np.float64))
            name = chan_map.get(str(ch + 1), "?")
            self.plots[ch].setLabel("left", f"ch{ch + 1}→{name}", size="9pt")
            self.trig_lines[ch].setValue(trig_idx)
            self.peak_lines[ch].setValue(pk_idx)
            self.plots[ch].setXRange(0, max(1, n - 1), padding=0)

    def clear(self) -> None:
        for ch in range(4):
            self.curves[ch].setData([], [])
