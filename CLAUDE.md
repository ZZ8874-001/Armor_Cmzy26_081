# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Firmware for a custom RoboMaster (DJI) referee-system armor module ("Armor", hardware rev 080/081) built around the STM32L432KBU6 (Cortex-M4F). The board detects hits with a TI ADS131M04 24-bit delta-sigma ADC and communicates with the RoboMaster 2025 referee system over CAN.

The codebase is a freshly generated STM32CubeMX HAL project: clock/GPIO/peripheral init is complete, but every `USER CODE BEGIN/END` block is empty — the application layer has not been written yet. The overall firmware design (hit detection, LED driving, communication over the team's in-vehicle CAN + ISO-TP bus at 500 kbps, module breakdown, milestones) is specified in **`Docs/装甲模块固件设计方案.md`** — read it before implementing application code; it also lists CubeMX config changes still to be applied.

## Build

- Toolchain: `arm-none-eabi-gcc` (on this machine via MSYS2 mingw64). Pass `make GCC_PATH=<dir>` if the toolchain is not on PATH.
- `make -j` → `build/Armor_Cmzy26_081.{elf,hex,bin,map}` (plus per-file `.lst` listings).
- `make clean`
- Application code lives in `App/` and is added via `-include app.mk` in the Makefile (after C_INCLUDES, before OBJECTS). **CubeMX regeneration overwrites the Makefile — re-add the include line after regenerating.** Structure conventions are documented in `App/README.md`.
- No tests, no linter, and no flash target in the Makefile. `openocd` (ST-Link) is installed on this machine for flashing:
  `openocd -f interface/stlink.cfg -f target/stm32l4x.cfg -c "program build/Armor_Cmzy26_081.elf verify reset exit"`
- `compile_commands.json` at the repo root feeds clangd (LSP); regenerate it after adding sources (its entries mirror the Makefile flags + `-IApp/...`).

## Architecture

### CubeMX-generated skeleton

`Armor_Cmzy26_081.ioc` is the CubeMX project. `Core/Src/main.c`, `stm32l4xx_it.c`, `stm32l4xx_hal_msp.c`, and `Core/Inc/main.h` are regenerated from it — only edit inside `USER CODE BEGIN/END` markers; anything outside is overwritten on regeneration. **Pin/peripheral definitions are authoritative in the `.ioc`** — the schematic has known labeling errors (e.g., HSE is 8 MHz, not 12 MHz).

Clock tree: HSE bypass (external clock input) → PLL (M=1, N=20, R=2) → 80 MHz SYSCLK, buses undivided; PLLSAI1 clocks ADC1; Clock Security System and PVD (level 6) are enabled.

### Peripheral map (from `.ioc` / `stm32l4xx_hal_msp.c`)

| Peripheral | Pins | Notes |
|---|---|---|
| CAN1 | PA11 RX / PA12 TX (AF9) | Team in-vehicle bus; 500 kbps configured (presc 10, 1+13+2 TQ) ✓ |
| SPI1 | PB3 SCK / PB4 MISO / PB5 MOSI / PC14 CS (software, GPIO) | Fully configured ✓: 8-bit, 5 MHz (presc 16), Mode 1 (CPHA=2EDGE), NSS off, TX/RX DMA (DMA1_Ch3/Ch2). 5 MHz chosen for DOUT timing margin (tp(SCDO)=50 ns worst case) |
| USART1 | PA9 TX / PA10 RX | 115200 8N1 full duplex |
| USART2 | PA2 TX (AF7, push-pull, no pull) | Fully configured ✓: WS2812 strip (≤13 LEDs), 2.6667 MBd, TX-only, TX DMA (DMA1_Ch7) |
| TIM16 CH1N | PB6 = `ADC_CLK` | 8 MHz CLKIN configured (PWM, ARR=9) ✓ |
| ADC1 | PA1 = IN6 (regular CH6); PA5–PA7 = IN10–IN12 analog | Fully configured ✓: 4-channel scan (CH6/10/11/12), TIM1 CC1 external trigger (≈1.22 kHz), 47.5-cycle sampling |
| GPIO | PA15 `ADC_NDRDY` in (EXTI15), PC14 `ADC_CS` out, PC15 `ADC_SYNC` out, PB0 `IND_ROHT` / PB1 `IND_NORM` out | ADS131M04 DRDY/CS/SYNC lines (CS/DRDY swapped vs schematic — CubeMX is authoritative); PB0 = over-temperature LED, PB1 = system-normal LED |

### ADS131M04 driver — integration reference

`Driver_Examples/ads131m08/` is TI's reference driver for the ADS131M0x family (applies to the M04 as well):

- `ads131m0x.h/.c` — portable device layer (register map, command sequencing, CRC); default word length 24-bit.
- `hal.h/.c` — hardware abstraction called by the device layer. The provided implementation uses TI SimpleLink APIs and **must be ported to STM32 HAL** (SPI, GPIO, delay) before use; see the bundled `README.md`.
- ADS131M0x uses SPI mode 1 (CPOL=0, CPHA=1) per that README, while the CubeMX SPI1 config uses CPHA=1edge (mode 0) — verify against the ADS131M04 datasheet in `Docs/` when wiring it up.

### Docs

`Docs/` holds the design references (mostly in Chinese): board schematic `SCH_Armor_Sc_EVE_081_2026-08-20.pdf`, the RM 2025 referee system user manual (V1.4), DJI armor module manuals (AM01/AM11 and AM02/AM12), the ADS131M04 datasheet, and the STM32L432 datasheet plus RM0394 reference manual. `Docs/ADS131M04_初始化SPI时序.md` is the frame-by-frame SPI trace of the ADC init sequence (122 frames, with expected MISO responses) — the authoritative reference when debugging or modifying `App/bsp/ads131m04.c`.
