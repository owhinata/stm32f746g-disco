/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    guix_touch.c
 * @brief   FT5336 -> GUIX pen-event input thread (issue #55).  See guix_touch.h.
 */
#include "guix_touch.h"
#include "touch.h"
#include "ltdc_display.h"        /* LTDC_LCD_WIDTH / HEIGHT for coord validation */

#include "gx_api.h"

#define LOG_TAG "guix"
#include "log.h"

#include <stdbool.h>
#include <string.h>

/* Input thread: priority 13 (above the GUIX system thread at 14 so queued pen
   events are ready when GUIX wakes; below camera/LED at 10).  1 KiB stack is
   plenty -- the loop only touches a small touch_state and a GX_EVENT. */
#define GUIX_TOUCH_PRIORITY    13
#define GUIX_TOUCH_STACK_SIZE  1024
#define GUIX_TOUCH_POLL_MS     16          /* ~60 Hz while active            */
#define GUIX_TOUCH_PARK_MS     50          /* idle poll of the active flag   */

static TX_THREAD     guix_touch_thread;
static UCHAR         guix_touch_stack[GUIX_TOUCH_STACK_SIZE];
static volatile bool guix_touch_active;    /* armed by gui start / cleared by stop */
static bool          guix_touch_created;   /* one-time create latch          */
static ULONG         guix_touch_display;   /* GUIX display handle for events  */

static void guix_send_pen(ULONG type, USHORT x, USHORT y)
{
	GX_EVENT ev;

	memset(&ev, 0, sizeof ev);
	ev.gx_event_type                            = type;
	ev.gx_event_display_handle                  = guix_touch_display;
	ev.gx_event_payload.gx_event_pointdata.gx_point_x = (GX_VALUE)x;
	ev.gx_event_payload.gx_event_pointdata.gx_point_y = (GX_VALUE)y;
	/* TX_NO_WAIT inside; a full queue just drops this sample (recovered next
	   poll), so the return value is intentionally not fatal. */
	(void)gx_system_event_send(&ev);
}

static void guix_touch_entry(ULONG arg)
{
	bool   was_down = false;
	USHORT last_x = 0, last_y = 0;

	(void)arg;

	for (;;) {
		struct touch_state st;

		/* Parked: sleep here, OUTSIDE any I2C transaction, and re-check.  A
		   pending press is released so a finger held across a stop/start does
		   not look stuck. */
		if (!guix_touch_active) {
			if (was_down) {
				guix_send_pen(GX_EVENT_PEN_UP, last_x, last_y);
				was_down = false;
			}
			tx_thread_sleep(GUIX_TOUCH_PARK_MS);
			continue;
		}

		/* A touch is "valid" only when the controller reports a point inside the
		   panel.  The FT5336 occasionally returns 0xFFF/0xFFF (4095,4095) for the
		   first sample of a press; treating that as a PEN_DOWN sends an
		   off-screen press that GUIX drops, after which the real coordinates
		   arrive as PEN_DRAG and the button never sees its press.  Filter it. */
		if (touch_read(&st) == TOUCH_OK && st.count > 0 &&
		    st.p[0].x < LTDC_LCD_WIDTH && st.p[0].y < LTDC_LCD_HEIGHT) {
			USHORT x = st.p[0].x;
			USHORT y = st.p[0].y;

			if (!was_down)
				guix_send_pen(GX_EVENT_PEN_DOWN, x, y);
			else if (x != last_x || y != last_y)
				guix_send_pen(GX_EVENT_PEN_DRAG, x, y);
			last_x   = x;
			last_y   = y;
			was_down = true;
		} else if (was_down) {
			guix_send_pen(GX_EVENT_PEN_UP, last_x, last_y);
			was_down = false;
		}

		tx_thread_sleep(GUIX_TOUCH_POLL_MS);
	}
}

UINT guix_touch_start(ULONG display_handle)
{
	UINT rc;

	guix_touch_display = display_handle;

	if (guix_touch_created) {
		guix_touch_set_active(true);
		return TX_SUCCESS;
	}

	rc = tx_thread_create(&guix_touch_thread, "guix_touch", guix_touch_entry, 0,
	                      guix_touch_stack, sizeof guix_touch_stack,
	                      GUIX_TOUCH_PRIORITY, GUIX_TOUCH_PRIORITY,
	                      TX_NO_TIME_SLICE, TX_AUTO_START);
	if (rc != TX_SUCCESS) {
		LOG_ERR("touch input thread create failed (%u)", (unsigned)rc);
		return rc;
	}
	guix_touch_created = true;
	guix_touch_set_active(true);
	return TX_SUCCESS;
}

void guix_touch_set_active(bool active)
{
	guix_touch_active = active;
}
