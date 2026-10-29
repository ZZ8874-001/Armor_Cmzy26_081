# -*- coding: utf-8 -*-
"""Hit_ANZ 上位机应用包。

分层（依赖只向下的方向）：
  sources.py   通信层：串口/回放线程（QThread，唯一做阻塞 I/O 的地方）
  protocol.py  数据层：WAV1 帧流式解析（零 Qt，可 CLI 单测）
  store.py     数据层：帧存储 + trig_id 跳号 + 原始流缓冲
  settings.py  设置层：config.json 持久化
  analyzer.py  分析层：峰值/冲量/RMS 特征 + EMA 传递矩阵落点（零 Qt，可 CLI 单测）
  plot_widget.py / top_view.py / main_window.py  展示层：pyqtgraph + PySide6
"""
