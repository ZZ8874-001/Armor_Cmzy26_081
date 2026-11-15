# -*- coding: utf-8 -*-
"""v1.0.0 发布构建：三硬件变体固件 + 上位机 EXE + 发布清单。

用法（仓库根目录）：
    py -3 tools/release_build.py                 # 固件 + 上位机 + release 目录（不提交）
    py -3 tools/release_build.py --skip-host     # 只编固件
    py -3 tools/release_build.py --skip-git      # 兼容参数（当前默认不自动提交）
    py -3 tools/release_build.py --commit        # 生成后自动 git add/commit/tag（白名单路径）
    py -3 tools/release_build.py --push          # 在 --commit 基础上推送 main 与 tag

变体矩阵（由 app.mk 的 ARMOR_VARIANT 注入编译宏）：
    082_500g  082 板 + 500g 传感器  PGA ×1  HSI16 /2
    083_500g  083 板 + 500g 传感器  PGA ×1  HSE 8MHz
    083_2kg   083 板 + 2kg 传感器   PGA ×4  HSE 8MHz（默认变体）

安全约束：
  * 每个变体使用独立的 build/<variant>（对象不跟踪 -D 变化），只删除该子目录，
    绝不触碰仓库根 build/ 下的其它固件产物。
  * 发布目录 release/<VERSION>/ 由本脚本重新生成；不改动 Hit_ANZ/config.json、
    calib/ 源文件与 test/ 样例。
  * git 提交只 add 白名单路径，绝不 git add -A（避免卷入两个嵌套仓库）。
"""
from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import time
import sys
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = Path(__file__).resolve().parent.parent
TARGET = "Armor_Cmzy26_081"
VERSION = "v1.0.0"
MAX_BIN = 0x1E800                 # 镜像必须低于参数存储槽 B（0x0801E800）

VARIANTS = [
    # (变体名, 板, 传感器, PGA, 时钟说明)
    ("082_500g", "082", "500g", "×1", "HSI16 /2 → 80MHz（PLLSAI1 同源 M=2 → ADC 32MHz）"),
    ("083_500g", "083", "500g", "×1", "HSE 8MHz → 80MHz（PLLSAI1 → ADC 32MHz）"),
    ("083_2kg", "083", "2kg", "×4", "HSE 8MHz → 80MHz（PLLSAI1 → ADC 32MHz）"),
]

INCLUDES = [
    "-ICore/Inc",
    "-IDrivers/STM32L4xx_HAL_Driver/Inc",
    "-IDrivers/STM32L4xx_HAL_Driver/Inc/Legacy",
    "-IDrivers/CMSIS/Device/ST/STM32L4xx/Include",
    "-IDrivers/CMSIS/Include",
    "-IApp", "-IApp/bsp", "-IApp/detect", "-IApp/comm", "-IApp/app",
]

# git 白名单（相对仓库根）；不含 release/dist/build 之外任何未跟踪文件
GIT_PATHS = [
    "app.mk",
    "App/board_variant.h",
    "App/app/version.h",
    "App/app/main_app.c",
    "App/bsp/ads131m04.c",
    "App/detect/calibration.c",
    "Core/Src/main.c",
    "Core/Src/stm32l4xx_hal_msp.c",
    "compile_commands.json",
    "tools/release_build.py",
    "Hit_ANZ/app/version.py",
    "Hit_ANZ/app/main_window.py",
    "Hit_ANZ/README.md",
    "CLAUDE.md",
    "App/README.md",
    "Docs/装甲模块固件设计方案.md",
    "Docs/ADS131M04_初始化SPI时序.md",
    "Docs/开机标定与状态手册.md",
]


def rmtree_retry(path: Path, tries: int = 8, delay: float = 0.5) -> None:
    """Windows 上刚写完的大目录偶尔被 Defender/索引器短暂占用，重试删除。"""
    for _ in range(tries - 1):
        try:
            shutil.rmtree(path)
            return
        except PermissionError:
            time.sleep(delay)
    shutil.rmtree(path)


def run(cmd, cwd=None, **kw):
    print("$ " + " ".join(str(c) for c in cmd))
    kw.setdefault("text", True)
    kw.setdefault("encoding", "utf-8")
    kw.setdefault("errors", "replace")
    return subprocess.run(cmd, cwd=str(cwd or ROOT), **kw)


