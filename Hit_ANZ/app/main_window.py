# -*- coding: utf-8 -*-
"""展示层：主窗口——工具栏 + 双页签（波形 / 落点分析）+ 设置面板 + 日志。

数据流：SourceWorker(QThread) --chunk(bytes)--> _on_chunk(主线程)
        → FrameStore.add_raw（原始流缓冲，供导出）
        → FrameParser.feed（帧/文本行分流）
        → 波形图刷新 + 落点分析 + 日志 + 帧历史下拉 + 状态栏。
标定模式下，新到达的帧额外送入 analyzer 的逐点采集器（仍正常显示）。
设置改动即写入 config.json（settings.save）。
"""
from __future__ import annotations

import html
import time
from pathlib import Path

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QAction, QFont
from PySide6.QtWidgets import (
    QAbstractItemView, QCheckBox, QComboBox, QDoubleSpinBox, QFileDialog,
    QFormLayout, QGroupBox, QHBoxLayout, QHeaderView, QLabel, QMainWindow,
    QMessageBox, QPlainTextEdit, QPushButton, QScrollArea, QSpinBox, QSplitter,
    QTabWidget, QTableWidget, QTableWidgetItem, QToolBar, QVBoxLayout, QWidget,
)

from . import analyzer, commands
from .plot_widget import WavePlot
from .protocol import FrameParser
from .settings import SENSOR_NAMES, Settings
from .sources import FileReplayWorker, SerialWorker, SourceWorker, available_ports
from .store import FrameStore
from .top_view import TopView

