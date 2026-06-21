/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    touch.h
 * @brief   FT5336 capacitive touch panel over I2C3 (issue #54, Epic #48).
 *
 * Reads the FocalTech FT5336 controller behind the board's 4.3" RK043FN48H
 * panel: chip-ID probe and a polled multi-touch read (up to 5 simultaneous
 * points).  Phase of #48 after the LTDC display (#52): enough to verify the
 * touch wiring and the X/Y mapping with the `touch` command before EXTI-driven
 * dispatch and GUIX input (#55/#56).
 *
 * Hardware facts (UM1907 / ST BSP):
 *   - Bus: I2C3, SCL=PH7 / SDA=PH8 (AF4, open-drain), the board's shared
 *     "DISCOVERY" I2C used by the audio codec and the touch controller.  This
 *     is a SEPARATE bus from the camera's I2C1 (PB8/PB9), so the two never
 *     contend.
 *   - Address: 0x70 (8-bit, the HAL Mem API form).
 *   - INT: FT5336_INT = PI13 (EXTI15_10).  touch_irq_enable() puts the FT5336 in
 *     interrupt (trigger) mode and arms PI13 as a rising-edge EXTI so a touch
 *     wakes a waiter (touch_wait_event) -- the GUIX input path uses this to idle
 *     at ~0 % CPU instead of polling (issue #62).
 *
 * Concurrency: public calls serialize on an internal TX_MUTEX; the API is
 * thread-context only (never from an ISR).  I2C reads/writes are interrupt-driven
 * (HAL_I2C_Mem_Read_IT / _Write_IT + a completion semaphore), so a transaction
 * blocks the caller without busy-waiting.  touch_init() does NO I2C I/O, so it is
 * safe to call from tx_application_define() before the scheduler runs; the first
 * bus transaction happens lazily on touch_probe()/touch_read()/touch_irq_enable().
 *
 * Clean-room implementation; the ST BSP component driver (ft5336.h) and the
 * STM32746G-Discovery BSP (stm32746g_discovery_ts.c, for TS_SWAP_XY and the
 * coordinate assembly) were used as a register / mapping reference only.
 */
#ifndef TOUCH_H
#define TOUCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error returns (negative); 0 is success. */
#define TOUCH_OK          0
#define TOUCH_ERR_HAL    -1   /* a HAL I2C transaction failed                 */
#define TOUCH_ERR_STATE  -2   /* touch_init() did not bring the bus up        */
#define TOUCH_ERR_ID     -3   /* wrong / no chip ID at 0x70 (panel absent?)   */
#define TOUCH_ERR_TIMEOUT -4  /* an I2C-IT xfer / EXTI wait timed out         */

/* FT5336 reports at most 5 simultaneous touch points. */
#define TOUCH_MAX_POINTS  5

/* Panel pixel extent (RK043FN48H 480x272).  A reported point outside this is the
   FT5336 "not touched" sentinel (all-ones, 0xFFF/0xFFF) -- touch_read() drops it
   so callers never see a phantom point. */
#define TOUCH_PANEL_W  480
#define TOUCH_PANEL_H  272

/**
 * One touch point.  Coordinates are PANEL PIXELS: x 0..479, y 0..271, origin
 * top-left, with the TS_SWAP_XY mapping this panel needs applied (the
 * controller's native axes are transposed relative to the LCD).  The FT5336 on
 * this board reports pixel coordinates directly (no 0..4095 scaling needed) --
 * confirmed on hardware by corner taps (top-left ~(8,5), bottom-right (479,271),
 * centre ~(245,135)).  @ref event is the FT5336 touch-event flag:
 *   0 = press-down, 1 = lift-up, 2 = contact (held), 3 = no-event.
 * @ref id is the FT5336 touch-ID tag (0..F) that follows a finger across reads.
 */
struct touch_point {
	uint16_t x;       /* panel X 0..479 (TS_SWAP_XY applied)    */
	uint16_t y;       /* panel Y 0..271 (TS_SWAP_XY applied)    */
	uint8_t  event;   /* 0 down / 1 up / 2 contact / 3 no-event */
	uint8_t  id;      /* FT5336 touch-ID tag                    */
};

/** Snapshot of the active touch points from one touch_read(). */
struct touch_state {
	uint8_t            count;                /* 0..TOUCH_MAX_POINTS */
	struct touch_point p[TOUCH_MAX_POINTS];
};

/**
 * One-time bring-up: I2C3 (PH7/PH8 AF4, ~100 kHz) and the operation mutex.
 * Performs **no I2C I/O**, so it is safe to call from tx_application_define()
 * before the scheduler runs.  Idempotent: a second call returns 0 without
 * re-doing setup.  Returns TOUCH_ERR_HAL (fail-soft) if the HAL init fails.
 */
int touch_init(void);

/** Nonzero once touch_init() brought the I2C3 bus up (the bus is usable). */
bool touch_is_up(void);

/**
 * Read the FT5336 chip-ID register (0xA8) and verify it is 0x51.  On success
 * *id (may be NULL) receives the raw ID byte.  Returns TOUCH_ERR_STATE if the
 * bus is down, TOUCH_ERR_HAL on an I2C error, TOUCH_ERR_ID on a wrong value.
 */
int touch_probe(uint8_t *id);

/**
 * Poll the FT5336 for the current touch points into @p st.  Reads the
 * touch-data-status register (count) and the per-point XH/XL/YH/YL bytes,
 * applies the TS_SWAP_XY mapping, and fills @p st with the points that fall
 * INSIDE the panel.  Out-of-panel points (the FT5336 all-ones 0xFFF/0xFFF "not
 * touched" sentinel, which an idle or just-released controller can report with a
 * nonzero status count) are dropped, so @p st.count is the number of REAL points
 * (0 when idle).  Returns TOUCH_ERR_STATE if the bus is down, TOUCH_ERR_HAL on an
 * I2C error.
 */
int touch_read(struct touch_state *st);

/* ---- EXTI13 interrupt-driven wake (issue #62) --------------------------- */

/**
 * Arm the FT5336 INT line (PI13 / EXTI15_10): configure PI13 as a rising-edge
 * EXTI and enable EXTI15_10 in the NVIC FIRST, then put the controller in
 * interrupt (trigger) mode -- GMODE reg 0xA4 = 0x01 -- so no edge is lost in the
 * window between enabling INT and arming the line.  The GMODE write does I2C I/O,
 * so this MUST run in thread context with the scheduler running (NOT from
 * touch_init()).  After this an INT edge posts the wake semaphore (see
 * touch_wait_event).  Returns TOUCH_OK or a TOUCH_ERR_*.
 */
int touch_irq_enable(void);

/** Disarm the EXTI15_10 wake (NVIC disable + stop honouring posts). */
void touch_irq_disable(void);

/**
 * Block until the EXTI wake semaphore is posted (a touch INT edge) or @p
 * timeout_ticks elapse.  TX_WAIT_FOREVER blocks indefinitely (~0 % CPU until a
 * touch).  Returns TOUCH_OK on a wake, TOUCH_ERR_TIMEOUT on timeout.  Thread
 * context only.
 */
int touch_wait_event(unsigned long timeout_ticks);

/** Drop any pending wake posts -- call after un-parking so edges that piled up
 *  while parked (or the trailing edges of a finished press) do not trigger a
 *  phantom wake. */
void touch_evt_drain(void);

/** Post the wake semaphore from another thread, so e.g. gui stop can kick a
 *  thread out of a touch_wait_event(TX_WAIT_FOREVER) to re-check its run flag. */
void touch_evt_signal(void);

#ifdef __cplusplus
}
#endif

#endif /* TOUCH_H */
