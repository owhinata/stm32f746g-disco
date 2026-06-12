# Camera (B-CAMS-OMV / OV5640)

ST's camera module bundle **B-CAMS-OMV** (MB1683 adapter + MB1379 camera module, **OV5640** 5MP sensor) connects to the board's **P1** (30-pin ZIF) connector. The driver lives in `port/camera/camera.{c,h}`, the shell command is `camera` (`shell/cmds/cmd_camera.c`).

Camera epic (#22): Phase 1 (#39) = sensor detection (PWR_EN + I2C1/SCCB chip ID); Phase 2 (#41) = **DCMI + DMA2 single-frame QVGA RGB565 snapshot into SDRAM** (`camera capture`). Saving frames to a file is Phase 3 (#42).

## Wiring (P1 ↔ B-CAMS-OMV CN5)

The board's P1 (UM1907 §7.2 Table 5) and the B-CAMS-OMV 30-pin ZIF CN5 (UM2779 Table 7) match **1:1 for every signal** over the bundled FFC (the two connectors number their pins in opposite order).

| Signal | MCU pin | Notes |
|--------|---------|-------|
| I2C1_SCL (SCCB) | PB8 | AF4, open-drain. Bus shared with the CN2 extension connector |
| I2C1_SDA (SCCB) | PB9 | AF4, open-drain |
| DCMI_PWR_EN | PH13 | **Low = powered** (the module's POWER_DOWN, active-high) |
| DCMI_NRST | -    | tied to the board **NRST net** (no GPIO control) |
| DCMI_HSYNC | PA4 | AF13 |
| DCMI_PIXCLK | PA6 | AF13 |
| DCMI_VSYNC | PG9 | AF13 |
| DCMI D0-D4 | PH9-PH12, PH14 | AF13 |
| DCMI D5 / D6 / D7 | PD3 / PE5 / PE6 | AF13 |

No conflict with the existing peripherals (USART1=PA9/PB7, QSPI=PB2/PB6/PD11-13/PE2, SDMMC1=PC8-13/PD2, LED=PI1).

!!! note "Clocking (XCLK)"
    The MB1379 module clocks the OV5640 from its **own 24 MHz crystal** (MB1379/X1, UM2779 §3.2). The host supplies **no MCO**. P1 pin 21 (Camera_CLK, OSC_24M from the board's X1) is wired but unused.

## I2C (SCCB)

| Item | Value | Source |
|------|-------|--------|
| Instance | **I2C1** (PB8/PB9) | UM1907 CN2 / P1 |
| Address | **0x78** (8-bit write) | OV5640 / H747I BSP |
| Register addresses | **16-bit** (`I2C_MEMADD_SIZE_16BIT`) | OV5640 datasheet |
| Chip ID | 0x300A/0x300B = **0x5640** | OV5640 datasheet |
| Speed | ~100 kHz standard mode | SCCB |

TIMINGR is recomputed for **PCLK1 = 54 MHz** (RM0385 §30.4.10): PRESC=11 / SCLL=24 / SCLH=19 / SCLDEL=5 / SDADEL=2 → SCL ≈ 99 kHz. The ST BSP constant `0x40912732` assumes APB1 = 50 MHz and is deliberately not reused (it would land at ~118 kHz on this clock tree).

## Power and reset

- `camera_init()` writes the OFF level to PWR_EN (PH13) **before** switching the pin to output, so the module never sees a power glitch
- Every probe starts with a **power cycle** (high 100 ms → low → 20 ms settle, the H747I BSP HwReset timing)
- DCMI_NRST cannot be driven from a GPIO, so a **PWR_EN cycle plus the OV5640 software reset** (`OV5640_ReadID` writes 0x3008=0x80 and waits 500 ms) stands in for it

## OV5640 component driver (submodule)

Sensor register control uses ST's official **OV5640 BSP component driver**:

- submodule: `lib/ov5640` = [STMicroelectronics/stm32-ov5640](https://github.com/STMicroelectronics/stm32-ov5640) **v4.0.3** (BSD-3-Clause, see `NOTICE`)
- BSP v2 API (`OV5640_Object_t` + `OV5640_IO_t`), pure-I2C and self-contained (no DCMIPP dependencies)
- The bus glue is `cam_io_*` in `port/camera/camera.c` (`HAL_I2C_Mem_Write/Read`). `OV5640_ReadID()` calls `IO.Init()` unconditionally, so **no-op Init/DeInit stubs must be registered**

!!! note "Why not stm32-mw-camera"
    ST's Camera Middleware (`stm32-mw-camera`) and X-CUBE-ISP target **DCMIPP** (STM32N6 family) and cannot drive the F7's classic DCMI. Only the standalone OV5640 component driver is pulled in.

## Capture path (DCMI + DMA2, #41)

| Item | Value | Source |
|------|-------|--------|
| Resolution / format | **QVGA 320x240 RGB565** (153,600 B, little-endian) | `OV5640_Init(R320x240, RGB565)` |
| DCMI | 8-bit parallel, HW sync, **HSYNC=HIGH / VSYNC=HIGH / PCLK=RISING** | H747I-DISCO BSP's proven OV5640 values (`OV5640_Init` programs the matching sensor side) |
| DMA | **DMA2 Stream1 / Ch1** (RM0385 Table 26; SD owns Stream3/6 -- no conflict), `DMA_NORMAL` single-shot, word, FIFO full + MBURST INC4 | 38,400 words ≤ the 65,535 NDTR ceiling → one plain transfer |
| Frame buffer | `.sdram` section (8 MB SDRAM, **MPU non-cacheable** → no cache maintenance) | see [SDRAM](sdram.md) |
| NVIC | DCMI=8, DMA2_Stream1=8 (below USART1=5 / SDMMC=6 / SD-DMA=7, above SysTick=14) | ThreadX masks with PRIMASK, so `tx_semaphore_put` is ISR-priority-agnostic |

Completion model (HAL): once the DMA has moved the frame's words, `DCMI_DMAXferCplt` arms the **FRAME interrupt**; the FRAME ISR calls `HAL_DCMI_FrameEventCallback` → `tx_semaphore_put`. Sync errors, overrun and DMA errors all funnel into `HAL_DCMI_ErrorCallback`. The same **drain + `cam_xfer_active` gate + finite timeout (1 s)** discipline as the SD driver suppresses stale callbacks.

Sensor setup is lazy, at capture time: probe if needed → `OV5640_Init` (once per power-up, 300 ms AEC/AWB settle) → `OV5640_ColorbarModeConfig` (100 ms settle on a live/colorbar switch) → snapshot. `sdram test` clobbers `.sdram`, so it calls `camera_frame_invalidate()` first to drop the captured-frame flag.

## Locking and thread context

- Public API calls serialize on an internal ThreadX mutex (TX_INHERIT). The real work lives in `*_locked()` helpers, so no public entry can **re-acquire the mutex it already holds** (Phase 2's capture will probe on demand through the `_locked` variant)
- **Thread context only** -- never call the API from an ISR
- Initialization runs once from `tx_application_define()` (`src/main.c`) and performs **no sensor I/O** (GPIO/I2C1 setup plus the mutex). The first sensor access happens lazily on `camera probe`

## The `camera` shell command

```
camera probe          power cycle + read the OV5640 chip ID (~1 s)
camera info           driver / sensor state
camera capture [test] snapshot one QVGA RGB565 frame (test = colorbar pattern)
camera off            cut module power
```

`camera capture` prints per-channel (R5/G6/B5) min/max/mean statistics and a hexdump of the first 16 pixels. `camera capture test` grabs the OV5640's built-in **8-bar colorbar** (white/yellow/cyan/green/magenta/red/blue/black), validating DCMI polarity, timing and wiring **independent of optics** (expect mins near 0, maxes near saturation, flat values within each band). On a live capture, covering the lens must pull the means down.

Example session:

```
sh> camera capture test
camera: capturing colorbar test frame ...
frame: 320x240 RGB565 (153600 bytes)
R5: min  0  max 31  mean 15.4
G6: min  0  max 63  mean 31.2
B5: min  0  max 31  mean 15.5
row0[0..15]:
00000000: ff ff ff ff ff ff ff ff  ff ff ff ff ff ff ff ff
00000010: ff ff ff ff ff ff ff ff  ff ff ff ff ff ff ff ff
sh> camera capture
camera: capturing live frame ...
frame: 320x240 RGB565 (153600 bytes)
...
```

## References

- UM2779 — B-CAMS-OMV user manual (CN5 pinout, MB1379/X1 crystal, JP1)
- UM1907 §7.2 — P1 camera connector pinout
- RM0385 §30 — I2C (TIMINGR)
- `_ref/STM32Cube_FW_H7_V1.13.0/Drivers/BSP/STM32H747I-DISCO/stm32h747i_discovery_camera.c` — reference for the OV5640 power sequence / polarities
