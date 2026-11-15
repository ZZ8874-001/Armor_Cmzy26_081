# -*- coding: utf-8 -*-
"""把 Hit_ANZ 打包为 Windows EXE（PyInstaller，默认 onedir）。

用法（在 Hit_ANZ/ 目录下）：
    py -3 build_exe.py              # 默认：onedir + 无控制台窗口
    py -3 build_exe.py --console    # 调试用：保留控制台看崩溃信息
    py -3 build_exe.py --onefile    # 单文件（启动慢、更易被杀软误报，不推荐分发）

产物：
    Hit_ANZ/dist/Hit_ANZ/Hit_ANZ.exe     ← 整个 dist/Hit_ANZ 文件夹拷走即可运行
    Hit_ANZ/pyi_build/                   ← PyInstaller 的 work/spec（可随时删除）

数据目录：打包版首次运行在 EXE 旁创建 config.json 与 calib/（app/settings.data_dir，
不可写时回退 %LOCALAPPDATA%\\Hit_ANZ）。

安全约定（重要）：
  * 只写 dist/ 与 pyi_build/ 两个目录；**绝不使用仓库根的 build/**——
    那里是固件（arm-none-eabi）的构建产物，PyInstaller 默认会在当前目录建 build/
    并覆盖同名内容，所以这里全部路径显式指定。
  * 不读取、不复制、不修改 config.json / calib/ / test/ 等本地数据：
    打包只消费源码，本地日志/标定/样例抓包保持原样。
  * --noconfirm 仅替换 dist/Hit_ANZ（由本脚本生成），不删除其它任何目录。
"""
from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = Path(__file__).resolve().parent
DIST = ROOT / "dist"
WORK = ROOT / "pyi_build"          # 名字刻意避开仓库根 build/（固件产物）
ENTRY = ROOT / "main.py"
APP_NAME = "Hit_ANZ"

# 确认未使用、可安全排除的大模块（运行时只用 PySide6.QtCore/QtGui/QtWidgets）。
# **不可排除 PySide6.QtOpenGL / QtOpenGLWidgets**：pyqtgraph/Qt/OpenGLHelpers.py
# 对二者做无条件 importlib 导入（PlotCurveItem 会加载它），排除后整个 pyqtgraph
# 在启动时即 ModuleNotFoundError（2026-11-15 实测）。QtSvg/QtTest 由
# pyqtgraph/Qt/__init__.py 以 try/except 可选导入，排除安全（QtSvg 保留以兼容导出器）。
EXCLUDES = [
    "scipy", "matplotlib", "tkinter", "IPython", "pytest",
    "PySide6.QtQml", "PySide6.QtQuick", "PySide6.QtQuickWidgets",
    "PySide6.QtQuickControls2", "PySide6.QtNetwork", "PySide6.QtWebEngineWidgets",
    "PySide6.QtWebEngineCore", "PySide6.QtWebEngineQuick", "PySide6.QtWebChannel",
    "PySide6.QtWebSockets", "PySide6.QtMultimedia", "PySide6.QtMultimediaWidgets",
    "PySide6.QtPdf", "PySide6.QtPdfWidgets", "PySide6.QtSql", "PySide6.QtDBus",
    "PySide6.QtDesigner", "PySide6.QtUiTools", "PySide6.QtAxContainer",
    "PySide6.QtBluetooth", "PySide6.QtCharts", "PySide6.QtGraphs",
    "PySide6.QtLocation", "PySide6.QtNfc", "PySide6.QtPositioning",
    "PySide6.QtRemoteObjects", "PySide6.QtScxml", "PySide6.QtSensors",
    "PySide6.QtSerialBus", "PySide6.QtSpatialAudio", "PySide6.QtStateMachine",
    "PySide6.QtTextToSpeech", "PySide6.QtWebView", "PySide6.QtHelp",
    "PySide6.QtConcurrent", "PySide6.QtXml", "PySide6.QtPrintSupport",
    "PySide6.QtTest",
]

