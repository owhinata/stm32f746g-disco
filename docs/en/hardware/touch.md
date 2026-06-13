# Touch (FT5336 capacitive, I2C3)

The board's 4.3″ RK043FN48H panel carries a **FocalTech FT5336** capacitive touch controller. The driver lives in `port/touch/touch.{c,h}`, the shell command is `touch` (`shell/cmds/cmd_touch.c`).

This is **issue #54** of the LTDC + GUIX epic (#48), after the display bring-up ([LCD](display.md), #52). This phase covers **polled** multi-touch (up to 5 points): the `touch` command probes the chip ID and reads the active points so the wiring and the X/Y mapping can be verified on real hardware. EXTI-driven dispatch and GUIX input follow in #55.

## Configuration

| Item | Value | Source |
|------|-------|--------|
| Controller | FocalTech **FT5336** | UM1907 / ft5336.h |
| Bus | **I2C3** (PH7=SCL, PH8=SDA, AF4, open-drain) | UM1907 DISCOVERY I2C |
| Address | **0x70** (8-bit) | ft5336.h `FT5336_I2C_SLAVE_ADDRESS` |
| Register addresses | **8-bit** (`I2C_MEMADD_SIZE_8BIT`) | ft5336.h |
| Chip ID | reg `0xA8` = **0x51** | ft5336.h `FT5336_CHIP_ID_REG` / `FT5336_ID_VALUE` |
| Max points | **5** | ft5336.h `FT5336_MAX_DETECTABLE_TOUCH` |
| Coordinates | **panel pixels** (x 0..479 / y 0..271), **TS_SWAP_XY** applied | stm32746g_discovery_ts.c / hardware-confirmed |
| Speed | ~100 kHz standard mode | RM0385 §34 (I2C) |
| Mode | **polling** (INT pin PI13 unused) | this driver |

TIMINGR is the same value the camera's I2C1 uses, computed for **PCLK1 = 54 MHz** (PRESC=11 / SCLL=24 / SCLH=19 / SCLDEL=5 / SDADEL=2 → SCL ≈ 99 kHz). The ST BSP constant `0x40912732` assumes APB1 = 50 MHz and is deliberately not reused.

!!! note "Separate bus from the camera"
    The camera (OV5640) uses **I2C1** (PB8/PB9). The touch controller uses **I2C3** (PH7/PH8). The two are independent buses, so there is **no contention** between `camera` and `touch`.

## Reading touches

The FT5336 register map (ft5336.h):

- **TD_STATUS (0x02)** — low nibble = number of active touch points (0..5).
- Per point `n` a 6-byte block starting at `0x03 + 6·n`: `XH, XL, YH, YL, WEIGHT, MISC`.
  - `XH`: bits 7..6 = **event flag**, bits 3..0 = X position MSB.
  - `XL`: X position LSB → `rawx = ((XH & 0x0F) << 8) | XL` (12-bit).
  - `YH`: bits 7..4 = **touch-ID tag**, bits 3..0 = Y position MSB.
  - `YL`: Y position LSB → `rawy = ((YH & 0x0F) << 8) | YL` (12-bit).

The **event flag** is: `0` = press-down, `1` = lift-up, `2` = contact (held), `3` = no-event.

This panel needs **TS_SWAP_XY** (the controller's native axes are transposed relative to the LCD), so the assembled coordinates swap: `x = rawy`, `y = rawx` (matching `stm32746g_discovery_ts.c`). **The FT5336 on this panel reports panel-pixel coordinates directly** (x 0..479 / y 0..271, origin top-left), so **no scaling is applied** — confirmed on hardware by corner taps (top-left ≈(8,5) / top-right (479,5) / bottom-left (1,271) / bottom-right (479,271) / centre ≈(245,135)). GUIX (#55) owns any calibration it needs.

## Locking and thread context

- Public API calls serialize on an internal ThreadX mutex (TX_INHERIT).
- **Thread context only** — never call the API from an ISR.
- `touch_init()` runs once from `tx_application_define()` (`src/main.c`) and performs **no I2C I/O** (GPIO/I2C3 setup plus the mutex), so it is safe before the scheduler starts. The first bus transaction happens lazily on `touch probe` / `touch read`.

## The `touch` shell command

```
touch probe   read the FT5336 chip ID (0x51 = present)
touch info    bus / address / state / point count / mode
touch read    poll the active touch points until Ctrl+C (100 ms period)
```

`touch read` prints each active point as `P<id>: x=<x> y=<y> event=<e>`, where `<id>` is the FT5336 touch-ID tag. Press Ctrl+C to stop.

Example session:

```
sh> touch probe
FT5336 detected: chip ID 0x51
sh> touch read
polling FT5336 (Ctrl+C to stop) ...
P0: x=132 y=210 event=2
P0: x=133 y=211 event=2
^C
```

## Interrupt line (future)

The FT5336 INT output is wired to **PI13** (`EXTI15_10`). It is **not used** by this polling driver. EXTI-driven touch dispatch arrives with GUIX (#55), where a per-pin EXTI dispatcher is added.

!!! note "EXTI line shared with SD_DETECT"
    The microSD card-detect (SD_DETECT = **PC13**) and the touch INT (**PI13**) both land on EXTI line 13 (`EXTI15_10_IRQn`). SD_DETECT is currently **polled**, not EXTI-driven, so there is no conflict today; a future EXTI13 handler must dispatch by source pin.

## References

- RM0385 §34 (I2C)
- UM1907 (board user manual, DISCOVERY I2C / touch wiring)
- ft5336.h (FocalTech FT5336 register map, ST BSP component driver)
- stm32746g_discovery_ts.c (TS_SWAP_XY and coordinate assembly)
