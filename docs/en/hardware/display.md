# LCD (LTDC, 4.3″ 480×272 RGB)

The board's 4.3-inch RGB LCD (Rocktech **RK043FN48H-CT**, 480×272, capacitive touch) is driven by the STM32F746's built-in **LTDC** (LCD-TFT Display Controller). The driver lives in `port/ltdc/ltdc_display.{c,h}`, the shell command is `lcd` (`shell/cmds/cmd_lcd.c`).

This started as **Phase 1** (#52) of the LTDC + GUIX epic (#48) — a single RGB565 layer with a static display. **Phase 2** (#53) adds **DMA2D-accelerated drawing and a tear-free double buffer** (see the "Double buffer + DMA2D + tear-free" section below). The `lcd` command paints solid colours, colour bars, a gradient, an animation and a blit to verify the **wiring / RGB channel order / pixel clock / DMA2D / tear-free swap** on real hardware. Touch (#54) and GUIX (#55/#56) follow. Touch is not implemented here.

## Configuration

| Item | Value | Source |
|------|-------|--------|
| Panel | RK043FN48H-CT (480×272, 24-bit RGB, capacitive touch) | UM1907 / rk043fn48h.h |
| Pixel format | **RGB565** (16 bpp) | LTDC layer 0 |
| Frame buffer | 480×272×2 × **2 buffers** = **522240 B (~510 KB)**, in `.sdram` | non-cacheable, double-buffer |
| LCD_CLK | **9.6 MHz** (PLLSAI) | derivation below |
| Frame rate | 565×285 clk @ 9.6 MHz ≈ **59.3 Hz** (~16.9 ms/frame) | timings below |
| Timings (spec) | HSYNC=41 / HBP=13 / HFP=32 / VSYNC=10 / VBP=2 / VFP=2 | rk043fn48h.h |
| Polarity | HS / VS / DE = **active-low**, PC = **IPC** (not inverted) | ST BSP / RM0385 §18.7.5 |
| Drawing | **DMA2D** (R2M fill / M2M blit, polled to completion) | RM0385 §9 |
| Interrupt | **reload-ready only** (`LTDC_IT_RR`, prio 9); underrun/transfer error polled | RM0385 §18.7.9 |

The values programmed into LTDC are each **spec minus one** (`HorizontalSync=40 / VerticalSync=9 / AccumulatedHBP=53 / AccumulatedVBP=11 / AccumulatedActiveW=533 / AccumulatedActiveH=283 / TotalWidth=565 / TotalHeigh=285`).

## Clock: PLLSAI → LCD_CLK 9.6 MHz

The LTDC pixel clock comes from **PLLSAI**, previously unused. PLLSAI **shares the main PLL's input divider M (=25)** (RM0385 §5.3.8), so:

```
VCO_in    = HSE / M       = 25 MHz / 25 = 1 MHz
PLLSAI VCO = VCO_in × PLLSAIN(192)   = 192 MHz   (within 100..432 MHz, RM0385 §5.3.24)
PLLLCDCLK = VCO / PLLSAIR(5)          = 38.4 MHz
LCD_CLK   = PLLLCDCLK / PLLSAIDIVR(4) = 9.6 MHz   (RM0385 §5.3.25 RCC_DCKCFGR1)
```

PLLSAI is a **separate PLL from the main PLL**, so configuring it via `HAL_RCCEx_PeriphCLKConfig(RCC_PERIPHCLK_LTDC)` **does not disturb SYSCLK (216 MHz) or the FMC SDRAM clock (108 MHz)**.

!!! note "Why it is safe from tx_application_define (the HAL tick)"
    The HAL PLLSAI lock wait is `HAL_GetTick()`-based (100 ms timeout). This project's `SysTick_Handler` calls `HAL_IncTick()` **unconditionally**, regardless of whether the ThreadX timer is running (`port/threadx/tx_glue.c`), so the HAL tick advances even inside `tx_application_define` before the scheduler starts, and a failed lock still times out. `sdram_init()` uses a TIM2 busy-wait because the *ThreadX* timer tick is a different thing; the HAL tick is separate.

## Pins

LTDC pins are **AF14 except PG12 (AF9)** (UM1907 / ST BSP):

`PE4 / PG12(AF9) / PI9,10,14,15 / PJ0-11,13-15 / PK0,1,2,4,5,6,7`

Plus two manually-driven GPIO outputs: **LCD_DISP = PI12** and **LCD_BL_CTRL = PK3** (display enable + backlight), asserted together at the end of `ltdc_init()`. No pin clash with the existing peripherals (VCP=PA9/PB7, LED=PI1, camera DCMI/I2C1, SDMMC1, QSPI, FMC SDRAM) — PI9-15 are different pins from the LED's PI1.

## Frame buffer: `.sdram` (NOLOAD) + MPU non-cacheable

The frame buffer lives in the same **`.sdram` (NOLOAD) section** as the camera frames. `bsp_init()` already remaps the whole 8 MB through MPU region 0 as **Normal, non-cacheable**, so the **LTDC read DMA and the CPU writes are coherent** (no cache maintenance). See [SDRAM](sdram.md).

```c
static uint16_t ltdc_fb[2][480 * 272] __attribute__((aligned(32), section(".sdram")));
```

Double buffering (#53) uses **two buffers = ~510 KB**. Current `.sdram` occupancy (does not survive reset): `cam_frame` 150 KB + `cam_ring[4]` 600 KB + `ltdc_fb[2]` ~510 KB ≈ **1.26 MB / 8 MB**. DMA2D is also an AHB master reading/writing this region, but it stays coherent with the CPU and LTDC because the region is MPU non-cacheable.

## Initialization flow

`ltdc_init()` (from `tx_application_define`, **only when `sdram_init()` succeeded**):

0. **SDRAM guard**: return `LTDC_ERR_STATE` immediately unless `sdram_is_up()`. The frame buffer is in `.sdram` (0xC0000000); touching it with the FMC down would fault, so the caller gates it too (defence in depth).
1. **Create ThreadX objects**: the reload semaphore + the `ltdc_lock` mutex (creating them in `tx_application_define` is the normal use; no interrupt fires at run time)
2. PLLSAI → LCD_CLK 9.6 MHz (`HAL_RCCEx_PeriphCLKConfig`)
3. LTDC clock + GPIO (AF + DISP/BL outputs, parked off for now)
4. **Clear BOTH buffers to black** (`.sdram` is NOLOAD = undefined; clearing before ConfigLayer / backlight-on prevents a garbage front frame and a garbage first flip). `ltdc_front=0`
5. `HAL_LTDC_Init` (timings / polarity / black background above)
6. `HAL_LTDC_ConfigLayer` (layer 0, RGB565, 480×272, FB = `ltdc_fb[0]`)
7. **Enable the reload-ready IRQ** in the NVIC (prio 9). It never fires until `HAL_LTDC_Reload()` arms it.
8. **Finally** assert LCD_DISP / LCD_BL_CTRL (display + backlight on)

Mostly polling (only object creation and the NVIC setup besides). Idempotent and fail-soft (on failure it cleans up — DISP/BL off + `HAL_LTDC_DeInit` + delete the objects — the `lcd` command reports it, everything else keeps running).

## Double buffer + DMA2D + tear-free (#53)

Phase 2 (#53) adds **DMA2D drawing** and a **tear-free double buffer**.

### Two frame buffers

There are **two** RGB565 frame buffers (`ltdc_fb[2][480*272]`, both in `.sdram` = **~510 KB** total). Drawing always targets the **back** (off-screen) buffer; `ltdc_flip()` then swaps it to the **front** (displayed) buffer tear-free.

```c
static uint16_t ltdc_fb[2][480 * 272] __attribute__((aligned(32), section(".sdram")));
```

- `ltdc_framebuffer()` — the currently displayed front buffer (**read-only**; do not draw here)
- `ltdc_back_buffer()` — the back buffer to draw into (valid only while `ltdc_lock_frame()` is held)
- `ltdc_lock_frame()` / `ltdc_unlock_frame()` — serialize draw→flip through `ltdc_lock` (a ThreadX mutex, recursively acquirable by one thread)

### DMA2D (Chrom-ART)

Drawing is accelerated by the built-in **DMA2D**. As an AHB master it reads/writes the **same MPU non-cacheable `.sdram`** as the CPU and the LTDC read DMA, so all three are coherent with no cache maintenance.

- **R2M solid fill** — `ltdc_fill` / `ltdc_fill_rect` / `ltdc_colorbar` (one fill per bar)
- **M2M blit** — `ltdc_blit` (copy a tightly-packed RGB565 bitmap into the back buffer) / `ltdc_blit_demo` (strided self-copy of the back buffer's left half over its right half)
- The gradient (`ltdc_gradient`) is per-column, so the CPU draws it directly

Each op follows the same ST BSP `LL_FillBuffer` / `LL_ConvertLineToARGB8888` idiom: per op `HAL_DMA2D_Init` → `ConfigLayer` → `Start` → `PollForTransfer(30 ms)`.

### Tear-free flip (VBR authoritative + fail-closed)

`ltdc_flip()` (under `ltdc_lock`):

1. Stage the back buffer address into CFBAR with `HAL_LTDC_SetAddress_NoReload()` (not applied yet)
2. `HAL_LTDC_Reload(LTDC_RELOAD_VERTICAL_BLANKING)` requests the **register reload at the next vertical blanking** (and arms the reload-ready interrupt `LTDC_IT_RR`)
3. Wait on the reload-ready interrupt (`LTDC_IRQHandler` → `HAL_LTDC_ReloadEventCallback` → `ltdc_reload_sem`) as a wake-up hint (up to 100 ms)
4. **Authoritative check**: the hardware reload self-clears `LTDC->SRCR.VBR` **only after** it commits the reload (RM0385 §18.7.6). The interrupt is only a hint; **VBR==0 is authoritative**, confirmed by polling for up to ~100 ms.
   - VBR==0 → swap `ltdc_front` to the back buffer (commit), return `LTDC_OK`
   - timeout with VBR==1 → **fail-closed**: leave the front buffer unchanged, latch `ltdc_fault` (so `ltdc_is_up()` becomes false), return `LTDC_ERR_HAL`. Recovery is a system reset.

The LTDC frame period is 565×285 clk @ 9.6 MHz ≈ **59.3 Hz** (~16.9 ms/frame), so 100 ms is several frames of slack. The interrupt priority is **9** (below DCMI/DMA2=8, no clash with SD=6-7 / USART=5).

## The `lcd` shell command

```
lcd info            panel / clock / frame buffer / buffers / DMA2D / LTDC error flags
lcd fill <color>    flood the screen (colour name black/blue/green/cyan/red/magenta/yellow/white, or 0xRGB565)
lcd bar             8 vertical colour bars (RGB wiring / bit-order check)
lcd grad            horizontal gradient (black→white, pixel-clock check)
lcd clear           fill black
lcd anim            bouncing rectangle (tear-free double-buffer demo; Ctrl+C to stop)
lcd blit            DMA2D M2M demo (copy the colour bars' left half over the right)
lcd on | lcd off    display enable + backlight
```

The drawing commands (fill/bar/grad/clear/anim/blit) **draw into the back buffer then present with `ltdc_flip()`** (one atomic frame under `ltdc_lock_frame()`/`ltdc_unlock_frame()`). `lcd anim` needs no explicit sleep — `ltdc_flip()` blocks until the VSYNC reload, so the loop is **naturally paced to the frame rate** (it polls Ctrl+C once per frame).

The **errors line** of `lcd info` shows the `LTDC->ISR` FIFO-underrun / transfer-error flags (RM0385 §18.7.9) — **underrun=YES is evidence of SDRAM bandwidth starvation**.

Example session:

```
sh> lcd info
panel:   RK043FN48H-CT 480x272 RGB565 (LTDC layer 0)
clock:   LCD_CLK 9.60 MHz (PLLSAI N=192 R=5, DIVR/4)
fb:      0xc00bb800 (.sdram, non-cacheable)
buffers: 2 (double, tear-free VBR)
front:   0
DMA2D:   on
state:   up
errors:  underrun=no transfer=no
sh> lcd bar
sh> lcd anim
^C
sh> lcd fill red
```

## Bandwidth

The LTDC continuously reads **only the front buffer** during the active period, every frame (double buffering does not increase the LTDC read bandwidth — it still fetches one buffer). At RGB565 and a 9.6 MHz pixel clock the upper bound is **9.6 Mpix/s × 2 B = 19.2 MB/s** (≈15.6 MB/s effective for active fetch only). Against the SDRAM's theoretical bandwidth (16-bit @ 108 MHz = 216 MB/s) that is about 9%. The DMA2D fills/blits are short write bursts to the back buffer (at most ~510 KB per drawn frame), and the CPU waits on `PollForTransfer`, so they contend with the LTDC read only briefly. There is headroom alongside DCMI writes (QVGA ~11 fps ≈ 1.65 MB/s) and the CPU (verify on hardware: `lcd anim`/`blit`, `camera capture`/`stream` regress cleanly, underrun=no). Re-evaluate with higher resolution/fps.

## References

- UM1907 — RK043FN48H-CT LCD (LTDC RGB wiring, LCD_DISP/LCD_BL_CTRL)
- RM0385 §18 — LTDC, §5 (RCC / PLLSAI)
- rk043fn48h.h — panel timing constants (resolution / HSYNC / porch / divider)
- `_ref/STM32Cube_FW_F7_V1.17.0/Drivers/BSP/STM32746G-Discovery/stm32746g_discovery_lcd.c` — reference implementation for LTDC / PLLSAI / GPIO init (read-only)