# 构建后裁剪（安全项：Qt 语言包与软件 OpenGL 回退 DLL，本程序均不使用）：
#   本程序的界面文本全部自绘，不加载 Qt 翻译；pyqtgraph 2D 用 QPainter 光栅渲染，
#   不依赖软件 OpenGL。裁剪后仍需通过启动冒烟（见 README 的打包验证）。
SLIM_PATHS = ["PySide6/translations", "PySide6/opengl32sw.dll"]


def main() -> int:
    console = "--console" in sys.argv
    onefile = "--onefile" in sys.argv

    try:
        import PyInstaller  # noqa: F401
    except ImportError:
        print("缺少 PyInstaller：py -3 -m pip install -i "
              "https://pypi.tuna.tsinghua.edu.cn/simple pyinstaller")
        return 2
    if not ENTRY.exists():
        print(f"找不到入口：{ENTRY}")
        return 2

    # 防御：产物目录必须在本目录内（绝不写仓库根 build/ 或其它位置）
    for p in (DIST, WORK):
        assert p.parent == ROOT, f"产物路径越界：{p}"

    cmd = [
        sys.executable, "-m", "PyInstaller",
        "--noconfirm",
        "--clean",                       # 只清 PyInstaller 缓存，不动任何项目文件
        "--onefile" if onefile else "--onedir",
        "--console" if console else "--windowed",
        "--name", APP_NAME,
        "--distpath", str(DIST),
        "--workpath", str(WORK),
        "--specpath", str(WORK),
        "--paths", str(ROOT),
        # pyqtgraph 的 colormap/资源文件按包收集（viridis 等数据文件不是 .py）
        "--collect-all", "pyqtgraph",
    ]
    for mod in EXCLUDES:
        cmd += ["--exclude-module", mod]
    cmd.append(str(ENTRY))

    print("== PyInstaller ==")
    print(" ".join(cmd))
    rc = subprocess.call(cmd, cwd=str(ROOT))
    if rc != 0:
        print(f"打包失败（exit {rc}）")
        return rc

    if onefile:
        exe = DIST / f"{APP_NAME}.exe"
    else:
        exe = DIST / APP_NAME / f"{APP_NAME}.exe"
    if not exe.exists():
        print(f"未找到产物：{exe}")
        return 1

    # 安全裁剪：只删 dist 内的 Qt 语言包/软件 OpenGL 回退（本程序不使用）
    if not onefile:
        internal = exe.parent / "_internal"
        for rel in SLIM_PATHS:
            target = internal / rel
            if target.is_dir():
                shutil.rmtree(target)
                print(f"裁剪目录：{rel}")
            elif target.exists():
                target.unlink()
                print(f"裁剪文件：{rel}")

    # 防止误运行：删掉 pyi_build 里的中间产物 EXE。它旁边没有 _internal，
    # 双击必报「Failed to load Python DLL ...\\pyi_build\\...」；分发只认 dist。
    intermediate = WORK / APP_NAME / f"{APP_NAME}.exe"
    if intermediate.exists():
        intermediate.unlink()
        print(f"已删除中间产物：{intermediate}（避免误双击）")

    size_mb = sum(f.stat().st_size for f in
                  (exe.parent.rglob("*") if not onefile else [exe])
                  if f.is_file()) / (1024 * 1024)
    print("\n== 完成 ==")
    print(f"运行   : 双击 {exe}")
    print("         （pyi_build 里的同名 EXE 是中间产物、无法运行，已自动删除）")
    print(f"体积   : {size_mb:.1f} MB（整个文件夹一起分发）")
    print("数据   : 首次运行在 EXE 旁生成 config.json（不可写时回退 %LOCALAPPDATA%\\Hit_ANZ）")
    print("注意   : 本脚本只写 dist/ 与 pyi_build/，未触碰仓库 build/、config.json、calib/、test/")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
