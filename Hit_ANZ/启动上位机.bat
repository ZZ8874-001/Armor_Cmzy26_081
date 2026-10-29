@echo off
setlocal
chcp 65001 >nul
title Hit_ANZ - Armor Waveform Viewer

rem ---- Switch to this script's folder ---------------------------------------
rem pushd (NOT cd /d) is required: cd /d fails on UNC paths such as
rem \\server\share\...\Hit_ANZ, while pushd temporarily maps a drive letter.
pushd "%~dp0"
if errorlevel 1 (
    echo.
    echo [ERROR] Cannot enter script directory: %~dp0
    echo.
    pause
    exit /b 1
)

rem ---- Python 3 launcher present? -------------------------------------------
py -3 -V >nul 2>&1
if errorlevel 1 (
    echo.
    echo [ERROR] Python launcher "py -3" not found.
    echo         Install Python 3.13 x86-64 from python.org, then retry.
    echo.
    pause
    exit /b 1
)

rem ---- Dependencies present? -------------------------------------------------
py -3 -c "import PySide6,pyqtgraph,serial,numpy,scipy" >nul 2>&1
if errorlevel 1 (
    echo.
    echo [ERROR] Python packages missing. Install them first, then run again:
    echo.
    echo   Online, Tsinghua mirror:
    echo     py -3 -m pip install -i https://pypi.tuna.tsinghua.edu.cn/simple PySide6-Essentials pyqtgraph pyserial numpy scipy
    echo.
    echo   Offline, wheels folder copied to e.g. D:\wheels:
    echo     py -3 -m pip install --no-index --find-links D:\wheels PySide6-Essentials pyqtgraph pyserial numpy scipy
    echo.
    pause
    exit /b 1
)

rem ---- Run ------------------------------------------------------------------
py -3 main.py
if errorlevel 1 (
    echo.
    echo [ERROR] Hit_ANZ exited abnormally. See the message above.
    pause
)
popd
endlocal
