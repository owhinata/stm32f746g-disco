/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    guix_touch.h
 * @brief   FT5336 -> GUIX pen-event input driver (issue #55, Epic #48).
 *
 * A dedicated ThreadX thread turns the first FT5336 touch point into GUIX pen
 * events (GX_EVENT_PEN_DOWN / _PEN_DRAG / _PEN_UP) via gx_system_event_send().
 * It is EXTI-wake driven (issue #62): with no finger down it blocks on the
 * FT5336 INT wake (touch_wait_event) at ~0 % CPU, and only while a finger stays
 * down does it poll ~60 Hz to follow a drag (the controller emits no edges during
 * sustained contact).  Coordinates are panel pixels straight from touch_read()
 * (no scaling -- see touch.h).  The thread is created once and then
 * parked/un-parked by an `active` flag: gui stop clears it (and posts the wake)
 * so the thread sleeps OUTSIDE any I2C transaction (never tx_thread_suspend(),
 * which could stop it while it holds the touch mutex).
 */
#ifndef GUIX_TOUCH_H
#define GUIX_TOUCH_H

#include <stdbool.h>

#include "tx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create and start the input thread (idempotent).  @p display_handle is the
 *  GUIX display handle the pen events are routed to (gx_display.gx_display_handle).
 *  Starts parked; call guix_touch_set_active(true) to begin emitting events.
 *  Returns TX_SUCCESS or a ThreadX error. */
UINT guix_touch_start(ULONG display_handle);

/** Arm (true) or park (false) input.  When parked the thread sleeps at the top
 *  of its loop, outside any FT5336 I2C transaction. */
void guix_touch_set_active(bool active);

#ifdef __cplusplus
}
#endif

#endif /* GUIX_TOUCH_H */
