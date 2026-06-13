# LCD (LTDC, 4.3″ 480×272 RGB)

The board's 4.3-inch RGB LCD (Rocktech **RK043FN48H-CT**, 480×272, capacitive touch) is driven by the STM32F746's built-in **LTDC** (LCD-TFT Display Controller). The driver lives in `port/ltdc/ltdc_display.{c,h}`, the shell command is `lcd` (`shell/cmds/cmd_lcd.c`).

This is **Phase 1** (#52) of the LTDC + GUIX epic (#48). This phase covers **a single RGB565 layer with a static display** (the LTDC continuously reads a SDRAM frame buffer). The `lcd` command paints solid colours, colour bars and a gradient to verify the **wiring / RGB channel order / pixel clock** on real hardware. DMA2D + double buffering (#53), touch (#54) and GUIX (#55/#56) follow. Touch is not implemented here.

## Configuration

| Item | Value | Source |
|------|-------|--------|
| Panel | RK043FN48H-CT (480×272, 24-bit RGB, capacitive touch) | UM1907 / rk043fn48h.h |
| Pixel format | **RGB565** (16 bpp) | LTDC layer 0 |
| Frame buffer | 480×272×2 = **261120 B (255 KB)**, in `.sdram` | non-cacheable |
| LCD_CLK | **9.6 MHz** (PLLSAI) | derivation below |
| Timings (spec) | HSYNC=41 / HBP=13 / HFP=32 / VSYNC=10 / VBP=2 / VFP=2 | rk043fn48h.h |
| Polarity | HS / VS / DE = **active-low**, PC = **IPC** (not inverted) | ST BSP / RM0385 §18.7.5 |
| Interrupt | **none** (static display); underrun/transfer error are polled | RM0385 §18.7.9 |

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
static uint16_t ltdc_fb[480 * 272] __attribute__((aligned(32), section(".sdram")));
```

Current `.sdram` occupancy (does not survive reset): `cam_frame` 150 KB + `cam_ring[4]` 600 KB + `ltdc_fb` 255 KB ≈ **1.0 MB / 8 MB** (the on-target map shows SDRAM 12.27% used; the FB sits at `0xC00BB800`).

## Initialization flow

`ltdc_init()` (from `tx_application_define`, **only when `sdram_init()` succeeded**):

0. **SDRAM guard**: return `LTDC_ERR_STATE` immediately unless `sdram_is_up()`. The frame buffer is in `.sdram` (0xC0000000); touching it with the FMC down would fault, so the caller gates it too (defence in depth).
1. PLLSAI → LCD_CLK 9.6 MHz (`HAL_RCCEx_PeriphCLKConfig`)
2. LTDC clock + GPIO (AF + DISP/BL outputs, parked off for now)
3. **Clear the frame buffer to black** (`.sdram` is NOLOAD = undefined; clearing before ConfigLayer / backlight-on prevents a garbage frame at boot)
4. `HAL_LTDC_Init` (timings / polarity / black background above)
5. `HAL_LTDC_ConfigLayer` (layer 0, RGB565, 480×272, FB = `ltdc_fb`)
6. **Finally** assert LCD_DISP / LCD_BL_CTRL (display + backlight on)

Polling only (no interrupts, DMA, or ThreadX objects). Idempotent and fail-soft (on failure it cleans up — DISP/BL off + `HAL_LTDC_DeInit` — the `lcd` command reports it, everything else keeps running).

## The `lcd` shell command

```
lcd info            panel / clock / frame buffer / LTDC error flags
lcd fill <color>    flood the screen (colour name black/blue/green/cyan/red/magenta/yellow/white, or 0xRGB565)
lcd bar             8 vertical colour bars (RGB wiring / bit-order check)
lcd grad            horizontal gradient (black→white, pixel-clock check)
lcd clear           fill black
lcd on | lcd off    display enable + backlight
```

Phase 1 draws with the CPU (`.sdram` is non-cacheable). The **errors line** of `lcd info` shows the `LTDC->ISR` FIFO-underrun / transfer-error flags (RM0385 §18.7.9) — even without an LTDC interrupt, **underrun=YES is evidence of SDRAM bandwidth starvation**.

Example session:

```
sh> lcd info
panel:   RK043FN48H-CT 480x272 RGB565 (LTDC layer 0)
clock:   LCD_CLK 9.60 MHz (PLLSAI N=192 R=5, DIVR/4)
fb:      0xc00bb800 (.sdram, non-cacheable)
state:   up
errors:  underrun=no transfer=no
sh> lcd bar
sh> lcd fill red
```

## Bandwidth

The LTDC continuously reads the frame buffer during the active period, every frame. At RGB565 and a 9.6 MHz pixel clock the upper bound is **9.6 Mpix/s × 2 B = 19.2 MB/s** (≈15.6 MB/s effective for active fetch only). Against the SDRAM's theoretical bandwidth (16-bit @ 108 MHz = 216 MB/s) that is about 9%, leaving headroom alongside DCMI writes (QVGA ~11 fps ≈ 1.65 MB/s) and the CPU (verified on hardware: `camera capture`/`stream` regress cleanly, underrun=no). Re-evaluate with double buffering and higher resolution/fps (#53 onward).

## References

- UM1907 — RK043FN48H-CT LCD (LTDC RGB wiring, LCD_DISP/LCD_BL_CTRL)
- RM0385 §18 — LTDC, §5 (RCC / PLLSAI)
- rk043fn48h.h — panel timing constants (resolution / HSYNC / porch / divider)
- `_ref/STM32Cube_FW_F7_V1.17.0/Drivers/BSP/STM32746G-Discovery/stm32746g_discovery_lcd.c` — reference implementation for LTDC / PLLSAI / GPIO init (read-only)
