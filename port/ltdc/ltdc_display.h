/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    ltdc_display.h
 * @brief   On-board RK043FN48H LCD bring-up via LTDC (issue #52, Epic #48).
 *
 * Drives the board's 4.3" 480x272 RGB panel (Rocktech RK043FN48H-CT, UM1907
 * §6.10) through the STM32F746 LTDC controller.  Phase 1 of #48: a single
 * layer, RGB565, statically displayed from a SDRAM frame buffer -- enough to
 * verify the LCD wiring, RGB channel order and pixel clock with the `lcd`
 * command before adding DMA2D / double-buffering (#53), touch (#54) and GUIX
 * (#55/#56).
 *
 * Clock: the LTDC pixel clock comes from PLLSAI, which is otherwise unused.
 * PLLSAI shares the main PLL's input divider M (=25), so VCO_in = HSE/M =
 * 25/25 = 1 MHz; with PLLSAIN=192 the PLLSAI VCO is 192 MHz (within the
 * 100..432 MHz range, RM0385 §5.3.24), /PLLSAIR(5) = 38.4 MHz PLLLCDCLK, and
 * /PLLSAIDIVR(4) = 9.6 MHz LCD_CLK (RM0385 §5.3.25 RCC_DCKCFGR1).  This is a
 * separate PLL from the main PLL, so it does NOT disturb SYSCLK (216 MHz) or
 * the FMC SDRAM clock (108 MHz).
 *
 * Memory: the frame buffer lives in the `.sdram` (NOLOAD) section -- the same
 * 8 MB region that bsp_init() maps Normal **non-cacheable** through the MPU, so
 * LTDC DMA reads and CPU writes are coherent by construction (no cache
 * maintenance).  ltdc_init() therefore REQUIRES sdram_init() to have succeeded;
 * it returns LTDC_ERR_STATE otherwise (the caller must not invoke it when the
 * FMC is down -- touching 0xC0000000 would fault).
 *
 * ltdc_init() polls (HAL register waits, PLLSAI lock) and uses no interrupts or
 * ThreadX objects, so it may run from tx_application_define() before the
 * scheduler starts.  (The HAL PLLSAI lock wait is HAL_GetTick()-based, and the
 * SysTick handler increments the HAL tick unconditionally -- tx_glue.c -- so
 * the timeout works there too.)  Idempotent; later calls return the first
 * result.  Phase 1 uses no LTDC interrupt (static display).
 *
 * Clean-room implementation; the ST BSP (stm32746g_discovery_lcd.c,
 * rk043fn48h.h) and RM0385 §18 (LTDC) / §5 (RCC/PLLSAI) were used as a
 * register/pin/timing reference only.
 */
#ifndef LTDC_DISPLAY_H
#define LTDC_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error returns (negative); 0 is success. */
#define LTDC_OK          0
#define LTDC_ERR_HAL    -1   /* a HAL init step (PLLSAI/LTDC/layer) failed   */
#define LTDC_ERR_STATE  -2   /* SDRAM is down, or ltdc_init() did not succeed */

/* RK043FN48H panel geometry. */
#define LTDC_LCD_WIDTH   480u
#define LTDC_LCD_HEIGHT  272u

/* LCD_CLK fed to the panel: PLLSAI VCO(192) / R(5) / DIVR(4) = 9.6 MHz. */
#define LTDC_PIXEL_CLOCK_HZ  9600000u

/* Handy RGB565 constants for the `lcd` command. */
#define LTDC_RGB565_BLACK    0x0000u
#define LTDC_RGB565_BLUE     0x001Fu
#define LTDC_RGB565_GREEN    0x07E0u
#define LTDC_RGB565_CYAN     0x07FFu
#define LTDC_RGB565_RED      0xF800u
#define LTDC_RGB565_MAGENTA  0xF81Fu
#define LTDC_RGB565_YELLOW   0xFFE0u
#define LTDC_RGB565_WHITE    0xFFFFu

/* LTDC error-flag bits returned by ltdc_errors() (RM0385 §18.7.9 LTDC_ISR). */
#define LTDC_ERRFLAG_FIFO_UNDERRUN  0x1u   /* FUIF  */
#define LTDC_ERRFLAG_TRANSFER_ERROR 0x2u   /* TERRIF */

/**
 * One-time bring-up: PLLSAI -> LCD_CLK, LTDC + GPIO (AF14, plus PG12=AF9 and
 * the manually-driven LCD_DISP/LCD_BL_CTRL outputs), one RGB565 layer pointed
 * at the SDRAM frame buffer, then assert display-enable and backlight.  The
 * frame buffer is cleared to black BEFORE the layer/backlight come on (the
 * `.sdram` section is NOLOAD = undefined at reset).  Requires sdram_is_up();
 * returns LTDC_ERR_STATE otherwise.  Polling only -- safe from
 * tx_application_define().  Idempotent; on failure it cleans up (display off,
 * HAL_LTDC_DeInit) and leaves ltdc_is_up() false.
 */
int ltdc_init(void);

/** Nonzero once ltdc_init() succeeded (the panel is live). */
bool ltdc_is_up(void);

/** Base of the active RGB565 frame buffer (LTDC_LCD_WIDTH*HEIGHT u16), or NULL
 *  when the display is down.  Row-major, one u16 per pixel, no padding. */
uint16_t *ltdc_framebuffer(void);

/** Pixel clock actually programmed (LTDC_PIXEL_CLOCK_HZ); 0 when down. */
uint32_t ltdc_pixel_clock_hz(void);

/** Drive LCD_DISP (PI12) + LCD_BL_CTRL (PK3): true = display on + backlight. */
void ltdc_backlight(bool on);

/** Read the LTDC FIFO-underrun / transfer-error status flags (sticky since the
 *  last clear).  These are set by hardware regardless of the interrupt enable
 *  (RM0385 §18.7.9), so they work without an LTDC IRQ: a non-zero FIFO-underrun
 *  bit is evidence of SDRAM-bandwidth starvation.  Returns a mask of
 *  LTDC_ERRFLAG_*; when @p clear is true the flags are cleared afterwards. */
uint32_t ltdc_errors(bool clear);

/* ---- Phase 1 CPU-drawn test patterns (FB is non-cacheable; no DMA2D yet). -- */

/** Fill the whole frame buffer with one RGB565 colour. */
void ltdc_fill(uint16_t rgb565);

/** Fill an axis-aligned rectangle (clipped to the panel) with one colour. */
void ltdc_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                    uint16_t rgb565);

/** Eight vertical colour bars (white/yellow/cyan/green/magenta/red/blue/black)
 *  -- verifies the RGB channel wiring and RGB565 bit order. */
void ltdc_colorbar(void);

/** Horizontal black-to-white gradient -- verifies pixel-clock/timing stability
 *  (banding or shimmer means a clock/porch problem). */
void ltdc_gradient(void);

#ifdef __cplusplus
}
#endif

#endif /* LTDC_DISPLAY_H */
