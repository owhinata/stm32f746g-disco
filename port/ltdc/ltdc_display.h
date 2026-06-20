/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    ltdc_display.h
 * @brief   On-board RK043FN48H LCD bring-up via LTDC (issue #52/#53, Epic #48).
 *
 * Drives the board's 4.3" 480x272 RGB panel (Rocktech RK043FN48H-CT, UM1907
 * §6.10) through the STM32F746 LTDC controller.  Phase 1 of #48 (#52) brought
 * up a single RGB565 layer statically displayed from a SDRAM frame buffer to
 * verify the LCD wiring, RGB channel order and pixel clock; Phase 2 (#53) adds
 * DMA2D-accelerated drawing and a tear-free double buffer.  Touch (#54) and
 * GUIX (#55/#56) follow.
 *
 * Double buffer (#53): two RGB565 frame buffers live in SDRAM.  Drawing always
 * targets the *back* buffer (ltdc_back_buffer()); ltdc_flip() then makes it the
 * front (displayed) one with a tear-free swap -- HAL_LTDC_SetAddress_NoReload()
 * stages the new CFBAR, HAL_LTDC_Reload(VERTICAL_BLANKING) requests the
 * register reload at the next VSYNC, and the swap is committed only after the
 * hardware has actually reloaded (SRCR.VBR reads back 0, RM0385 §18.7.6 -- VBR
 * self-clears after the HW reload).  A reload-ready IRQ (LTDC_IRQHandler ->
 * HAL_LTDC_ReloadEventCallback) wakes ltdc_flip() promptly; the VBR poll is the
 * authoritative truth.  If the reload never lands within the timeout the
 * display is latched faulted (fail-closed): the front buffer is left unchanged
 * and ltdc_is_up() goes false.
 *
 * Drawing acceleration (#53): DMA2D (Chrom-ART) does register-to-memory fills
 * (ltdc_fill / ltdc_fill_rect / ltdc_colorbar) and memory-to-memory blits
 * (ltdc_blit).  DMA2D is an AHB master writing the same MPU non-cacheable SDRAM
 * as the CPU and the LTDC read DMA, so all three are coherent by construction.
 * The drawing + flip APIs serialize through ltdc_lock (a ThreadX mutex), so
 * concurrent callers never tear each other's frames.
 *
 * Clock: the LTDC pixel clock comes from PLLSAI, which is otherwise unused.
 * PLLSAI shares the main PLL's input divider M (=25), so VCO_in = HSE/M =
 * 25/25 = 1 MHz; with PLLSAIN=192 the PLLSAI VCO is 192 MHz (within the
 * 100..432 MHz range, RM0385 §5.3.24), /PLLSAIR(5) = 38.4 MHz PLLLCDCLK, and
 * /PLLSAIDIVR(8) = 4.8 MHz LCD_CLK (RM0385 §5.3.25 RCC_DCKCFGR1).  This is a
 * separate PLL from the main PLL, so it does NOT disturb SYSCLK (216 MHz) or
 * the FMC SDRAM clock (108 MHz).  LCD_CLK was lowered from the stock 9.6 MHz to
 * 4.8 MHz (~29.6 Hz; only PLLSAIDIVR doubled 4 -> 8) to relieve the LTDC's
 * continuous SDRAM read pressure and the DCMI DMA FIFO errors of #59 (measured
 * -51% preview FE) -- a DELIBERATE out-of-spec operating point (line period
 * 118 us > the RK043FN48H 65 us max, refresh < the ~50 Hz floor) validated on
 * hardware; in-spec fallback ~8.8 MHz / ~54 Hz (PLLSAIN=176).
 *
 * Memory: the frame buffer lives in the `.sdram` (NOLOAD) section -- the same
 * 8 MB region that bsp_init() maps Normal **non-cacheable** through the MPU, so
 * LTDC DMA reads and CPU writes are coherent by construction (no cache
 * maintenance).  ltdc_init() therefore REQUIRES sdram_init() to have succeeded;
 * it returns LTDC_ERR_STATE otherwise (the caller must not invoke it when the
 * FMC is down -- touching 0xC0000000 would fault).
 *
 * ltdc_init() only polls (HAL register waits, PLLSAI lock) -- it touches no
 * interrupt at run time -- and the only ThreadX work it does is *creating* the
 * flip semaphore + lock, which is exactly what tx_application_define() is for.
 * So it may still run from tx_application_define() before the scheduler starts.
 * (The HAL PLLSAI lock wait is HAL_GetTick()-based, and the SysTick handler
 * increments the HAL tick unconditionally -- tx_glue.c -- so the timeout works
 * there too.)  It enables the LTDC reload-ready IRQ (NVIC), but that vector
 * only fires once HAL_LTDC_Reload() arms it inside ltdc_flip() (post-scheduler).
 * Idempotent; later calls return the first result.
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

/* LCD_CLK fed to the panel: PLLSAI VCO(192) / R(5) / DIVR(8) = 4.8 MHz (#59). */
#define LTDC_PIXEL_CLOCK_HZ  4800000u

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
 * One-time bring-up: the flip semaphore + lock, PLLSAI -> LCD_CLK, LTDC + DMA2D
 * clocks, GPIO (AF14, plus PG12=AF9 and the manually-driven
 * LCD_DISP/LCD_BL_CTRL outputs), one RGB565 layer pointed at frame buffer 0, the
 * reload-ready IRQ (NVIC), then assert display-enable and backlight.  BOTH frame
 * buffers are cleared to black BEFORE the layer/backlight come on (the `.sdram`
 * section is NOLOAD = undefined at reset).  Requires sdram_is_up(); returns
 * LTDC_ERR_STATE otherwise.  Mostly polling -- safe from tx_application_define()
 * (the IRQ stays dormant until the first ltdc_flip()).  Idempotent; on failure
 * it cleans up (display off, HAL_LTDC_DeInit, objects deleted) and leaves
 * ltdc_is_up() false.
 */
int ltdc_init(void);

/** Nonzero once ltdc_init() succeeded AND the display has not latched a reload
 *  fault (see ltdc_flip()).  False means drawing/flip are no-ops. */
bool ltdc_is_up(void);

/** Base of the currently displayed (front) RGB565 frame buffer
 *  (LTDC_LCD_WIDTH*HEIGHT u16), or NULL when the display is down.  Row-major,
 *  one u16 per pixel, no padding.  READ-ONLY -- do NOT draw here (the LTDC is
 *  reading it); draw into ltdc_back_buffer() then ltdc_flip(). */
uint16_t *ltdc_framebuffer(void);

/** Base of the off-screen (back) RGB565 frame buffer to draw into, or NULL when
 *  the display is down.  The returned pointer is only valid while the caller
 *  holds ltdc_lock_frame() -- a flip from another thread swaps which buffer is
 *  "back".  Pair drawing with ltdc_flip() to present it. */
uint16_t *ltdc_back_buffer(void);

/** Index (0/1) of the currently displayed front buffer -- diagnostic only. */
uint8_t ltdc_active_buffer(void);

/*
 * Frame lock (ltdc_lock, a ThreadX mutex; recursive within one thread).  Hold
 * it around a draw-then-flip sequence so the back buffer cannot be swapped out
 * from under the caller and concurrent drawers do not tear each other.  The
 * individual draw helpers and ltdc_flip() take it internally too (the mutex is
 * reentrant), so a handler can wrap several of them in one lock for atomicity.
 * Thread-context only (waits on a ThreadX mutex); never call from an ISR.
 */
void ltdc_lock_frame(void);
void ltdc_unlock_frame(void);

/**
 * Tear-free present: stage the back buffer's address, request a vertical-
 * blanking reload, and commit the swap only once the hardware has reloaded
 * (SRCR.VBR reads 0).  Returns LTDC_OK on a successful swap; LTDC_ERR_STATE if
 * the display is down; LTDC_ERR_HAL if the reload never landed within the
 * timeout -- in which case the display is latched faulted (front unchanged,
 * ltdc_is_up() false; recovery is a system reset).  Thread-context only.
 */
int ltdc_flip(void);

/* ---- Display ownership interlock for GUIX (#55) -----------------------------
 * GUIX (port/guix) drives the same double buffer as the `lcd` command.  The
 * ltdc_lock mutex only serializes *individual* draws/flips; it does not stop the
 * shell from swapping ltdc_front out from under GUIX (whose canvas is bound to
 * ltdc_back_buffer()).  So while GUIX runs it *takes ownership*: with ownership
 * held, the public draw helpers (ltdc_fill/.../ltdc_gradient) and the public
 * ltdc_flip() become no-ops/refusals -- a backgrounded `lcd anim &` that takes
 * the lock after ownership was taken finds both its draw and its flip disabled,
 * so ltdc_front cannot move.  GUIX presents through the owner-only ltdc_gui_flip()
 * instead.  The flag is set/cleared under ltdc_lock, so it is atomic against an
 * in-flight draw helper (take() blocks until that helper releases the lock). */

/** Take (on=true) or release (on=false) GUIX ownership of the display.  Atomic
 *  with ltdc_lock.  Returns false if the display is down/faulted, or (for
 *  on=true) if scanout is disabled (#66) -- GUIX needs scanout running. */
bool ltdc_gui_take(bool on);

/** Nonzero while GUIX owns the display (the `lcd` draw/flip path is disabled). */
bool ltdc_gui_owns(void);

/** Owner-only tear-free present: same as ltdc_flip() but NOT gated on ownership
 *  (the public ltdc_flip() refuses while GUIX owns).  GUIX's buffer-toggle uses
 *  this.  Thread-context only; takes ltdc_lock (recursive, so safe when the
 *  caller already holds it). */
int ltdc_gui_flip(void);

/** Pixel clock actually programmed (LTDC_PIXEL_CLOCK_HZ); 0 when down. */
uint32_t ltdc_pixel_clock_hz(void);

/** Drive LCD_DISP (PI12) + LCD_BL_CTRL (PK3): true = display on + backlight. */
void ltdc_backlight(bool on);

/** Stop (false) / start (true) LTDC scanout at runtime (#66): clears/sets
 *  LTDC_GCR.LTDCEN so the controller stops/resumes fetching the framebuffer from
 *  SDRAM, and parks/raises the backlight.  Lets the LTDC's continuous SDRAM read
 *  be removed (e.g. to measure its contribution to the DCMI DMA FIFO errors of
 *  #59, or to save bandwidth/power when no display is needed).  Returns LTDC_OK,
 *  or LTDC_ERR_STATE if the display is down/faulted or GUIX owns it.  While
 *  disabled, ltdc_flip()/ltdc_gui_flip() refuse (so no VBR-timeout fault) and
 *  ltdc_gui_take(true) fails; the shell `lcd` draw commands refuse via lcd_ready().
 *  Direct ltdc_fill()/ltdc_blit() still write the back buffer but nothing is
 *  presented until re-enabled. */
int ltdc_set_scanout(bool on);

/** True when the LTDC is up but scanout is currently disabled (#66). */
bool ltdc_scanout_off(void);

/** True when the LTDC is up AND scanout is running (fetching the framebuffer
 *  from SDRAM).  This is the condition under which a 48 MHz (30 fps) DCMI burst
 *  overruns the 16-bit SDRAM, so the camera clamps to 24 MHz when it holds (#67).
 *  Distinct from !ltdc_scanout_off(): both are false when the LTDC never came up
 *  (no display = no contention), which is the correct "scanout not active" case. */
bool ltdc_scanout_active(void);

/** Read the LTDC FIFO-underrun / transfer-error status flags (sticky since the
 *  last clear).  These are set by hardware regardless of the interrupt enable
 *  (RM0385 §18.7.9), so they work without an LTDC IRQ: a non-zero FIFO-underrun
 *  bit is evidence of SDRAM-bandwidth starvation.  Returns a mask of
 *  LTDC_ERRFLAG_*; when @p clear is true the flags are cleared afterwards. */
uint32_t ltdc_errors(bool clear);

/* ---- Drawing into the back buffer (#53; DMA2D-accelerated where noted). -----
 * All of these draw into the back buffer only; they do NOT present.  Call
 * ltdc_flip() afterwards to make the result visible.  No-ops when down. */

/** Fill the whole back buffer with one RGB565 colour (DMA2D R2M). */
void ltdc_fill(uint16_t rgb565);

/** Fill an axis-aligned rectangle (clipped to the panel) with one RGB565 colour
 *  in the back buffer (DMA2D R2M). */
void ltdc_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                    uint16_t rgb565);

/** Copy a tightly-packed RGB565 source bitmap (w*h u16) into the back buffer at
 *  (x,y), clipped to the panel (DMA2D M2M). */
void ltdc_blit(const uint16_t *src, uint16_t x, uint16_t y,
               uint16_t w, uint16_t h);

/** Eight vertical colour bars (white/yellow/cyan/green/magenta/red/blue/black)
 *  into the back buffer -- verifies the RGB channel wiring and RGB565 bit order
 *  (DMA2D R2M per bar). */
void ltdc_colorbar(void);

/** DMA2D M2M self-copy demo: copy the back buffer's left half over its right
 *  half (a strided memory-to-memory blit within one frame buffer).  Exercises
 *  the M2M path on a non-tightly-packed source; draw a pattern (e.g.
 *  ltdc_colorbar) into the back buffer first, then ltdc_flip() to present. */
void ltdc_blit_demo(void);

/** Horizontal black-to-white gradient into the back buffer -- verifies
 *  pixel-clock/timing stability (banding or shimmer means a clock/porch
 *  problem).  CPU-drawn (per-column colour). */
void ltdc_gradient(void);

#ifdef __cplusplus
}
#endif

#endif /* LTDC_DISPLAY_H */