BAUD_CHOICES = ("921600", "460800", "230400", "115200", "57600", "9600")


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("Hit_ANZ 装甲板波形上位机")
        self.resize(1360, 900)

        self.settings = Settings.load()
        self.parser = FrameParser()
        self.store = FrameStore()
        self.worker: SourceWorker | None = None
        self._loading = True            # 程序化改控件期间抑制保存
        self._calib_data = None         # 已载入/刚保存的 CalibData
        self._calib_acc = None          # 标定模式下的采集器
        self._last_loc = None           # 最近一次分析的 LocResult
        self._serial_ready = False      # HCMD：串口已打开（命令使能门）
        self._cmd_inflight = None       # HCMD：在飞命令 verb（一次一令）
        self._cmd_chain_save = False    # HCMD：参数同步成功后链发存档
        self._cmd_timer = QTimer(self)  # HCMD：2s 回执超时
        self._cmd_timer.setSingleShot(True)
        self._cmd_timer.setInterval(2000)
        self._cmd_timer.timeout.connect(self._on_cmd_timeout)

        self._build_toolbar()
        self._build_central()
        self._build_statusbar()
        self._apply_settings_to_widgets()
        self._reload_calib(quiet=True)
        self._loading = False
        self._update_status()

    # ================= 构建 UI =================
    def _build_toolbar(self) -> None:
        tb = QToolBar("主工具栏")
        tb.setMovable(False)
        self.addToolBar(tb)

        self.act_connect = QAction("连接串口", self)
        self.act_connect.triggered.connect(self._on_connect)
        self.act_disconnect = QAction("断开", self)
        self.act_disconnect.triggered.connect(self._stop_worker)
        self.act_disconnect.setEnabled(False)
        self.act_refresh = QAction("刷新端口", self)
        self.act_refresh.triggered.connect(lambda: self._refresh_ports())
        self.act_replay = QAction("回放文件…", self)
        self.act_replay.triggered.connect(self._on_replay)
        self.act_export = QAction("导出原始流…", self)
        self.act_export.triggered.connect(self._on_export_raw)
        self.act_clear_log = QAction("清空日志", self)
        self.act_clear_log.triggered.connect(lambda: self.log.clear())

        for act in (self.act_connect, self.act_disconnect, self.act_refresh):
            tb.addAction(act)
        tb.addSeparator()
        for act in (self.act_replay, self.act_export):
            tb.addAction(act)
        tb.addSeparator()
        tb.addAction(self.act_clear_log)

    def _build_central(self) -> None:
        splitter = QSplitter(Qt.Orientation.Vertical)

        top = QWidget()
        top_h = QHBoxLayout(top)
        top_h.setContentsMargins(4, 4, 4, 0)

        left = QWidget()
        left_v = QVBoxLayout(left)
        left_v.setContentsMargins(0, 0, 0, 0)

        row = QHBoxLayout()
        row.addWidget(QLabel("历史帧："))
        self.frame_combo = QComboBox()
        self.frame_combo.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToContents)
        self.frame_combo.activated.connect(self._on_frame_selected)
        row.addWidget(self.frame_combo, 1)
        left_v.addLayout(row)

        self.meta_label = QLabel(
            "等待数据…　（黄虚线 = 触发样本 n−P22−1，灰点线 = 窗内 |hp_sum| 峰值）")
        self.meta_label.setWordWrap(True)
        self.meta_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        left_v.addWidget(self.meta_label)

        self.tabs = QTabWidget()
        self.plot = WavePlot()
        self.tabs.addTab(self.plot, "波形")
        self.tabs.addTab(self._build_analysis_tab(), "落点分析")
        self.tabs.currentChanged.connect(self._on_tab_changed)
        left_v.addWidget(self.tabs, 1)

        top_h.addWidget(left, 1)
        top_h.addWidget(self._build_settings_panel())

        splitter.addWidget(top)

        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(2000)
        self.log.setFont(QFont("Consolas", 9))
        splitter.addWidget(self.log)

        splitter.setStretchFactor(0, 4)
        splitter.setStretchFactor(1, 1)
        self.setCentralWidget(splitter)

    def _build_settings_panel(self) -> QWidget:
        panel = QWidget()
        panel.setFixedWidth(400)
        pv = QVBoxLayout(panel)
        pv.setContentsMargins(4, 0, 4, 4)

        # ---- 串口 ----
        gb_serial = QGroupBox("串口")
        f = QFormLayout(gb_serial)
        self._port_combo = QComboBox()
        self._port_combo.currentIndexChanged.connect(self._on_port_changed)
        row = QHBoxLayout()
        row.addWidget(self._port_combo, 1)
        refresh_btn = QPushButton("刷新")
        refresh_btn.clicked.connect(lambda: self._refresh_ports())
        row.addWidget(refresh_btn)
        f.addRow("端口", row)
        self._baud_combo = QComboBox()
        self._baud_combo.setEditable(True)
        self._baud_combo.addItems(BAUD_CHOICES)
        self._baud_combo.currentTextChanged.connect(self._on_baud_changed)
        f.addRow("波特率", self._baud_combo)
        pv.addWidget(gb_serial)

        # ---- 几何 ----
        gb_geo = QGroupBox("击打面几何")
        f = QFormLayout(gb_geo)
        self._ab_spin = QDoubleSpinBox()
        self._ab_spin.setRange(10.0, 5000.0)
        self._ab_spin.setDecimals(1)
        self._ab_spin.setSuffix(" mm")
        self._ab_spin.valueChanged.connect(self._on_geometry_changed)
        f.addRow("AB 长度（横）", self._ab_spin)
        self._ad_spin = QDoubleSpinBox()
        self._ad_spin.setRange(10.0, 5000.0)
        self._ad_spin.setDecimals(1)
        self._ad_spin.setSuffix(" mm")
        self._ad_spin.valueChanged.connect(self._on_geometry_changed)
        f.addRow("AD 长度（纵）", self._ad_spin)
        self._follow_check = QCheckBox("传感器位置跟随矩形四角")
        self._follow_check.toggled.connect(self._on_follow_toggled)
        f.addRow(self._follow_check)
        pv.addWidget(gb_geo)

        # ---- 传感器位置 ----
        gb_pos = QGroupBox("传感器位置（原点 = a 角，x 沿 AB，y 沿 AD）")
        f = QFormLayout(gb_pos)
        self._sx_spins: dict[str, QDoubleSpinBox] = {}
        self._sy_spins: dict[str, QDoubleSpinBox] = {}
        for name in SENSOR_NAMES:
            sx = QDoubleSpinBox()
            sy = QDoubleSpinBox()
            for spin, axis in ((sx, "x"), (sy, "y")):
                spin.setRange(-1000.0, 5000.0)
                spin.setDecimals(1)
                spin.setMinimumWidth(120)
                spin.setPrefix(f"{axis} ")
                spin.setSuffix(" mm")
                spin.valueChanged.connect(self._on_sensor_pos_changed)
            row = QHBoxLayout()
            row.addWidget(sx, 1)
            row.addWidget(sy, 1)
            f.addRow(f"传感器 {name}", row)
            self._sx_spins[name] = sx
            self._sy_spins[name] = sy
        pv.addWidget(gb_pos)

        # ---- 通道映射 ----
        gb_map = QGroupBox("通道 → 传感器 映射")
        f = QFormLayout(gb_map)
        self._map_combos: dict[str, QComboBox] = {}
        for ch in ("1", "2", "3", "4"):
            combo = QComboBox()
            for name in SENSOR_NAMES:
                combo.addItem(name, name)
            combo.currentIndexChanged.connect(self._on_chan_map_changed)
            f.addRow(f"ch{ch} →", combo)
            self._map_combos[ch] = combo
        pv.addWidget(gb_map)

        # ---- 检测参数（仅影响标记位置） ----
        gb_det = QGroupBox("检测参数（标记位置用）")
        f = QFormLayout(gb_det)
        self._p21_spin = QSpinBox()
        self._p21_spin.setRange(0, 128)
        self._p21_spin.valueChanged.connect(self._on_det_param_changed)
        f.addRow("P21 预触发样本", self._p21_spin)
        self._p22_spin = QSpinBox()
        self._p22_spin.setRange(0, 128)
        self._p22_spin.valueChanged.connect(self._on_det_param_changed)
        f.addRow("P22 后触发样本", self._p22_spin)
        pv.addWidget(gb_det)

        hint = QLabel("设置改动即时保存到 config.json，重启自动还原。")
        hint.setWordWrap(True)
        hint.setStyleSheet("color: #78909c;")
        pv.addWidget(hint)
        pv.addStretch(1)

        scroll = QScrollArea()
        scroll.setWidget(panel)
        scroll.setWidgetResizable(True)
        scroll.setFixedWidth(420)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        return scroll

    def _build_analysis_tab(self) -> QWidget:
        page = QWidget()
        h = QHBoxLayout(page)
        h.setContentsMargins(4, 4, 4, 4)

        self.top_view = TopView()
        h.addWidget(self.top_view, 1)

        panel = QWidget()
        panel.setFixedWidth(340)
        pv = QVBoxLayout(panel)
        pv.setContentsMargins(4, 0, 4, 4)

        # ---- 击打特征 ----
        gb_feat = QGroupBox("击打特征（当前帧，按传感器 a..d）")
        fv = QVBoxLayout(gb_feat)
        self.feature_table = QTableWidget(4, 4)
        self.feature_table.setHorizontalHeaderLabels(
            ["传感器", "峰值", "冲量 counts·s", "RMS"])
        self.feature_table.verticalHeader().setVisible(False)
        self.feature_table.horizontalHeader().setSectionResizeMode(
            QHeaderView.ResizeMode.Stretch)
        self.feature_table.setEditTriggers(
            QAbstractItemView.EditTrigger.NoEditTriggers)
        self.feature_table.setSelectionMode(
            QAbstractItemView.SelectionMode.NoSelection)
        self.feature_table.setFixedHeight(170)
        for i, name in enumerate(SENSOR_NAMES):
            self.feature_table.setItem(i, 0, QTableWidgetItem(name))
            for col in (1, 2, 3):
                item = QTableWidgetItem("—")
                item.setTextAlignment(Qt.AlignmentFlag.AlignRight
                                      | Qt.AlignmentFlag.AlignVCenter)
                self.feature_table.setItem(i, col, item)
        fv.addWidget(self.feature_table)
        self.sum_label = QLabel("Σ冲量 —")
        fv.addWidget(self.sum_label)
        pv.addWidget(gb_feat)

        # ---- 落点估计 ----
        gb_res = QGroupBox("落点估计")
        fr = QFormLayout(gb_res)
        self.feature_combo = QComboBox()
        self.feature_combo.addItem("冲量（推荐）", "impulse")
        self.feature_combo.addItem("峰值", "peak")
        self.feature_combo.addItem("RMS", "rms")
        self.feature_combo.currentIndexChanged.connect(self._on_loc_feature_changed)
        fr.addRow("质心特征源", self.feature_combo)
        self.centroid_label = QLabel("—")
        fr.addRow("加权质心（对照）", self.centroid_label)
        self.ema_label = QLabel("—")
        fr.addRow("EMA 落点", self.ema_label)
        self.resid_label = QLabel("—")
        fr.addRow("残差/置信度", self.resid_label)
        self.force_label = QLabel("—")
        fr.addRow("力历史冲量/RMS", self.force_label)
        self.loc_status = QLabel("尚未分析")
        self.loc_status.setWordWrap(True)
        self.loc_status.setStyleSheet("color: #78909c;")
        fr.addRow(self.loc_status)
        pv.addWidget(gb_res)

        # ---- 标定 ----
        gb_cal = QGroupBox("标定（冲击锤逐点敲击）")
        fc = QFormLayout(gb_cal)
        self.calib_status = QLabel("未标定")
        self.calib_status.setWordWrap(True)
        fc.addRow(self.calib_status)

        grid_row = QHBoxLayout()
        self._rows_spin = QSpinBox()
        self._rows_spin.setRange(2, 10)
        self._rows_spin.setPrefix("行 ")
        self._cols_spin = QSpinBox()
        self._cols_spin.setRange(2, 10)
        self._cols_spin.setPrefix("列 ")
        for spin in (self._rows_spin, self._cols_spin):
            spin.valueChanged.connect(self._on_loc_settings_changed)
        grid_row.addWidget(self._rows_spin)
        grid_row.addWidget(self._cols_spin)
        fc.addRow("网格", grid_row)

        self._taps_spin = QSpinBox()
        self._taps_spin.setRange(1, 10)
        self._taps_spin.setSuffix(" 次/点")
        self._taps_spin.valueChanged.connect(self._on_loc_settings_changed)
        fc.addRow("每点敲击", self._taps_spin)

        band_row = QHBoxLayout()
        self._fmin_spin = QSpinBox()
        self._fmin_spin.setRange(0, 1900)
        self._fmin_spin.setSingleStep(10)
        self._fmin_spin.setSuffix(" Hz")
        self._fmax_spin = QSpinBox()
        self._fmax_spin.setRange(100, 1953)
        self._fmax_spin.setSingleStep(10)
        self._fmax_spin.setSuffix(" Hz")
        for spin in (self._fmin_spin, self._fmax_spin):
            spin.valueChanged.connect(self._on_loc_settings_changed)
        band_row.addWidget(self._fmin_spin)
        band_row.addWidget(self._fmax_spin)
        fc.addRow("频带", band_row)

        path_row = QHBoxLayout()
        self.calib_path_label = QLabel("—")
        self.calib_path_label.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse)
        path_row.addWidget(self.calib_path_label, 1)
        browse_btn = QPushButton("选择…")
        browse_btn.clicked.connect(self._on_calib_browse)
        path_row.addWidget(browse_btn)
        fc.addRow("数据集", path_row)

        self._btn_calib_start = QPushButton("开始标定")
        self._btn_calib_finish = QPushButton("完成并保存")
        self._btn_calib_skip = QPushButton("跳过此点")
        self._btn_calib_retap = QPushButton("重敲此点")
        self._btn_calib_cancel = QPushButton("取消")
        self._btn_calib_start.clicked.connect(self._on_calib_start)
        self._btn_calib_finish.clicked.connect(self._on_calib_finish)
        self._btn_calib_skip.clicked.connect(self._on_calib_skip)
        self._btn_calib_retap.clicked.connect(self._on_calib_retap)
        self._btn_calib_cancel.clicked.connect(self._on_calib_cancel)
        row1 = QHBoxLayout()
        row1.addWidget(self._btn_calib_start)
        row1.addWidget(self._btn_calib_finish)
        row2 = QHBoxLayout()
        row2.addWidget(self._btn_calib_skip)
        row2.addWidget(self._btn_calib_retap)
        row2.addWidget(self._btn_calib_cancel)
        fc.addRow(row1)
        fc.addRow(row2)

        self.calib_progress = QPlainTextEdit()
        self.calib_progress.setReadOnly(True)
        self.calib_progress.setMaximumBlockCount(200)
        self.calib_progress.setFont(QFont("Consolas", 8))
        self.calib_progress.setFixedHeight(110)
        fc.addRow(self.calib_progress)
        pv.addWidget(gb_cal)
        pv.addStretch(1)

        # 右侧嵌套选项卡：定位标定（现面板原样搬入）/ 击打参数 / 参照替换
        # （勿改名为 self.tabs——外层页签 smoke 断言 count==2）
        self.analysis_tabs = QTabWidget()
        self.analysis_tabs.setFixedWidth(380)
        self.analysis_tabs.addTab(self._scroll_wrap(panel), "定位标定")
        self.analysis_tabs.addTab(self._scroll_wrap(self._build_params_tab()),
                                  "击打参数")
        self.analysis_tabs.addTab(self._scroll_wrap(self._build_ref_tab()),
                                  "参照替换")
        h.addWidget(self.analysis_tabs)

        self._set_calib_buttons(False)
        self._update_command_buttons()
        return page

    @staticmethod
    def _scroll_wrap(widget: QWidget) -> QScrollArea:
        scroll = QScrollArea()
        scroll.setWidget(widget)
        scroll.setWidgetResizable(True)
        scroll.setFixedWidth(360)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        return scroll

    def _build_params_tab(self) -> QWidget:
        """击打参数页：精选 6 参数 spinbox + 读/写/存档/恢复默认（HCMD）。"""
        panel = QWidget()
        panel.setFixedWidth(340)
        pv = QVBoxLayout(panel)
        pv.setContentsMargins(4, 0, 4, 4)

        gb = QGroupBox("在线击打参数（写入即生效，掉电丢失）")
        f = QFormLayout(gb)
        self._param_spins: dict[str, QSpinBox] = {}
        for spec in commands.PARAM_SPECS:
            spin = QSpinBox()
            spin.setRange(spec.lo, spec.hi)
            spin.setSingleStep(spec.step)
            spin.setValue(spec.default)
            spin.setSuffix(f" {spec.unit}")
            f.addRow(f"{spec.key} {spec.label}", spin)
            self._param_spins[spec.key] = spin
        pv.addWidget(gb)

        self.params_cmd_status = QLabel("未连接（命令仅串口模式可用）")
        self.params_cmd_status.setWordWrap(True)
        self.params_cmd_status.setStyleSheet("color: #78909c;")
        pv.addWidget(self.params_cmd_status)

        self.params_save_label = QLabel("Flash 存档：未读取")
        self.params_save_label.setWordWrap(True)
        pv.addWidget(self.params_save_label)

        self._btn_param_read = QPushButton("读取当前值")
        self._btn_param_write = QPushButton("写入（生效）")
        self._btn_param_save = QPushButton("存档到 Flash")
        self._btn_param_defaults = QPushButton("恢复默认")
        self._btn_param_read.clicked.connect(self._on_params_read)
        self._btn_param_write.clicked.connect(self._on_params_write)
        self._btn_param_save.clicked.connect(self._on_params_save)
        self._btn_param_defaults.clicked.connect(self._on_params_defaults)
        row1 = QHBoxLayout()
        row1.addWidget(self._btn_param_read)
        row1.addWidget(self._btn_param_write)
        row2 = QHBoxLayout()
        row2.addWidget(self._btn_param_save)
        row2.addWidget(self._btn_param_defaults)
        pv.addLayout(row1)
        pv.addLayout(row2)

        hint = QLabel(
            "提示：值不保存到本机 config.json，板子才是真值，重连后按「读取当前值」"
            "同步。P01 改动会重算基线热身；P27=0 时相关度只统计不判拒；"
            "P21/P22 不开放（改窗长会毁参照与标定签名）。")
        hint.setWordWrap(True)
        hint.setStyleSheet("color: #78909c;")
        pv.addWidget(hint)
        pv.addStretch(1)
        return panel

    def _build_ref_tab(self) -> QWidget:
        """参照替换页：选帧 → 确认 → SET_REFERENCE（立即生效）。"""
        panel = QWidget()
        panel.setFixedWidth(340)
        pv = QVBoxLayout(panel)
        pv.setContentsMargins(4, 0, 4, 4)

        gb = QGroupBox("Flash 参照替换（立即生效）")
        f = QFormLayout(gb)
        self.ref_frame_label = QLabel("未选择帧")
        self.ref_frame_label.setWordWrap(True)
        f.addRow("当前帧", self.ref_frame_label)
        self.ref_warn_label = QLabel("")
        self.ref_warn_label.setWordWrap(True)
        self.ref_warn_label.setStyleSheet("color: #e53935;")
        f.addRow(self.ref_warn_label)
        pv.addWidget(gb)

        self._btn_ref_write = QPushButton("写入参照（立即生效）")
        self._btn_ref_write.clicked.connect(self._on_ref_write)
        pv.addWidget(self._btn_ref_write)

        self.ref_cmd_status = QLabel("未连接")
        self.ref_cmd_status.setWordWrap(True)
        self.ref_cmd_status.setStyleSheet("color: #78909c;")
        pv.addWidget(self.ref_cmd_status)

        hint = QLabel(
            "把历史帧下拉框所选击打的四通道 hp 求和窗推给板子，替换 Flash 页 63 "
            "参照并置位「本上电已训练」：立即参与相关度判定（基线热身 5τ 内仍不判拒）。"
            "坏参照恢复 = 再推一帧好参照；清空参照需调试器（命令通道不提供）。")
        hint.setWordWrap(True)
        hint.setStyleSheet("color: #78909c;")
        pv.addWidget(hint)
        pv.addStretch(1)
        return panel

    def _build_statusbar(self) -> None:
        self._status_source = QLabel("未连接")
        self._status_stats = QLabel("")
        self.statusBar().addWidget(self._status_source, 1)
        self.statusBar().addPermanentWidget(self._status_stats)

    # ================= HCMD 命令通道 =================
    def _update_command_buttons(self) -> None:
        connected = (isinstance(self.worker, SerialWorker)
                     and self._serial_ready)
        for btn in (self._btn_param_read, self._btn_param_write,
                    self._btn_param_save, self._btn_param_defaults,
                    self._btn_ref_write):
            btn.setEnabled(connected)

    def _send_command(self, verb: str, data: bytes) -> bool:
        if not (isinstance(self.worker, SerialWorker) and self._serial_ready):
            self._log("未连接串口，命令未发送", "warn")
            return False
        if self._cmd_inflight is not None:
            self._log(f"上一条命令（{self._cmd_inflight}）未收到回执，忽略", "warn")
            return False
        if not self.worker.send(data):
            self._log("串口写入失败", "warn")
            return False
        self._cmd_inflight = verb
        self._cmd_timer.start()
        return True

    def _on_cmd_timeout(self) -> None:
        if self._cmd_inflight is None:
            return
        self._log(f"命令 {self._cmd_inflight} 超时（板未应答）", "warn")
        self.params_cmd_status.setText(f"命令超时（{self._cmd_inflight}）")
        self.params_cmd_status.setStyleSheet("color: #e53935;")
        self._cmd_inflight = None
        self._cmd_chain_save = False

    def _handle_cmd_line(self, line: str) -> None:
        """把板子 ACK 文本行路由到命令状态机（仅串口模式生效）。"""
        if not (isinstance(self.worker, SerialWorker) and self._serial_ready):
            return
        ack = commands.parse_cmd_line(line)
        if ack is None:
            return
        if ack.verb in ("params", "set-params", "defaults") and ack.params:
            self._apply_params_to_spins(ack.params)
        color = "#43a047" if ack.ok else "#e53935"
        text = ack.text.removeprefix("[CMD] ")
        if ack.verb == "save":
            self.params_save_label.setText(f"Flash 存档：{text}")
        elif ack.verb == "ref":
            self.ref_cmd_status.setText(text)
            self.ref_cmd_status.setStyleSheet(f"color: {color};")
        else:
            self.params_cmd_status.setText(text)
            self.params_cmd_status.setStyleSheet(f"color: {color};")
        self._cmd_inflight = None
        self._cmd_timer.stop()
        if ack.ok and ack.verb == "set-params" and self._cmd_chain_save:
            self._cmd_chain_save = False
            if self._send_command("save", commands.build_save_params()):
                self.params_save_label.setText("Flash 存档：写入中…")

    def _apply_params_to_spins(self, params: dict) -> None:
        prev = self._loading
        self._loading = True
        try:
            for key, value in params.items():
                spin = self._param_spins.get(key)
                if spin is not None:
                    spec = commands.PARAM_SPECS_BY_KEY[key]
                    spin.setValue(min(spec.hi, max(spec.lo, int(value))))
        finally:
            self._loading = prev

    def _params_from_spins(self) -> dict:
        return {key: spin.value() for key, spin in self._param_spins.items()}

    def _on_params_read(self) -> None:
        if self._send_command("get-params", commands.build_get_params()):
            self.params_cmd_status.setText("已发送读取请求，等待回执…")
            self.params_cmd_status.setStyleSheet("color: #78909c;")

    def _on_params_write(self) -> None:
        try:
            data = commands.build_set_params(self._params_from_spins())
        except ValueError as exc:
            self._log(f"参数越界：{exc}", "warn")
            return
        if self._send_command("set-params", data):
            self.params_cmd_status.setText("已发送，等待回执…")
            self.params_cmd_status.setStyleSheet("color: #78909c;")

    def _on_params_save(self) -> None:
        try:
            data = commands.build_set_params(self._params_from_spins())
        except ValueError as exc:
            self._log(f"参数越界：{exc}", "warn")
            return
        if self._send_command("set-params", data):
            self._cmd_chain_save = True
            self.params_save_label.setText("Flash 存档：先同步参数…")

    def _on_params_defaults(self) -> None:
        if self._send_command("defaults", commands.build_set_defaults()):
            self.params_cmd_status.setText("已发送恢复默认，等待回执…")
            self.params_cmd_status.setStyleSheet("color: #78909c;")

    def _on_ref_write(self) -> None:
        frame = self._current_frame()
        if frame is None:
            self._log("没有可选帧，无法写入参照", "warn")
            return
        box = QMessageBox(self)
        box.setWindowTitle("写入参照")
        box.setText(f"将帧 #trig {frame.trig_id}（n={frame.n}，"
                    f"{frame.type_name}）的四通道求和窗写入板子 Flash 页 63 "
                    "并立即生效。")
        box.setInformativeText("坏参照会把真实击打判拒，直到再推一帧好参照。确认？")
        box.setStandardButtons(QMessageBox.StandardButton.Yes
                               | QMessageBox.StandardButton.No)
        box.setDefaultButton(QMessageBox.StandardButton.No)
        if box.exec() != QMessageBox.StandardButton.Yes:
            return
        try:
            data = commands.build_set_ref(frame)
        except ValueError as exc:
            self._log(f"该帧无法作为参照：{exc}", "warn")
            return
        if self._send_command("ref", data):
            self.ref_cmd_status.setText("已发送，等待回执…")
            self.ref_cmd_status.setStyleSheet("color: #78909c;")

    # ================= 设置应用 / 保存 =================
    def _apply_settings_to_widgets(self) -> None:
        self._refresh_ports(select=self.settings.port)
        self._baud_combo.setCurrentText(str(self.settings.baudrate))
        self._ab_spin.setValue(self.settings.ab_mm)
        self._ad_spin.setValue(self.settings.ad_mm)
        self._follow_check.setChecked(self.settings.follow_corners)
        for name in SENSOR_NAMES:
            x, y = self.settings.sensor(name)
            self._sx_spins[name].setValue(x)
            self._sy_spins[name].setValue(y)
            editable = not self.settings.follow_corners
            self._sx_spins[name].setEnabled(editable)
            self._sy_spins[name].setEnabled(editable)
        for ch, combo in self._map_combos.items():
            idx = combo.findData(self.settings.chan_map.get(ch, "a"))
            combo.setCurrentIndex(max(0, idx))
        self._p21_spin.setValue(self.settings.p21)
        self._p22_spin.setValue(self.settings.p22)
        idx = self.feature_combo.findData(self.settings.loc_feature)
        self.feature_combo.setCurrentIndex(max(0, idx))
        self._rows_spin.setValue(int(self.settings.calib_rows))
        self._cols_spin.setValue(int(self.settings.calib_cols))
        self._taps_spin.setValue(int(self.settings.taps_per_point))
        self._fmin_spin.setValue(int(round(self.settings.f_min_hz)))
        self._fmax_spin.setValue(int(round(self.settings.f_max_hz)))
        self._update_calib_status_label()

    def _save(self) -> None:
        if not self._loading:
            self.settings.save()

    def _refresh_ports(self, select: str | None = None) -> None:
        prev = self._loading
        self._loading = True
        try:
            target = select or self._port_combo.currentData() or self.settings.port
            self._port_combo.clear()
            ports = available_ports()
            if not ports:
                self._port_combo.addItem("（未发现串口）", "")
                self._port_combo.setCurrentIndex(0)
                return
            for device, desc in ports:
                text = f"{device}  {desc}".strip()
                self._port_combo.addItem(text, device)
            idx = self._port_combo.findData(target) if target else -1
            self._port_combo.setCurrentIndex(idx if idx >= 0 else 0)
        finally:
            self._loading = prev

    # ---- 各设置的槽 ----
    def _on_port_changed(self, _index: int) -> None:
        if self._loading:
            return
        self.settings.port = self._port_combo.currentData() or ""
        self._save()

    def _on_baud_changed(self, text: str) -> None:
        if self._loading:
            return
        try:
            self.settings.baudrate = int(text.strip())
        except ValueError:
            return
        self._save()

    def _on_geometry_changed(self, _value: float) -> None:
        if self._loading:
            return
        self.settings.ab_mm = self._ab_spin.value()
        self.settings.ad_mm = self._ad_spin.value()
        self.settings.sync_corners()
        self._save()
        if self.settings.follow_corners:
            self._apply_settings_to_widgets_geometry_only()
        self._refresh_analysis()

    def _apply_settings_to_widgets_geometry_only(self) -> None:
        prev = self._loading
        self._loading = True
        try:
            for name in SENSOR_NAMES:
                x, y = self.settings.sensor(name)
                self._sx_spins[name].setValue(x)
                self._sy_spins[name].setValue(y)
        finally:
            self._loading = prev

    def _on_follow_toggled(self, checked: bool) -> None:
        if self._loading:
            return
        self.settings.follow_corners = bool(checked)
        for name in SENSOR_NAMES:
            self._sx_spins[name].setEnabled(not checked)
            self._sy_spins[name].setEnabled(not checked)
        self.settings.sync_corners()
        self._save()
        if checked:
            self._apply_settings_to_widgets_geometry_only()
        self._refresh_analysis()

    def _on_sensor_pos_changed(self, _value: float) -> None:
        if self._loading or self.settings.follow_corners:
            return
        for name in SENSOR_NAMES:
            self.settings.sensors[name] = [self._sx_spins[name].value(),
                                           self._sy_spins[name].value()]
        self._save()
        self._refresh_analysis()

    def _on_chan_map_changed(self, _index: int) -> None:
        if self._loading:
            return
        for ch, combo in self._map_combos.items():
            self.settings.chan_map[ch] = combo.currentData() or "a"
        self._save()
        self._refresh_current()

    def _on_det_param_changed(self, _value: int) -> None:
        if self._loading:
            return
        self.settings.p21 = self._p21_spin.value()
        self.settings.p22 = self._p22_spin.value()
        self._save()
        self._refresh_current()

    def _on_loc_feature_changed(self, _index: int) -> None:
        if self._loading:
            return
        self.settings.loc_feature = self.feature_combo.currentData() or "impulse"
        self._save()
        self._refresh_analysis()

    def _on_loc_settings_changed(self, _value: int = 0) -> None:
        if self._loading:
            return
        self.settings.calib_rows = self._rows_spin.value()
        self.settings.calib_cols = self._cols_spin.value()
        self.settings.taps_per_point = self._taps_spin.value()
        self.settings.f_min_hz = float(self._fmin_spin.value())
        self.settings.f_max_hz = float(self._fmax_spin.value())
        self._save()
        self._refresh_analysis()

    def _on_tab_changed(self, index: int) -> None:
        if index == 1:
            self._refresh_analysis()

    # ================= 数据源 =================
    def _on_connect(self) -> None:
        port = self._port_combo.currentData() or ""
        if not port:
            QMessageBox.warning(self, "未选择串口", "请先选择串口（可点“刷新”重新枚举）。")
            return
        try:
            baud = int(self._baud_combo.currentText().strip())
        except ValueError:
            QMessageBox.warning(self, "波特率无效", "波特率必须是整数。")
            return
        self.settings.port = port
        self.settings.baudrate = baud
        self._save()
        self._start_worker(SerialWorker(port, baud))

    def _on_replay(self) -> None:
        start_dir = self.settings.last_open_dir or str(Path.cwd())
        path, _ = QFileDialog.getOpenFileName(
            self, "选择回放文件（原始抓包 .bin）", start_dir,
            "二进制抓包 (*.bin);;所有文件 (*)")
        if not path:
            return
        self.settings.last_open_dir = str(Path(path).parent)
        self._save()
        self._start_worker(FileReplayWorker(path))

    def _start_worker(self, worker: SourceWorker) -> None:
        if self.worker is not None:
            return
        self.parser.reset()
        self.store.clear()
        self.plot.clear()
        self.top_view.clear()
        self.frame_combo.clear()
        self._last_loc = None
        self._log("—— 新会话 ——")

        self.worker = worker
        worker.chunk.connect(self._on_chunk)
        worker.opened.connect(self._on_source_opened)
        worker.error.connect(lambda msg: self._log(msg, "warn"))
        worker.closed.connect(self._on_source_closed)
        worker.finished.connect(self._on_worker_finished)
        worker.start()

        self._serial_ready = False
        self._cmd_inflight = None
        self._cmd_chain_save = False
        self._cmd_timer.stop()
        self._update_command_buttons()

        self._status_source.setText("连接中…")
        self.act_connect.setEnabled(False)
        self.act_disconnect.setEnabled(True)
        self.act_replay.setEnabled(False)

    def _stop_worker(self) -> None:
        worker = self.worker
        if worker is None:
            return
        self._serial_ready = False
        self._update_command_buttons()
        worker.stop()
        if not worker.wait(3000):
            self._log("工作线程未在 3 秒内退出（继续等待）", "warn")
            worker.wait()
        self.worker = None
        self._status_source.setText("未连接")
        self.act_connect.setEnabled(True)
        self.act_disconnect.setEnabled(False)
        self.act_replay.setEnabled(True)

    def _on_source_opened(self, desc: str) -> None:
        self._status_source.setText(desc)
        self._log(f"数据源已打开：{desc}")
        self._serial_ready = isinstance(self.worker, SerialWorker)
        self._update_command_buttons()

    def _on_source_closed(self) -> None:
        self._status_source.setText("未连接")
        self._serial_ready = False
        self._cmd_inflight = None
        self._cmd_chain_save = False
        self._cmd_timer.stop()
        self._update_command_buttons()

    def _on_worker_finished(self) -> None:
        if self.sender() is self.worker:
            self.worker = None
        self.act_connect.setEnabled(True)
        self.act_disconnect.setEnabled(False)
        self.act_replay.setEnabled(True)
        self._serial_ready = False
        self._update_command_buttons()

    # ================= 数据到达 =================
    def _on_chunk(self, data: bytes) -> None:
        self.store.add_raw(data)
        crc_before = self.parser.stats.crc_errors
        frames, lines = self.parser.feed(data)
        for line in lines:
            self._log(line)
            self._handle_cmd_line(line)
        for frame in frames:
            gap = self.store.add_frame(frame)
            self._log(f"[帧 #{frame.seq}] {frame.summary()}", "frame")
            if gap is not None:
                self._log(gap.message(), "warn")
            self._on_live_frame(frame)
        new_crc_errors = self.parser.stats.crc_errors - crc_before
        if new_crc_errors > 0:
            self._log(
                f"⚠ CRC 校验失败 {new_crc_errors} 次：帧损坏已重同步"
                f"（累计 {self.parser.stats.crc_errors}；坏帧区间内的文本行被丢弃）",
                "warn")
        if frames:
            self._rebuild_frame_combo()
            self._refresh_current()
            loc = self._last_loc
            if loc is not None and loc.success and not loc.out_of_bounds:
                self.top_view.append_history(loc.x, loc.y)
        self._update_status()

    def _rebuild_frame_combo(self) -> None:
        self._loading = True
        try:
            self.frame_combo.clear()
            for frame in reversed(self.store.frames):     # 新 → 旧
                self.frame_combo.addItem(
                    f"#{frame.seq}  trig={frame.trig_id}  {frame.type_name}  "
                    f"corr={frame.corr_q10}", frame.seq)
            self.frame_combo.setCurrentIndex(0)
        finally:
            self._loading = False

    def _current_frame(self):
        seq = self.frame_combo.currentData()
        if seq is None:
            return self.store.latest()
        return self.store.at(int(seq))

    def _on_frame_selected(self, _index: int) -> None:
        self._refresh_current()

    def _refresh_current(self) -> None:
        frame = self._current_frame()
        if frame is None:
            self._last_loc = None
            self.top_view.clear()
            self._reset_analysis_labels()
            return
        self.plot.show_frame(frame, self.settings.p22, self.settings.chan_map)
        names = " ".join(
            f"ch{ch}→{self.settings.chan_map.get(ch, '?')}" for ch in ("1", "2", "3", "4"))
        self.meta_label.setText(
            f"接收 #{frame.seq}｜trig_id {frame.trig_id}｜{frame.type_name}｜"
            f"flags 0x{frame.flags:02x}｜corr {frame.corr_q10}｜pk_idx {frame.pk_idx}｜"
            f"peak_dif {frame.peak_dif}｜peak_ch {list(frame.peak_ch)}｜"
            f"触发样本 @{frame.trig_index(self.settings.p22)}｜{names}")
        self._update_ref_labels(frame)
        self._refresh_analysis()

    def _update_ref_labels(self, frame) -> None:
        """参照替换页的当前帧信息 + 适宜性警告。"""
        self.ref_frame_label.setText(
            f"#{frame.seq} trig={frame.trig_id} n={frame.n} "
            f"{frame.type_name} corr={frame.corr_q10} pk_idx={frame.pk_idx}")
        warns = []
        if frame.type == 2:
            warns.append("该帧曾被相关度拒绝——仅在确认是真实击打时使用")
        if 0 < frame.pk_idx < 5 or frame.pk_idx + 5 >= frame.n:
            warns.append("峰值贴窗边，形状可能不完整，通常不适合作为参照")
        if frame.n != 128:
            warns.append(f"窗长 n={frame.n} ≠ 128：与板子当前 P21+P22 不一致会被拒绝")
        self.ref_warn_label.setText("\n".join(warns))

    # ================= 落点分析 =================
    def _refresh_analysis(self) -> None:
        frame = self._current_frame()
        if frame is None:
            self._last_loc = None
            self.top_view.clear()
            self._reset_analysis_labels()
            return
        self.top_view.set_geometry(self.settings)
        params = analyzer.params_from_settings(self.settings)
        loc = analyzer.localize(frame, self._calib_data, params)
        self._last_loc = loc
        self._fill_feature_table(loc.features)

        if loc.centroid is not None:
            self.centroid_label.setText(
                f"({loc.centroid[0]:.1f}, {loc.centroid[1]:.1f}) mm")
        else:
            self.centroid_label.setText("—")

        if loc.success:
            if loc.out_of_bounds:
                self.ema_label.setText(
                    f"({loc.x:.1f}, {loc.y:.1f}) mm　原始 "
                    f"({loc.x_raw:.0f}, {loc.y_raw:.0f})")
            else:
                self.ema_label.setText(f"({loc.x:.1f}, {loc.y:.1f}) mm")
            self.resid_label.setText(
                f"{loc.resid_min:.2f} / {loc.n_band} 点　置信度 {loc.confidence:.0%}")
            self.force_label.setText(
                f"{loc.force_impulse:.4g} counts·s　RMS {loc.force_rms:.4g}")
            if loc.out_of_bounds:
                self.loc_status.setStyleSheet("color: #ef5350;")
                self.loc_status.setText(loc.note or "越界：已修正到边界交点")
            else:
                self.loc_status.setStyleSheet("color: #66bb6a;")
                self.loc_status.setText("板内")
        else:
            self.ema_label.setText("—")
            self.resid_label.setText("—")
            self.force_label.setText("—")
            self.loc_status.setStyleSheet("color: #78909c;")
            self.loc_status.setText(loc.note or "未定位")
        self.top_view.show_hit(self.settings, loc)

    def _reset_analysis_labels(self) -> None:
        for item in (self.centroid_label, self.ema_label,
                     self.resid_label, self.force_label):
            item.setText("—")
        self.loc_status.setStyleSheet("color: #78909c;")
        self.loc_status.setText("尚未分析")
        for i in range(4):
            for col in (1, 2, 3):
                self.feature_table.item(i, col).setText("—")
        self.sum_label.setText("Σ冲量 —")

    def _fill_feature_table(self, features) -> None:
        if features is None or not features.valid:
            note = features.note if features is not None else "无数据"
            for i in range(4):
                for col in (1, 2, 3):
                    self.feature_table.item(i, col).setText("—")
            self.sum_label.setText(f"Σ冲量 —（{note}）")
            return
        for i in range(4):
            self.feature_table.item(i, 1).setText(f"{features.peak[i]:.0f}")
            self.feature_table.item(i, 2).setText(f"{features.impulse[i]:.4g}")
            self.feature_table.item(i, 3).setText(f"{features.rms[i]:.0f}")
        self.sum_label.setText(
            f"Σ冲量 {features.total_impulse:.4g} counts·s　"
            f"Σ峰值 {features.total_peak:.0f}")

    # ================= 标定工作流 =================
    def _calib_path(self) -> Path:
        if self.settings.calib_path:
            return Path(self.settings.calib_path)
        return Path(__file__).resolve().parent.parent / "calib" / "transfer.npz"

    def _reload_calib(self, quiet: bool = False) -> None:
        path = self._calib_path()
        self._calib_data = None
        if path.exists():
            try:
                self._calib_data = analyzer.load_calib(path)
                if not quiet:
                    self._log(
                        f"已载入标定集：{path}"
                        f"（{self._calib_data.n_valid_points} 点，"
                        f"q̄ {self._calib_data.mean_quality:.2f}）")
            except (OSError, ValueError) as exc:
                self._log(f"标定集载入失败：{path}（{exc}）", "warn")
        elif not quiet:
            self._log(f"未找到标定集：{path}")
        self._update_calib_status_label()
        self._refresh_current()

    def _update_calib_status_label(self) -> None:
        path = self._calib_path()
        self.calib_path_label.setText(path.name)
        self.calib_path_label.setToolTip(str(path))
        acc = self._calib_acc
        if acc is not None:
            done = sum(
                1 for i in range(acc.rows) for j in range(acc.cols)
                if acc.is_point_done(i, j) or acc.is_point_skipped(i, j))
            self.calib_status.setText(
                f"标定中 {done}/{acc.rows * acc.cols} 点"
                f"（跳过 {len(acc.skipped)}）")
        elif self._calib_data is not None:
            self.calib_status.setText(
                f"已标定 {self._calib_data.n_valid_points} 点，"
                f"一致性 q̄ {self._calib_data.mean_quality:.2f}")
        else:
            self.calib_status.setText("未标定（仅显示加权质心对照）")

    def _set_calib_buttons(self, active: bool) -> None:
        for spin in (self._rows_spin, self._cols_spin, self._taps_spin):
            spin.setEnabled(not active)
        self._btn_calib_start.setEnabled(not active)
        self._btn_calib_finish.setEnabled(active)
        for btn in (self._btn_calib_skip, self._btn_calib_retap,
                    self._btn_calib_cancel):
            btn.setEnabled(active)

    def _on_calib_browse(self) -> None:
        start_dir = str(self._calib_path().parent)
        path, _ = QFileDialog.getOpenFileName(
            self, "选择标定集", start_dir, "标定集 (*.npz);;所有文件 (*)")
        if not path:
            return
        self.settings.calib_path = path
        self._save()
        self._reload_calib()

    def _on_calib_start(self) -> None:
        if self._calib_acc is not None:
            return
        n = self.store.frames[-1].n if self.store.frames else 128
        try:
            acc = analyzer.make_calib_accumulator(
                self._rows_spin.value(), self._cols_spin.value(),
                self._taps_spin.value(),
                self.settings.ab_mm, self.settings.ad_mm, n=n)
        except ValueError as exc:
            QMessageBox.warning(self, "无法开始标定", str(exc))
            return
        self._calib_acc = acc
        self.settings.calib_rows = acc.rows
        self.settings.calib_cols = acc.cols
        self.settings.taps_per_point = acc.taps_per_point
        self._save()
        self._set_calib_buttons(True)
        self.calib_progress.clear()
        self._log(
            f"[标定] 开始 {acc.rows}×{acc.cols} 网格，每点 "
            f"{acc.taps_per_point} 次；按俯视图黄色圆环逐点敲击")
        self._update_calib_progress()

    def _on_calib_skip(self) -> None:
        acc = self._calib_acc
        if acc is None:
            return
        pt = acc.next_point()
        if pt is None:
            return
        acc.skip(*pt)
        self._log(f"[标定] 跳过点 r{pt[0]} c{pt[1]}")
        self._update_calib_progress()

    def _on_calib_retap(self) -> None:
        acc = self._calib_acc
        if acc is None:
            return
        pt = acc.next_point()
        if pt is None:
            self._log("[标定] 所有点已完成，无法重敲", "warn")
            return
        acc.retap(*pt)
        self._log(f"[标定] 重敲点 r{pt[0]} c{pt[1]}（已清空）")
        self._update_calib_progress()

    def _on_calib_finish(self) -> None:
        acc = self._calib_acc
        if acc is None:
            return
        if not acc.is_complete():
            QMessageBox.warning(self, "标定未完成",
                                "还有未采集的点：继续敲击，或「跳过此点」。")
            return
        calib = analyzer.calib_finalize(acc)
        if calib.n_valid_points < 4:
            QMessageBox.warning(
                self, "有效点不足",
                f"至少需要 4 个有效点（当前 {calib.n_valid_points}）。")
            return
        path = self._calib_path()
        try:
            analyzer.save_calib(calib, path)
        except OSError as exc:
            QMessageBox.critical(self, "保存标定集失败", str(exc))
            return
        self.settings.calib_path = str(path)
        self._save()
        self._calib_data = calib
        self._calib_acc = None
        self._set_calib_buttons(False)
        self.top_view.clear_highlight()
        self._log(
            f"[标定] 已保存 {path}（{calib.n_valid_points} 点，"
            f"q̄ {calib.mean_quality:.2f}）")
        self._update_calib_status_label()
        self._refresh_analysis()

    def _on_calib_cancel(self) -> None:
        if self._calib_acc is None:
            return
        self._calib_acc = None
        self._set_calib_buttons(False)
        self.top_view.clear_highlight()
        self._update_calib_status_label()
        self._log("[标定] 已取消（未写入数据集）")

    def _on_live_frame(self, frame) -> None:
        """标定模式：把新到达的帧喂给当前待敲点（每帧一次敲击）。"""
        acc = self._calib_acc
        if acc is None:
            return
        pt = acc.next_point()
        if pt is None:
            return
        try:
            cnt = analyzer.calib_add_tap(acc, pt[0], pt[1], frame)
        except ValueError as exc:
            self._log(f"[标定] 本帧不可用：{exc}", "warn")
            return
        self._log(f"[标定] 点 r{pt[0]} c{pt[1]}  {cnt}/{acc.taps_per_point}")
        self._update_calib_progress()

    def _update_calib_progress(self) -> None:
        acc = self._calib_acc
        if acc is None:
            self.top_view.clear_highlight()
            self._update_calib_status_label()
            return
        lines = []
        for i in range(acc.rows):
            for j in range(acc.cols):
                x, y = acc.point_xy(i, j)
                cnt = acc.count(i, j)
                if acc.is_point_skipped(i, j):
                    state = "跳过"
                elif cnt >= acc.taps_per_point:
                    state = f"{cnt}/{acc.taps_per_point} q={acc.point_quality(i, j):.2f}"
                else:
                    state = f"{cnt}/{acc.taps_per_point}"
                lines.append(f"r{i} c{j}  ({x:5.0f},{y:5.0f}) mm  {state}")
        self.calib_progress.setPlainText("\n".join(lines))
        complete = acc.is_complete()
        self._btn_calib_skip.setEnabled(not complete)
        self._btn_calib_retap.setEnabled(not complete)
        pt = acc.next_point()
        if pt is None:
            self.top_view.clear_highlight()
        else:
            x, y = acc.point_xy(*pt)
            self.top_view.highlight_tap_point(
                (x, y),
                f"标定点 r{pt[0]} c{pt[1]}  {acc.count(*pt)}/{acc.taps_per_point}")
        self._update_calib_status_label()

    def _update_status(self) -> None:
        s = self.parser.stats
        self._status_stats.setText(
            f"字节 {s.bytes_in}　帧 {s.frames_total}　CRC 错 {s.crc_errors}　"
            f"重同步 {s.resyncs}　文本行 {s.text_lines}")

    # ================= 导出 / 日志 =================
    def _on_export_raw(self) -> None:
        if not self.store.raw:
            QMessageBox.information(self, "无数据", "当前会话还没有收到任何数据。")
            return
        default = time.strftime("capture_%Y%m%d_%H%M%S.bin")
        start_dir = self.settings.last_open_dir or str(Path.cwd())
        path, _ = QFileDialog.getSaveFileName(
            self, "导出原始流", str(Path(start_dir) / default), "二进制 (*.bin);;所有文件 (*)")
        if not path:
            return
        try:
            Path(path).write_bytes(bytes(self.store.raw))
        except OSError as exc:
            QMessageBox.critical(self, "导出失败", str(exc))
            return
        self.settings.last_open_dir = str(Path(path).parent)
        self._save()
        note = "（超过 64MB，仅保留最近部分）" if self.store.raw_truncated else ""
        self._log(f"已导出原始流：{path}，{len(self.store.raw)} 字节{note}")

    def _log(self, text: str, kind: str = "info") -> None:
        colors = {"warn": "#c62828", "frame": "#1565c0"}
        color = colors.get(kind)
        if color:
            self.log.appendHtml(
                f'<span style="color:{color}">{html.escape(text)}</span>')
        else:
            self.log.appendPlainText(text)

    # ================= 生命周期 =================
    def closeEvent(self, event) -> None:
        self._stop_worker()
        self._save()
        super().closeEvent(event)