def read_defines(hw: str, sensor: str) -> dict:
    """用预处理器导出 board_variant.h 的派生常量。"""
    gcc = shutil.which("arm-none-eabi-gcc")
    if gcc is None:
        raise SystemExit("找不到 arm-none-eabi-gcc（工具链不在 PATH？）")
    cmd = [gcc, "-dM", "-E", "-DUSE_HAL_DRIVER", "-DSTM32L432xx",
           f"-DARMOR_HW_{hw}", f"-DARMOR_SENSOR_{sensor}",
           *INCLUDES, "-include", "board_variant.h", "-x", "c", "-"]
    r = subprocess.run(cmd, cwd=str(ROOT), input=b"", capture_output=True,
                       text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise SystemExit(f"预处理器自检失败（{hw}/{sensor}）：\n{r.stderr}")
    out = {}
    for line in r.stdout.splitlines():
        if line.startswith("#define "):
            parts = line[8:].split(None, 1)
            if len(parts) == 2:
                out[parts[0]] = parts[1].strip()
    return out


def verify_matrix() -> None:
    expect = {
        "082_500g": ("082", "500G", "0x0000u", "1u", '"HSI16/2"'),
        "083_500g": ("083", "500G", "0x0000u", "1u", '"HSE8"'),
        "083_2kg": ("083", "2KG", "0x2222u", "4u", '"HSE8"'),
    }
    for name, (hw, sensor, gain1, pga, clk) in expect.items():
        d = read_defines(hw, sensor)
        checks = {
            "ADS_GAIN1_VALUE": gain1,
            "ADS_PGA_GAIN": pga,
            "ARMOR_CLK_STR": clk,
            "ARMOR_HW_STR": f'"{hw}"',
            "ARMOR_SENSOR_STR": '"' + ("2kg" if sensor == "2KG" else "500g") + '"',
        }
        for k, v in checks.items():
            if d.get(k) != v:
                raise SystemExit(f"{name}: {k}={d.get(k)!r}，应为 {v!r}")
        print(f"  preproc OK {name}: gain1={d['ADS_GAIN1_VALUE']} pga={d['ADS_PGA_GAIN']} clk={d['ARMOR_CLK_STR']}")

    # 非法组合必须被 #error 拦截
    gcc = shutil.which("arm-none-eabi-gcc")
    r = subprocess.run(
        [gcc, "-c", "-DUSE_HAL_DRIVER", "-DSTM32L432xx",
         "-DARMOR_HW_082", "-DARMOR_SENSOR_2KG", *INCLUDES,
         "-include", "board_variant.h", "-x", "c", "-", "-o", os.devnull],
        cwd=str(ROOT), input=b"", capture_output=True,
        text=True, encoding="utf-8", errors="replace")
    if r.returncode == 0 or "082 硬件仅适配 500g" not in r.stderr:
        raise SystemExit("非法组合 082_2kg 未被拦截（board_variant.h 校验失效）")
    print("  preproc OK：非法组合 082_2kg 被 #error 拦截")


def build_firmware(release_dir: Path) -> list[dict]:
    if shutil.which("make") is None:
        raise SystemExit("找不到 make")
    (ROOT / "build").mkdir(exist_ok=True)
    rows = []
    for name, hw, sensor, pga, clk in VARIANTS:
        build_dir = (ROOT / "build" / name).resolve()
        # 白名单守卫：只允许删除 build/<允许的变体名>
        if build_dir.parent != (ROOT / "build").resolve() or name not in {v[0] for v in VARIANTS}:
            raise SystemExit(f"拒绝清理非法路径：{build_dir}")
        if build_dir.exists():
            rmtree_retry(build_dir)

        r = run(["make", "-j", f"BUILD_DIR=build/{name}", f"ARMOR_VARIANT={name}"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode != 0:
            print(r.stdout[-4000:])
            raise SystemExit(f"变体 {name} 编译失败")
        warn = [ln for ln in r.stdout.splitlines() if "warning:" in ln]
        if warn:
            print("\n".join(warn[:10]))
            raise SystemExit(f"变体 {name} 有编译警告，发布中止")

        art = {}
        for ext in ("elf", "hex", "bin", "map"):
            src = build_dir / f"{TARGET}.{ext}"
            if not src.exists():
                raise SystemExit(f"缺少产物 {src}")
            dst_dir = release_dir / "firmware" / name
            dst_dir.mkdir(parents=True, exist_ok=True)
            dst = dst_dir / f"{TARGET}_{name}_{VERSION}.{ext}"
            shutil.copy2(src, dst)
            art[ext] = dst

        bin_size = art["bin"].stat().st_size
        if bin_size >= MAX_BIN:
            raise SystemExit(f"{name}: 镜像 {bin_size} B 超过 0x{MAX_BIN:X}（会压到参数存储页）")

        sz = subprocess.run(["arm-none-eabi-size", str(art["elf"])],
                            capture_output=True, text=True,
                            encoding="utf-8", errors="replace")
        size_line = sz.stdout.strip().splitlines()[-1] if sz.returncode == 0 else "n/a"
        rows.append(dict(name=name, hw=hw, sensor=sensor, pga=pga, clk=clk,
                         bin=bin_size, size=size_line))
        print(f"  {name}: bin={bin_size} B  ({size_line})")
    return rows


def build_host(release_dir: Path) -> Path | None:
    host_root = ROOT / "Hit_ANZ"
    r = run([sys.executable, "build_exe.py"], cwd=host_root,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        print(r.stdout[-4000:])
        raise SystemExit("上位机打包失败")
    src = host_root / "dist" / "Hit_ANZ"
    dst = release_dir / "host" / "Hit_ANZ"
    if not src.exists():
        raise SystemExit(f"缺少上位机产物目录：{src}")
    if dst.exists():
        rmtree_retry(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(src, dst)

    # 绝不带开发机配置
    cfg = dst / "config.json"
    if cfg.exists():
        cfg.unlink()

    # 按用户要求随包附当前标定集（来自当前板；换板需重标）
    calib_src = host_root / "calib" / "transfer.npz"
    if calib_src.exists():
        (dst / "calib").mkdir(exist_ok=True)
        shutil.copy2(calib_src, dst / "calib" / "transfer.npz")
    print(f"  上位机：{dst}（已附 calib/transfer.npz）")
    return dst


def write_readme(release_dir: Path, rows: list[dict]) -> None:
    lines = [
        f"# Armor_Cmzy26_081 固件 + Hit_ANZ 上位机 {VERSION}",
        "",
        "本发布包含 **3 个硬件/传感器变体** 的固件（elf/hex/bin/map）与 Windows 上位机 EXE。",
        "变体由编译期宏选择（`ARMOR_VARIANT`），**烧错变体会表现为增益错误（信号差 4 倍）或 082 无法启动**。",
        "",
        "## 变体矩阵",
        "",
        "| 变体 | 板 | 传感器 | PGA 增益 | GAIN1 | 时钟 |",
        "|---|---|---|---|---|---|",
    ]
    for r in rows:
        gain1 = "0x0000" if r["pga"] == "×1" else "0x2222"
        lines.append(f"| `{r['name']}` | {r['hw']} | {r['sensor']} | {r['pga']} | `{gain1}` | {r['clk']} |")
    lines += [
        "",
        "固件文件位于 `firmware/<变体>/Armor_Cmzy26_081_<变体>_" + VERSION + ".{elf,hex,bin,map}`。",
        "",
        "## 烧录",
        "",
        "```bash",
        "# 以 083_2kg 为例（把路径换成你的变体）：",
        "openocd -f interface/stlink.cfg -f target/stm32l4x.cfg \\",
        "        -c \"program release/" + VERSION + "/firmware/083_2kg/Armor_Cmzy26_081_083_2kg_" + VERSION + ".elf verify reset exit\"",
        "```",
        "",
        "## 烧写后判别（唯一可靠手段）",
        "",
        "CAN/状态帧不携带版本信息，请在上电日志（USART1 @921600 8N1）核对：",
        "",
        "```",
        "[APP] Armor " + VERSION + " (<变体>, gain1=<0x0000|0x2222>, <HSI16/2|HSE8>)",
        "[ADS] WREG 0x04=<0x0000|0x2222> OK",
        "```",
        "",
        "- 500g 变体：`gain1=0x0000`；2kg 变体：`gain1=0x2222`；",
        "- 082 变体：`HSI16/2`；083 变体：`HSE8`；",
        "- 082 若启动即停（无日志），通常是 CubeMX 重生成把 `Core/Src/main.c` 与",
        "  `Core/Src/stm32l4xx_hal_msp.c` 的 `ARMOR_HW_082` 守卫冲掉（HSE 配置 +",
        "  HSI 硬件 → PLLSAI1 不匹配 → `Error_Handler`）。重新生成后需恢复守卫。",
        "",
        "## 上位机（Windows，无需 Python）",
        "",
        "`host/Hit_ANZ/` 整个文件夹拷到目标机，双击 `Hit_ANZ.exe`。",
        "",
        "- **不要单独拷 EXE**：`_internal/` 是它的一部分；",
        "- **不要运行 `pyi_build` 里的同名 EXE**（那是中间产物，会报 Failed to load Python DLL）；",
        "- 首次运行在 EXE 旁生成 `config.json`（目录不可写时回退 `%LOCALAPPDATA%\\Hit_ANZ`）；",
        "- 串口 921600 8N1；在线调参与参照替换见 `Hit_ANZ/README.md`。",
        "",
        "## 标定集说明",
        "",
        "`host/Hit_ANZ/calib/transfer.npz` 是**当前测试板**的 EMA 定位标定集，与板面几何/",
        "传感器安装相关。换一块板或改安装后需重新标定（上位机「落点分析 → 定位标定」）。",
        "",
        "## 工程构建（复现）",
        "",
        "```bash",
        "make -j BUILD_DIR=build/083_2kg ARMOR_VARIANT=083_2kg   # 默认变体可省略 ARMOR_VARIANT",
        "py -3 tools/release_build.py                            # 重新生成本发布目录",
        "```",
        "",
        "## 文件校验",
        "",
        "```bash",
        "sha256sum -c SHA256SUMS",
        "```",
    ]
    with open(release_dir / "README.md", "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")


def write_sha256(release_dir: Path) -> None:
    """只校验发布关键产物：firmware/ 下 elf/hex/bin + 上位机 EXE。
    注意只扫 firmware/ 子树——PyInstaller 的 pyqtgraph 数据里有 .hex 色图文件，
    用全目录后缀匹配会误收。"""
    targets = [p for p in sorted((release_dir / "firmware").rglob("*"))
               if p.is_file() and p.suffix in (".elf", ".hex", ".bin")]
    exe = release_dir / "host" / "Hit_ANZ" / "Hit_ANZ.exe"
    if exe.exists():
        targets.append(exe)
    entries = []
    for p in targets:
        h = hashlib.sha256(p.read_bytes()).hexdigest()
        entries.append(f"{h}  {p.relative_to(release_dir).as_posix()}")
    # newline="\n"：Windows 的 write_text 会写 CRLF，导致 sha256sum -c 把 \r 当文件名
    with open(release_dir / "SHA256SUMS", "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(entries) + "\n")
    print(f"  SHA256SUMS：{len(entries)} 个文件")


def git_commit_and_tag(release_dir: Path, push: bool) -> None:
    rel = release_dir.relative_to(ROOT).as_posix()
    paths = list(GIT_PATHS) + [rel]
    r = run(["git", "add", "--", *paths])
    if r.returncode != 0:
        raise SystemExit("git add 失败（见上）")
    r = run(["git", "status", "--short"])
    msg = (f"release: {VERSION} (082/083, 500g/2kg)\n\n"
           "变体构建：ARMOR_VARIANT=082_500g / 083_500g / 083_2kg\n"
           "- 082：HSI16/2，PGA ×1；083：HSE 8MHz，500g ×1 / 2kg ×4\n"
           "- 启动横幅 [APP] Armor v1.0.0 (<变体>, gain1=..., <时钟>)\n"
           f"- 发布目录 {rel}（固件 3 变体 + Hit_ANZ EXE + 标定集 + SHA256SUMS）")
    r = run(["git", "commit", "-m", msg])
    if r.returncode != 0:
        raise SystemExit("git commit 失败（可能无变更或钩子拒绝）")
    if subprocess.run(["git", "rev-parse", "-q", "--verify", f"refs/tags/{VERSION}"],
                      cwd=str(ROOT), capture_output=True).returncode == 0:
        print(f"  tag {VERSION} 已存在，跳过创建")
    else:
        run(["git", "tag", "-a", VERSION, "-m", f"Armor {VERSION}: 082/083 × 500g/2kg + Hit_ANZ"])
    if push:
        run(["git", "push", "origin", "HEAD", "--tags"])
    else:
        print("  未推送。确认无误后执行：git push origin HEAD --tags")


def main() -> int:
    skip_host = "--skip-host" in sys.argv
    commit = "--commit" in sys.argv
    push = "--push" in sys.argv

    release_dir = (ROOT / "release" / VERSION).resolve()
    # 白名单守卫：只允许重建 release/<VERSION>，不碰其它发布目录
    if release_dir.parent != (ROOT / "release").resolve():
        raise SystemExit(f"拒绝清理非法发布路径：{release_dir}")
    print(f"== {VERSION} 发布构建 ==")
    print("-- 预处理器自检 --")
    verify_matrix()

    print("-- 固件三变体 --")
    if release_dir.exists():
        rmtree_retry(release_dir)
    (release_dir / "firmware").mkdir(parents=True, exist_ok=True)
    rows = build_firmware(release_dir)

    if not skip_host:
        print("-- 上位机打包 --")
        build_host(release_dir)

    print("-- 发布清单 --")
    write_readme(release_dir, rows)
    write_sha256(release_dir)
    print(f"完成：{release_dir}")

    if commit:
        print("-- git 提交与 tag --")
        git_commit_and_tag(release_dir, push)
    else:
        print("未提交。确认后运行：py -3 tools/release_build.py --commit [--push]")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
