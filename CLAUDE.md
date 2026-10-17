# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Firmware for the team's custom RoboMaster armor plate (hardware "EVE" rev 080 → 083) on an STM32L432KBU6 (Cortex-M4F, 80 MHz, bare-metal HAL, no RTOS).

- **Hit detection** — a TI ADS131M04 24-bit ΔΣ ADC on SPI1 samples 4 strain-gauge channels at 3.90625 kHz (256 µs/frame); `App/detect/hit_detect.c` turns the summed, high-passed force into at most one event per physical hit. A trigger locks a 128-sample window (32.8 ms: P21 = 64 pre-trigger + P22 = 64 post) whose normalized cross-correlation against a Flash-stored reference (the first accepted hit after the EMA warm-up of each power-up, provided its `|hp_sum|` peak lies ≥5 samples from either window edge — edge-peaked windows are skipped and the opportunity passes to the next accepted hit) is the shape-based confirmation criterion.
- **Feedback** — 13-LED WS2812 strip on USART2, plus PB0 (over-temperature) and PB1 (system-normal) indicators.
- **Uplink** — classic CAN 500 kbps to the team's L431PM power-management board ("电管"), which owns HP bookkeeping, death/respawn and the ESP32/Wi-Fi path. This board never maintains HP, never judges death, and is **not** connected to the official DJI referee system (the RM 2025 manual is a semantics reference only).
- Four identical plates run the same binary. The L431PM assigns each a runtime NodeID 1–4 derived from its STM32 UID; NodeID lives in RAM only, and no business CAN frame is sent or processed until the node is READY.

Status: the application layer is implemented and has run on hardware — ADC driver, LED effects, detection pipeline, unified fault registry and non-blocking power-on self-test have all landed. The hit-reference store (`App/bsp/ref_store.c`) is the one piece of persistence that exists; the remaining gap is parameter persistence: `Cal_Load`/`Cal_Save` are still `TODO(Step 5)`, so every boot restores `Cal_GetDefaults()`.

## Build, flash, debug

- Toolchain: `arm-none-eabi-gcc` + `make` (this machine: MSYS2, `/e/programfile/MSYS2/mingw64/bin`). `make -j` → `build/Armor_Cmzy26_081.{elf,hex,bin,map}` plus per-file `.lst`; `make clean`; `make GCC_PATH=<dir>` if the toolchain is not on PATH.
- No tests, no linter, and no flash target in the Makefile. `openocd` is on PATH here:
  `openocd -f interface/stlink.cfg -f target/stm32l4x.cfg -c "program build/Armor_Cmzy26_081.elf verify reset exit"`
- `.vscode/tasks.json` provides `STM32: Build` / `STM32: Flash HEX (J-Link)` / `STM32: Build and Flash`, but hard-codes paths from another machine (`E:\GnuWin32\bin\make.exe`, `C:\Program Files\JLink_V960\JLink.exe`) that do not exist here — prefer the CLI above, or fix the paths first.
- Runtime debugging is SEGGER Ozone via `Armor_DBG.jdebug` (loads `build/Armor_Cmzy26_081.elf`, SWD 4 MHz). `Armor_DBG.jdebug.user` holds ~40 pre-configured watched expressions, a Timeline graph (`s_last_frame.ch[0..3]`, `s_hit_sum`, `drdy_cnt`, `drop_cnt`, `s_loop_max_us`) and a Data Sampling window; the CSVs at the repo root were exported by hand from those windows.
- Logging is USART1 @115200 8N1 (connector U18), a non-blocking ring buffer flushed 2 characters per main-loop pass — any serial terminal shows startup, `[HIT]` and `[SELFTEST]` lines.
- `compile_commands.json` feeds clangd and is in sync with `app.mk` (16 App sources, 48 entries total). Regenerate it by hand after adding sources — no generator script exists in the repo.

## Architecture

### Layering

