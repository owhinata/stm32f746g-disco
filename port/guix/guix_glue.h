/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    guix_glue.h
 * @brief   GUIX bring-up / lifecycle on ThreadX (issue #55, Epic #48).
 *
 * Ties the GUIX 565rgb display driver (guix_display), the FT5336 input thread
 * (guix_touch) and the app's widget tree (built via a registered builder, e.g.
 * the ui/ camera app -- #61) together and starts GUIX on ThreadX.
 * Started ON at boot (issue #60): tx_application_define() calls guix_start()
 * after the LTDC/touch bring-up, symmetric with the camera producer.  This is
 * safe before the scheduler -- guix_first_start() only does memory setup +
 * ThreadX object creation (no blocking wait); the first paint is deferred to
 * the GUIX system thread once scheduling runs.  `gui stop` then hands the
 * display back so the `lcd`/`touch` test commands work, and `gui start` resumes.
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

/** The application (ui/) supplies the GUIX widget-tree builder; guix_first_start()
 *  calls it once after creating the display/canvas/root, then does
 *  gx_widget_show(root).  @p display / @p root are void* (GX_DISPLAY* /
 *  GX_WINDOW_ROOT*) so this header stays free of GUIX types and guix_glue does
 *  not depend on the ui layer (dependency inversion, #43/#61).  Returns 0 on
 *  success, nonzero (a GX status) on failure. */
typedef int (*guix_app_builder_fn)(void *display, void *root);

/** Register the widget-tree builder.  Call once (camera_ui_init from
 *  tx_application_define) before the first guix_start(). */
void guix_register_app_builder(guix_app_builder_fn fn);

/** Start (or resume) the GUIX UI: takes over the LCD + FT5336 touch.  Requires
 *  the LTDC display to be up and an app builder registered.  Idempotent while
 *  running.  Returns GUIX_OK, GUIX_ERR_STATE (display down) or GUIX_ERR (GUIX API
 *  failure / no builder registered). */
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
