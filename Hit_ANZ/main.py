# -*- coding: utf-8 -*-
"""Hit_ANZ 装甲板波形上位机入口。用法：py -3 main.py"""
from __future__ import annotations

import sys

from PySide6.QtWidgets import QApplication

from app.main_window import MainWindow


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName("Hit_ANZ")
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