`App/` is the only hand-written tree (CubeMX regeneration only rewrites `Core/` and the Makefile). Dependencies point downward in principle: `app/` → `detect/` + `comm/` + `bsp/`; `detect/` and `comm/` → `bsp/`; `bsp/` → HAL + `board.h`. Three deviations are deliberate and load-bearing: `App/app/faults.h` and `App/detect/calibration.h` are shared cross-layer interface headers (included from `detect/`, `bsp/temp_mon` and `comm/` alike); `App/comm/board_comm.c` includes `app/` headers, because it orchestrates state machine, LEDs and reporting; and `App/bsp/ads131m04.c` includes `comm/app_log.h`. The one rule that holds absolutely: `detect/` must never depend on `comm/`. **`App/README.md` is the conventions document** (naming prefixes, include guards, interrupt-ownership table, extension recipes) — read it before adding a module.

`App/board.h` is the single entry point for `extern` peripheral handles; never `extern` a CubeMX handle anywhere else.

### Startup and scheduling

`Core/Src/main.c` USER CODE hooks: `App_Init()` after `MX_TIM2_Init()`, `App_Loop()` in the `while(1)`. `App_Init` initializes modules in dependency order and then explicitly starts the 1 kHz system tick — **CubeMX only generates `MX_TIM2_Init()`, the `HAL_TIM_Base_Start_IT(&htim2)` is deliberate and must stay last** so no tick hits a half-initialized module.

