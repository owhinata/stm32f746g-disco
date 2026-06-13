/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    guix_glue.h
 * @brief   GUIX bring-up / lifecycle on ThreadX (issue #55, Epic #48).
 *
 * Ties the GUIX 565rgb display driver (guix_display), the FT5336 input thread
 * (guix_touch) and the demo UI (guix_app) together and starts GUIX on ThreadX.
 * Lazy-started from the `gui` shell command so the display/touch test commands
 * (`lcd`/`touch`) work normally until the UI is wanted.
 *
 * gx_system_initialize() + display/canvas/widget creation + gx_system_start()
 * happen exactly ONCE (the GUIX system thread and its global objects are not
 * torn down).  guix_stop() parks input, hides the UI, blanks the screen and
 * releases LTDC ownership; guix_start() after a stop re-acquires ownership,
 * re-shows and forces a repaint without re-initialising GUIX.
 */
#ifndef GUIX_GLUE_H
#define GUIX_GLUE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes (0 success, negative error). */
#define GUIX_OK          0
#define GUIX_ERR        -1   /* a GUIX API call failed                       */
#define GUIX_ERR_STATE  -2   /* LTDC display is down (cannot start)          */

/** Start (or resume) the GUIX UI: takes over the LCD + FT5336 touch.  Requires
 *  the LTDC display to be up.  Idempotent while running.  Returns GUIX_OK,
 *  GUIX_ERR_STATE (display down) or GUIX_ERR (GUIX API failure). */
int guix_start(void);

/** Stop the GUIX UI: park input, blank the screen, hand the display back to the
 *  `lcd` command.  The GUIX system thread stays alive (suspended on its event
 *  queue).  Idempotent.  Returns GUIX_OK. */
int guix_stop(void);

/** Nonzero while the GUIX UI is running (started and not stopped). */
bool guix_is_up(void);

/** Post a user event (gx_event_type = @p type) to the GUIX root window from any
 *  thread.  The camera live preview (#56) uses this from the producer/shell
 *  threads to drive screen show/hide and per-frame icon repaints -- target is
 *  always the root because a NULL-target custom event is not routed to the root
 *  handler.  @p type is unsigned long to hold GX_FIRST_USER_EVENT-based ids
 *  without forcing GUIX headers on callers.  Returns GUIX_OK, or GUIX_ERR when
 *  the UI is not running or the GUIX event queue is full (caller may retry). */
int guix_post_root_event(unsigned long type);

/** Diagnostic snapshot for `gui info`. */
struct guix_info {
	bool      inited;        /* GUIX initialised at least once       */
	bool      active;        /* UI currently running                 */
	bool      touch;         /* FT5336 input available               */
	uint32_t  thread_prio;   /* GUIX system thread priority          */
	uint32_t  display_handle;/* GUIX display handle                  */
	uintptr_t canvas_mem;    /* current canvas memory (back buffer)  */
};
void guix_get_info(struct guix_info *info);

#ifdef __cplusplus
}
#endif

#endif /* GUIX_GLUE_H */
