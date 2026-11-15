# -*- coding: utf-8 -*-
"""数据目录自测（零 Qt、无硬件）：

    py -3 tools/selftest_paths.py

断言源码模式的数据目录、HIT_ANZ_CONFIG 覆盖优先、frozen 模拟（EXE 同目录）
与嵌套目录自动创建；用于打包前确认 config/标定集不会落进临时解压目录。
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

from app import settings  # noqa: E402
from app.settings import Settings, config_path, data_dir  # noqa: E402


def main() -> None:
    # ---- 1. 源码模式：数据目录 = Hit_ANZ/ ----
    saved_env = os.environ.pop("HIT_ANZ_CONFIG", None)
    saved_frozen = getattr(sys, "frozen", None)
    try:
        settings._reset_data_dir()
        assert data_dir() == ROOT, f"源码模式 data_dir={data_dir()} ≠ {ROOT}"
        assert config_path() == ROOT / "config.json", config_path()
        print("  source OK（data_dir = Hit_ANZ/，config.json 同名）")

        # ---- 2. HIT_ANZ_CONFIG 覆盖优先（smoke 的隔离依赖它） ----
        tmp_cfg = Path(tempfile.gettempdir()) / "hit_anz_paths_cfg.json"
        os.environ["HIT_ANZ_CONFIG"] = str(tmp_cfg)
        assert config_path() == tmp_cfg, config_path()
        print("  env OK（HIT_ANZ_CONFIG 覆盖优先）")
        os.environ.pop("HIT_ANZ_CONFIG", None)

        # ---- 3. frozen 模拟：数据目录 = EXE 同目录 ----
        with tempfile.TemporaryDirectory() as td:
            exe_dir = Path(td) / "dist"
            exe_dir.mkdir()
            sys.frozen = True
            sys.executable = str(exe_dir / "Hit_ANZ.exe")
            settings._reset_data_dir()
            assert data_dir() == exe_dir.resolve(), data_dir()
            assert config_path() == exe_dir.resolve() / "config.json", config_path()
            Settings().save()
            after = config_path()
            assert after.exists(), "frozen 模式 config 未落盘"
            data = json.loads(after.read_text(encoding="utf-8"))
            assert "baudrate" in data, data
            # 探测文件不残留
            assert not list(exe_dir.glob(".hit_anz_write_probe")), "写探测文件残留"
            print("  frozen OK（data_dir = EXE 同目录，config 落盘，无探测残留）")

            # ---- 4. 嵌套路径自动建父目录 ----
            nested = Path(td) / "a" / "b" / "c.json"
            Settings().save(nested)
            assert nested.exists(), "嵌套路径未自动创建父目录"
            print("  mkdir OK（嵌套父目录自动创建）")
    finally:
        if saved_frozen is None:
            if hasattr(sys, "frozen"):
                del sys.frozen
        else:
            sys.frozen = saved_frozen
        if saved_env is not None:
            os.environ["HIT_ANZ_CONFIG"] = saved_env
        settings._reset_data_dir()

    print("paths selftest OK：源码 / 环境覆盖 / frozen 模拟 / 目录创建")


if __name__ == "__main__":
    main()
