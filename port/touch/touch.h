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
 *   - INT: FT5336_INT = PI13 (EXTI15_10), unused here -- this driver POLLS.
 *
 * Concurrency: public calls serialize on an internal TX_MUTEX; the API is
 * thread-context only (never from an ISR).  touch_init() does NO I2C I/O, so it
 * is safe to call from tx_application_define() before the scheduler runs; the
 * first bus transaction happens lazily on touch_probe()/touch_read().
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

/* FT5336 reports at most 5 simultaneous touch points. */
#define TOUCH_MAX_POINTS  5

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
 * applies the TS_SWAP_XY mapping, and fills @p st.  Returns TOUCH_ERR_STATE if
 * the bus is down, TOUCH_ERR_HAL on an I2C error.
 */
int touch_read(struct touch_state *st);

#ifdef __cplusplus
}
#endif

#endif /* TOUCH_H */
