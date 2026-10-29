# -*- coding: utf-8 -*-
"""通信层：串口读取线程 / 文件回放线程（同一信号协议）。

原则：阻塞 I/O 只发生在这里的工作线程，UI 线程绝不直接碰串口；数据经
``chunk(bytes)`` 信号排队回主线程解析。pyserial 的阻塞 read() 会释放 GIL，
无需额外 sleep 让出 CPU（timeout 已保证循环可退出）。
"""
from __future__ import annotations

import time
from pathlib import Path

import serial
from serial.tools import list_ports
from PySide6.QtCore import QThread, Signal


def available_ports() -> list[tuple[str, str]]:
    """返回 [(device, description), ...]，按设备名排序。"""
    ports = sorted(list_ports.comports(), key=lambda p: p.device)
    return [(p.device, p.description or "") for p in ports]


class SourceWorker(QThread):
    """数据源线程公共协议。"""

    chunk = Signal(bytes)        # 原始字节块
    opened = Signal(str)         # 描述（连接成功后发一次）
    closed = Signal()            # 线程结束前发一次
    error = Signal(str)          # 错误消息

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self._stop = False

    def stop(self) -> None:
        self._stop = True


class SerialWorker(SourceWorker):
    """pyserial 阻塞读循环；timeout=0.1 保证 stop() 后 ≤100ms 退出。"""

    def __init__(self, port: str, baudrate: int, parent=None) -> None:
        super().__init__(parent)
        self.port = port
        self.baudrate = int(baudrate)
        self._ser: serial.Serial | None = None

    def stop(self) -> None:
        super().stop()
        ser = self._ser
        if ser is not None:
            try:
                ser.cancel_read()      # 唤醒阻塞中的 read
            except Exception:
                pass

    def run(self) -> None:
        try:
            self._ser = serial.Serial(self.port, self.baudrate, timeout=0.1)
        except Exception as exc:                       # 端口占用/不存在/权限
            self._ser = None
            self.error.emit(f"打开串口失败：{self.port} — {exc}")
            self.closed.emit()
            return

        self.opened.emit(f"{self.port} @ {self.baudrate}")
        try:
            while not self._stop:
                try:
                    n = self._ser.in_waiting
                    data = self._ser.read(n if n > 0 else 1)
                except Exception as exc:               # 拔线 / 设备复位
                    if not self._stop:
                        self.error.emit(f"串口读取中断：{exc}")
                    break
                if data:
                    self.chunk.emit(data)
        finally:
            try:
                self._ser.close()
            except Exception:
                pass
            self._ser = None
            self.closed.emit()


class FileReplayWorker(SourceWorker):
    """按 921600 8N1 的等效速率回放原始抓包 .bin（无硬件验证路径）。"""

    CHUNK = 1024                                   # 字节
    # 1024 B × 10 bit / 921600 Bd ≈ 11.1 ms
    _DELAY_MS = max(1, round(CHUNK * 10 * 1000 / 921600))

    def __init__(self, path: str | Path, parent=None) -> None:
        super().__init__(parent)
        self.path = Path(path)

    def run(self) -> None:
        try:
            with self.path.open("rb") as fh:
                self.opened.emit(f"回放 {self.path.name}（{self.path.stat().st_size} B）")
                while not self._stop:
                    data = fh.read(self.CHUNK)
                    if not data:
                        break
                    self.chunk.emit(data)
                    time.sleep(self._DELAY_MS / 1000.0)
        except OSError as exc:
            self.error.emit(f"读取回放文件失败：{exc}")
        finally:
            self.closed.emit()
