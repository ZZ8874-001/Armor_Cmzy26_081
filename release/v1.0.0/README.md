# Armor_Cmzy26_081 固件 + Hit_ANZ 上位机 v1.0.0

本发布包含 **3 个硬件/传感器变体** 的固件（elf/hex/bin/map）与 Windows 上位机 EXE。
变体由编译期宏选择（`ARMOR_VARIANT`），**烧错变体会表现为增益错误（信号差 4 倍）或 082 无法启动**。

## 变体矩阵

| 变体 | 板 | 传感器 | PGA 增益 | GAIN1 | 时钟 |
|---|---|---|---|---|---|
| `082_500g` | 082 | 500g | ×1 | `0x0000` | HSI16 /2 → 80MHz（PLLSAI1 同源 M=2 → ADC 32MHz） |
| `083_500g` | 083 | 500g | ×1 | `0x0000` | HSE 8MHz → 80MHz（PLLSAI1 → ADC 32MHz） |
| `083_2kg` | 083 | 2kg | ×4 | `0x2222` | HSE 8MHz → 80MHz（PLLSAI1 → ADC 32MHz） |

固件文件位于 `firmware/<变体>/Armor_Cmzy26_081_<变体>_v1.0.0.{elf,hex,bin,map}`。

## 烧录

```bash
# 以 083_2kg 为例（把路径换成你的变体）：
openocd -f interface/stlink.cfg -f target/stm32l4x.cfg \
        -c "program release/v1.0.0/firmware/083_2kg/Armor_Cmzy26_081_083_2kg_v1.0.0.elf verify reset exit"
```

## 烧写后判别（唯一可靠手段）

CAN/状态帧不携带版本信息，请在上电日志（USART1 @921600 8N1）核对：

```
[APP] Armor v1.0.0 (<变体>, gain1=<0x0000|0x2222>, <HSI16/2|HSE8>)
[ADS] WREG 0x04=<0x0000|0x2222> OK
```

- 500g 变体：`gain1=0x0000`；2kg 变体：`gain1=0x2222`；
- 082 变体：`HSI16/2`；083 变体：`HSE8`；
- 082 若启动即停（无日志），通常是 CubeMX 重生成把 `Core/Src/main.c` 与
  `Core/Src/stm32l4xx_hal_msp.c` 的 `ARMOR_HW_082` 守卫冲掉（HSE 配置 +
  HSI 硬件 → PLLSAI1 不匹配 → `Error_Handler`）。重新生成后需恢复守卫。

## 上位机（Windows，无需 Python）

`host/Hit_ANZ/` 整个文件夹拷到目标机，双击 `Hit_ANZ.exe`。

- **不要单独拷 EXE**：`_internal/` 是它的一部分；
- **不要运行 `pyi_build` 里的同名 EXE**（那是中间产物，会报 Failed to load Python DLL）；
- 首次运行在 EXE 旁生成 `config.json`（目录不可写时回退 `%LOCALAPPDATA%\Hit_ANZ`）；
- 串口 921600 8N1；在线调参与参照替换见 `Hit_ANZ/README.md`。

## 标定集说明

`host/Hit_ANZ/calib/transfer.npz` 是**当前测试板**的 EMA 定位标定集，与板面几何/
传感器安装相关。换一块板或改安装后需重新标定（上位机「落点分析 → 定位标定」）。

## 工程构建（复现）

```bash
make -j BUILD_DIR=build/083_2kg ARMOR_VARIANT=083_2kg   # 默认变体可省略 ARMOR_VARIANT
py -3 tools/release_build.py                            # 重新生成本发布目录
```

## 文件校验

```bash
sha256sum -c SHA256SUMS
```
