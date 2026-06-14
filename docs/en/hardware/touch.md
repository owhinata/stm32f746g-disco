# Touch (FT5336 capacitive, I2C3)

The board's 4.3″ RK043FN48H panel carries a **FocalTech FT5336** capacitive touch controller. The driver lives in `port/touch/touch.{c,h}`, the shell command is `touch` (`shell/cmds/cmd_touch.c`).

This is **issue #54** of the LTDC + GUIX epic (#48), after the display bring-up ([LCD](display.md), #52): the `touch` command probes the chip ID and reads the active points so the wiring and the X/Y mapping can be verified on real hardware. **#62** then made the FT5336 INT (PI13) **EXTI13 interrupt-driven** and the I2C reads **interrupt-driven (IT)**, so the GUIX input thread idles on the interrupt (CPU ≈ 0 %) when nothing is touched (see below).

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
| Mode | **EXTI13 interrupt-driven + I2C IT** (INT = PI13, #62) | this driver |

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
- I2C reads/writes are **interrupt-driven (IT)** (`HAL_I2C_Mem_Read_IT` / `_Write_IT` + a completion semaphore, #62): the caller blocks without busy-waiting for the duration of the transaction. The Rx/Tx/error completion callbacks are weak symbols shared across all I2C units, so they **filter on `Instance == I2C3`** (the camera's I2C1 is blocking and never drives them). With no synchronous abort available, a timeout/error is recovered with `HAL_I2C_DeInit`+`Init`.
- `touch_init()` runs once from `tx_application_define()` (`src/main.c`) and performs **no I2C I/O** (GPIO/I2C3 setup, the mutex, the semaphores, and the I2C3 NVIC), so it is safe before the scheduler starts. The first bus transaction happens lazily on `touch probe` / `touch read` / `touch_irq_enable()`.

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

## EXTI13 interrupt-driven + I2C IT (#62)

The GUIX input poll (`port/guix/guix_touch.c`) used to hit the bus at 60 Hz even with nothing touched, busy-waiting inside the blocking `HAL_I2C_Mem_Read`, so it burned **CPU ≈ 20 % at idle**. #62 removes that in two steps.

**1. EXTI13 interrupt wake.** The FT5336 INT output is wired to **PI13** (`EXTI15_10`). `touch_irq_enable()` arms PI13 as a rising-edge EXTI and puts the FT5336 in **interrupt (trigger) mode** (GMODE reg `0xA4` = `0x01`) so it drives INT on touch. The order mirrors the ST BSP (`stm32746g_discovery_ts.c`): **arm the GPIO/NVIC first, write GMODE last**, so no edge is lost in the enable→arm window. The EXTI ISR never calls `gx_system_event_send` (GUIX is thread-context only) — it just posts a wake semaphore.

**2. Hybrid state machine.** The GUIX input thread waits on that semaphore with `TX_WAIT_FOREVER` when no finger is down (**CPU ≈ 0 %**). An INT edge wakes it, and **only while a finger stays down does it poll at ~60 Hz** to emit DOWN/DRAG/UP (the controller emits no edges during sustained contact, so polling is required to follow a drag). On release it returns to the semaphore wait. A short post-wake settle poll catches the FT5336's `0xFFF/0xFFF` invalid first sample so a press is never dropped. `gui stop` (park) disarms the wake and kicks the thread out of the semaphore wait.

!!! note "EXTI line shared with SD_DETECT — exclusive mux"
    The microSD card-detect (SD_DETECT = **PC13**) and the touch INT (**PI13**) both land on EXTI line 13 (`EXTI15_10_IRQn`). `SYSCFG_EXTICR4` maps line 13 to **exactly one port** (PC13 and PI13 **cannot both** use it). SD_DETECT is currently **polled** (`port/sd/sd_card.c`), so PI13 owns line 13; moving SD_DETECT to EXTI13 in the future would require resolving this mux (keep one of them polled).

The `touch read` shell command still polls on its own 100 ms cadence (each read is IT-backed internally).

## References

- RM0385 §34 (I2C)
- UM1907 (board user manual, DISCOVERY I2C / touch wiring)
- ft5336.h (FocalTech FT5336 register map, ST BSP component driver)
- stm32746g_discovery_ts.c (TS_SWAP_XY and coordinate assembly)
