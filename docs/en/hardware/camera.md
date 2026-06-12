# Camera (B-CAMS-OMV / OV5640)

ST's camera module bundle **B-CAMS-OMV** (MB1683 adapter + MB1379 camera module, **OV5640** 5MP sensor) connects to the board's **P1** (30-pin ZIF) connector. The driver lives in `port/camera/camera.{c,h}`, the shell command is `camera` (`shell/cmds/cmd_camera.c`).

This is **Phase 1** (#39) of the camera epic (#22). Phase 1 covers **sensor detection only**: power control (PWR_EN) and the chip ID read over I2C1 (SCCB). DCMI + DMA frame capture is Phase 2 (#41); saving frames to a file is Phase 3 (#42).

## Wiring (P1 ↔ B-CAMS-OMV CN5)

The board's P1 (UM1907 §7.2 Table 5) and the B-CAMS-OMV 30-pin ZIF CN5 (UM2779 Table 7) match **1:1 for every signal** over the bundled FFC (the two connectors number their pins in opposite order).

| Signal | MCU pin | Notes |
|--------|---------|-------|
| I2C1_SCL (SCCB) | PB8 | AF4, open-drain. Bus shared with the CN2 extension connector |
| I2C1_SDA (SCCB) | PB9 | AF4, open-drain |
| DCMI_PWR_EN | PH13 | **Low = powered** (the module's POWER_DOWN, active-high) |
| DCMI_NRST | -    | tied to the board **NRST net** (no GPIO control) |
| DCMI D0-D7 / HSYNC / VSYNC / PIXCLK | (Phase 2) | AF13, used by #41 |

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

## Locking and thread context

- Public API calls serialize on an internal ThreadX mutex (TX_INHERIT). The real work lives in `*_locked()` helpers, so no public entry can **re-acquire the mutex it already holds** (Phase 2's capture will probe on demand through the `_locked` variant)
- **Thread context only** -- never call the API from an ISR
- Initialization runs once from `tx_application_define()` (`src/main.c`) and performs **no sensor I/O** (GPIO/I2C1 setup plus the mutex). The first sensor access happens lazily on `camera probe`

## The `camera` shell command

```
camera probe   power cycle + read the OV5640 chip ID (~1 s)
camera info    driver / sensor state
camera off     cut module power
```

Example session:

```
sh> camera probe
camera: probing OV5640 (takes ~1s) ...
OV5640 detected: chip ID 0x5640
sh> camera info
module:     B-CAMS-OMV (OV5640) on P1/DCMI, I2C1 @0x78
power:      on
chip ID:    0x5640
configured: no
frame:      none
sh> camera off
camera: power off
```

## References

- UM2779 — B-CAMS-OMV user manual (CN5 pinout, MB1379/X1 crystal, JP1)
- UM1907 §7.2 — P1 camera connector pinout
- RM0385 §30 — I2C (TIMINGR)
- `_ref/STM32Cube_FW_H7_V1.13.0/Drivers/BSP/STM32H747I-DISCO/stm32h747i_discovery_camera.c` — reference for the OV5640 power sequence / polarities