- `App_OnTick1ms()` (TIM2 ISR): `StateMachine_Tick` + `LedStatus_Tick` every millisecond; `TempMon_Tick` + `HitDetect_Tick` at 20 Hz; a 1 Hz diagnostics snapshot (`s_rate_drdy/frames/spi_err/drop`).
- `App_Loop()` (main loop): `SelfTest_Poll` → consume one ADC frame if ready → `HitDetect_Feed` → drain hit events → `StateMachine_OnHitEvent` → `BoardComm_ReportHitEvent` (only while state is `SM_STATE_HIT`) → `[CONF]` diagnostics → `BoardComm_Loop` → PVD self-test retry → `App_Log_FlushSmall(2)`.
- **Calibration exit** (`StateMachine_Tick`, reached from the 1 kHz ISR): until `HitDetect_RefTrainedThisBoot()` is true the board sits in the purple two-stage phase — `REF_WARMUP` (1 Hz, the baseline-EMA 5τ) then `REF_PENDING` (5 Hz, the 20 s hit-detection window). `Sm_RefPendingNeeded()` leaves it on any of three paths: training completed; `BoardComm_MasterHeard() && HitDetect_RefValid()` (a master-direction CAN frame has arrived **and** Flash holds a reference — the car is already running, so don't make it wait); or 5τ + 20 s elapsed with a valid Flash reference. All three go through `Sm_LeaveRefPending()`, which since 2026-10-17 only sets NORMAL + LEDs — the page is no longer erased at boot, so an untrained exit needs no writeback and the reference survives by simply never being touched. With Flash empty, training is the only exit.

### Data flow

DRDY on PA15 (EXTI) pulls `ADC_CS` low and starts the SPI1 TX/RX DMA pair; the DMA-complete ISR releases CS and sets a frame-ready flag. Frames land in a 32-slot DMA ring (18 B each — ~8.2 ms of slack between the DRDY ISR and the main loop; the detection layer keeps its own, separately sized history ring). Concurrency rule (design doc R17): **the write pointer is touched only by ISRs, the read pointer only by the main loop**, and slot state uses atomic flags — ISRs only set flags/start DMA/count, they never call protocol code.

`HitDetect` judges on `hit_sum = Σ high-passed channels` (the differential signal is trigger-only), which is what guarantees one event per hit; waveform capture keeps the un-differenced sum. Tunables are the `hit_param_t` fields P01–P27 in `App/detect/calibration.h`; defaults live only in `Cal_GetDefaults()`. The confirmation criterion is the **Pearson correlation** of the locked window against the stored reference (`corr_min_q10`, P27 — the field's own value *is* the threshold: at or above it **accepts**, below it **rejects**: no event, no state machine, no CAN, not counted in `s_hit_count`; `0` = measure only, never reject, the mode to use when re-collecting a distribution). P27 is being tuned on the bench, so read the current number from `Cal_GetDefaults()` rather than trusting any value quoted in prose — `Cal_Load` is still a stub, so that one line is the only definition. **Three gates let a hit through regardless of P27**: (a) the baseline-EMA warm-up, 5τ after boot (`HitDetect_GetWarmupMs()`, ≈10.5 s at `s_bl`=13) — the filter settles from zero, so early windows are impure; those hits still count and report but are neither rejected nor trained on, which is what keeps the reference clean; (b) no valid reference — `corr_q10` is then 0, so judging would reject the first hit and make training unreachable, i.e. a fresh board could never train at all; (c) the reference has not been trained **this boot** (`s_ref_trained_boot` = 0, added 2026-10-17) — only a boot-trained reference may reject, otherwise a stale reference from a different excitation method would reject the first hit and make retraining unreachable (the mirror of gate (b)). Both windows are mean-subtracted and divided by the product of their standard deviations, so a hit's *amplitude* never influences the verdict — force is carried by the event's `force_01n`/`intensity` fields instead. The fixed-point form is `num / (den >> 10)`, **not** `num * 1024 / den` — the latter overflows uint64 at this window length and scores a perfect match as ~97. Host-verified numbers on a synthetic impact proxy (n = 128; full table in design doc §7.1): amplitude ×1.25e-4 … ×110 → 1019…1024; 1/4/12-sample misalignment → 943/92/78; equal-amplitude sine ringing → 163; white noise → 23.

Faults are published to a single 16-bit registry (`App/app/faults.h`) with a live domain (sensor saturation/offset/dead channel, TEMP window, over-temp) and a sticky domain (SPI/DRDY stall, PVD brownout) that only a passing self-test or a reset clears. State machine, LED effects and CAN reporting all read that one registry. `SelfTest_Poll` runs a ~1.1 s non-blocking sequence and takes over frame consumption while it runs (it keeps feeding `HitDetect` so the pipeline never starves).

### CAN protocol

`Docs/CAN_PROTOCOL.md` is the authoritative and current definition — fixed frames, standard 11-bit IDs, no ISO-TP/TLV (the ISO-TP design was explicitly removed on 2026-09-06). Summary: enumeration `0x120`–`0x123` (ENUM_START → UID announce → NodeID assign → ACK `0xA5`); business IDs `0x130 + (NodeID-1)×0x10 + offset` with offsets `0x0` reset cause, `0x1` fault flags, `0x2` init done, `0x3` hit event, `0x4/0x5` status query/reply, `0x6/0x7` LED control/ACK, `0x8/0x9` identity query/reply, `0xA` hit ACK. The RX filter accepts `0x100–0x17F`, standard data frames only. Hit events retry every 100 ms until the L431PM acknowledges the sequence; **the communication layer emits at most one CAN frame per main-loop pass** to protect the sampling path. The link counts as lost after `BOARD_COMM_LOST_MS` = 300 ms with no request **correctly addressed to this board's own slot** — enumeration frames and sibling boards' traffic are explicitly not heartbeat. Beware the interaction: a master that has only sent enumeration frames still sets `BoardComm_MasterHeard()` (which is direction-based, not addressing-based) without refreshing the heartbeat timer.

### Peripheral map (current branch, from `.ioc` / `board.h`)

| Peripheral | Pins | Notes |
|---|---|---|
| CAN1 | PA11 RX / PA12 TX | 500 kbps (prescaler 10, 1+13+2 TQ). Auto-retransmission is turned **on at runtime** by `App_Can_Init` clearing the `NART` bit, overriding the CubeMX-generated `AutoRetransmission = DISABLE` |
| SPI1 | PB3 SCK / PB4 MISO / PB5 MOSI, PC14 = `ADC_CS` (software) | Mode 1 (CPHA=2EDGE), 8-bit, 5 MHz, NSS off, TX/RX DMA (DMA1 Ch3/Ch2) |
| USART1 | PA9 TX / PA10 RX | 115200 8N1 debug log |
| USART2 | PA2 | WS2812: 3.75 MBd, TX-only, **TX inverted**, oversampling 8, TX DMA. 12 bytes per LED, GRB order, 156-byte frame (the older 2.667 MBd fractional scheme was abandoned) |
| TIM16 CH1N | PB6 = `ADC_CLK` | 8 MHz CLKIN for the ADC (PWM, ARR=10−1) |
| ADC1 | PA1 = IN6; PA5–PA7 = IN10–IN12 | 4-channel scan (CH6/10/11/12 = TMP_REF1–4), TIM1 CC1 trigger (≈1.22 kHz), 47.5-cycle sampling, DMA circular + continuous requests |
| DAC1 | PA4 (`ROHT_DAC_TEST_NC`) | Initialized by CubeMX, currently unused by the application |
| GPIO | PA15 `ADC_NDRDY` in (EXTI15), PC14 `ADC_CS` out, PC15 `ADC_SYNC` out, PB0 `IND_ROHT` / PB1 `IND_NORM` out | Indicator LEDs are **active high**; CS/DRDY are swapped relative to the schematic — the `.ioc` is authoritative |

Clock tree: HSE bypass 8 MHz → PLL (N=20, R=2) → 80 MHz SYSCLK, ADC clock 32 MHz, Clock Security System and PVD enabled.

### Flash layout

128 KB, 2 KB pages, single bank — an erase or program stalls instruction fetch, so both are main-loop/init-only operations. **The last page (`0x0801F800`, page 63) is reserved for the hit-reference store** (`App/bsp/ref_store.c`): magic + version + up to 128 `int32` sum samples + CRC32, one page, single copy, no double-buffering. Two rules:

- `RefStore_Load` runs once from `HitDetect_Init` inside `App_Init` and is a **pure read — it never erases the page** (validates and copies to RAM first). The page always holds the last reference between writes, so any power loss outside a write keeps the reference intact (this "scheme B" replaced the 2026-10-17 design of erase-at-boot + writeback-at-exit, whose erase→writeback window could empty the page on a power cut).
- `RefStore_Save` (the only write, from `Hit_RefTrain` on this boot's first accepted hit) does **erase + program ≈31 ms** in the main loop, inside the 50 ms refractory window, and **must never be called from an ISR**. Any future persistence (`Cal_Save`, identity, …) starts at page 62 and grows downwards; `_Static_assert`s in `ref_store.c` pin the page number and the 528-byte layout.

### ADS131M04 driver

`App/bsp/ads131m04.c` is a full driver, not the TI port: SPI mode 1, software CS, DRDY EXTI, DMA frame reads, ring buffer, diagnostics. Two rules that will break hardware if ignored:

- **Never write the `MODE` register** — on this silicon (REVID=0x05) it clears the word length to 16-bit and desynchronizes the whole frame stream (`ADS_REG_INIT_WRITE_MODE` stays 0).
- Register init (`ADS_REG_INIT_ENABLE=1`) writes every register except MODE, on the assumption the reset defaults are the target config. `Driver_Examples/ads131m08/ads131m0x.h` is the authority for register bit definitions; `Docs/ADS131M04_初始化SPI时序.md` is the frame-by-frame SPI trace with expected MISO responses for debugging init.

### CubeMX regeneration checklist

`Armor_Cmzy26_081.ioc` is the authoritative pin/peripheral source (the schematic has known labeling errors). After every regeneration: ① re-add `-include app.mk` to the Makefile **after** the `C_INCLUDES` block and **before** `OBJECTS`; ② confirm `USER CODE` blocks `Includes`/`2`/`3` still hold the `App_Init`/`App_Loop` calls; ③ re-apply any runtime fixups CubeMX would undo. Also required: `app.mk` filenames must stay globally unique, since the Makefile flattens objects with `notdir`.

### Debug-variable convention

Every runtime-observable value is exposed as a **scalar** global (`s_last_frame`, `s_hit_sum`, `s_hit_bl[0..3]`, `s_rate_*`, `s_loop_max_us`, `s_init_rx_w0[n]`, `s_st_rms[n]`, `can_node_diag`, …) because Ozone's CSV export cannot serialize arrays. When adding state worth watching on the bench, add a scalar next to it instead of expecting a debugger watch to unfold a struct.

## Hardware revisions and branches

Branches track hardware revisions, and the analog front end differs between them — a wrong firmware constant shows up as gain errors, not as a build failure. Recent commits ("量产版前端增益变动", "新硬件固件增益设置错误，修复为0x333") are exactly this class of bug.

| Branch | What it is |
|---|---|
| `Test_RegularEdition_082` (current) | Production/regular edition: HSE-sourced PLL, CSS on, ADC clock 32 MHz, `GAIN1=0x3333` (PGA ×8) matching the 499 R production front end |
| `Temp_Nhse` | Earlier experimental line: HSI-sourced PLL, ADC clock 64 MHz, no CSS, plus a bootloader and dual-slot A/B images (`STM32L432xx_{BOOT,SLOT_A,SLOT_B}.ld`) with Python CAN/firmware-update tooling under `tools/`, and the ISO-TP protocol |
| `Dev_HW080` | First working rev 080 line |
| `main` | Initial commit only (HW080 baseline) |

The rev 081 → 083 front-end change (5K1 → 499 R) is why the gain constant moved (`GAIN1=0x3333`). The crew manual that recorded it is no longer in the tree.

## Documentation map

`Docs/` is mostly Chinese. Trust order when documents disagree with code:

- `Docs/CAN_PROTOCOL.md` — sole authority for CAN, in sync with the code.
- `Docs/开机标定与状态手册.md` — operator-facing manual for the power-on sequence: the BOOT→warm-up→detection-window timeline with exact durations, the three calibration exits, the `ref_store` Flash latch layout and its erase-on-every-boot behaviour, the master-heard skip, every LED pattern with RGB values and periods, and a bench-triage table. Where this and the design doc overlap, this one is written from the code and is the more current of the two.
- `Docs/装甲模块固件设计方案.md` (v1.5) — authority for hardware facts, pinout, detection algorithm and the P01–P27 table (§7.1 documents the correlation-confirmation pipeline and its host-verified numbers). Realigned to the code on 2026-10-17; chapter 9.1–9.4 is now explicitly banner-marked as a historical ISO-TP record. Five numbers are still knowingly unreconciled and were left as written rather than silently changed: §3.3 66 ms vs 70.4 ms minimum trigger interval, §6.3's 50 ms power-on delay, P27's "≈600", §7.7's ≈50 KB image size, §12-M1's MODE description. `Docs/ADS131M04_初始化SPI时序.md` — §4.1/4.3 and §7/8/10.2 track current firmware; §4.2 and §5/§6 are retained as historical traces.
- `Docs/装甲模块方案概述.md` — orientation/overview, realigned on 2026-10-17; its §4 front-end gain (4×INA826, G≈10.7) and its 19N/490N force figures are still unreconciled against the production 0.5435× front end.
- `Docs/装甲板后续开发Agent提示词.md` — the L431PM integration brief. Its three debug commands have all shipped, so it is now a **regression checklist** to re-run after communication-layer changes, not a to-do. Still carries the explicit do-nots: don't change the `0x120`–`0x123` wire format or the UID sort rule, don't persist NodeID to Flash, don't flash hardware unasked.
- Hardware: `SCH_Armor_Sc_EVE_081_2026-08-20.pdf`; datasheets for the ADS131M04, OPA1656, STM32L432 and RM0394. The rev 083 schematic is not in the tree.

## Sibling trees (read-only, untracked)

Both are independent nested git repositories that the parent repo does not track (nor ignore). Do not edit them or add them to the parent's history without an explicit request.

- `SHOOT_cmzy_REV081/` — firmware for the robot's barrel module (STM32F072C8U6, VCNL4040 shot detection, its own CAN at 1 Mbps). The Armor WS2812 driver and team colors were ported from here; the Armor board never talks to it directly.
- `Driver_Examples/stm32-referee-isotp-stack/` — an STM32F407 ISO-TP + TLV + ACK reference stack (CMake). Background reference only; the Armor firmware must not reintroduce ISO-TP.
- `Driver_Examples/ads131m08/` — TI's ADS131M0x driver; its register map is still referenced, but its `hal.c` is SimpleLink-based and was never ported.
