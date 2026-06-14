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
#define GUIX_TOUCH_SETTLE      4           /* post-wake reads before re-idling  */

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

/*
 * Hybrid EXTI-wake + drag-poll input loop (issue #62).  When no finger is down
 * the thread blocks on the FT5336 INT wake (touch_wait_event FOREVER) at ~0 %
 * CPU; an INT edge wakes it, and while a finger stays down it polls at ~60 Hz to
 * follow a drag (the controller emits NO edges during sustained contact).  On
 * release it returns to the idle wake wait.  Park (gui stop) disarms the wake and
 * sleeps on the active flag.  If arming the INT fails (bus down) the loop
 * degrades to plain ~60 Hz polling so the UI still responds.
 */
static void guix_touch_entry(ULONG arg)
{
	bool     was_down = false;
	bool     irq_ready = false;
	unsigned settle = 0;      /* >0 = in a post-wake poll window (skip idle wait) */
	USHORT   last_x = 0, last_y = 0;

	(void)arg;

	for (;;) {
		struct touch_state st;

		/* Parked (gui stop): release a held press, disarm the wake, and sleep
		   OUTSIDE any I2C transaction.  gui stop posts the wake semaphore so a
		   thread blocked in the idle wait below returns promptly; this bounded
		   park sleep is the fallback. */
		if (!guix_touch_active) {
			if (was_down) {
				guix_send_pen(GX_EVENT_PEN_UP, last_x, last_y);
				was_down = false;
			}
			if (irq_ready) {
				touch_irq_disable();
				irq_ready = false;
			}
			settle = 0;
			tx_thread_sleep(GUIX_TOUCH_PARK_MS);
			continue;
		}

		/* First poll after un-parking: arm the FT5336 INT + PI13 EXTI.  Drop any
		   wake posts that piled up while parked so they cause no phantom wake. */
		if (!irq_ready) {
			touch_evt_drain();
			irq_ready = (touch_irq_enable() == TOUCH_OK);
		}

		/* Idle (no finger down, not in a post-wake settle window): block on the
		   touch wake.  Armed -> FOREVER wait = ~0 % CPU until an INT edge; arming
		   failed (bus down) -> bounded poll so the UI still responds.  A stop
		   wakes us via touch_evt_signal().  After a wake we DO NOT return straight
		   to FOREVER on the first read (settle below) -- the FT5336's first sample
		   of a press can be 0xFFF/0xFFF, and trigger-mode INT edges are not
		   guaranteed for every later frame, so we poll a short window to catch the
		   real coordinates instead of dropping the press. */
		if (!was_down && settle == 0) {
			if (irq_ready)
				(void)touch_wait_event(TX_WAIT_FOREVER);
			else
				tx_thread_sleep(GUIX_TOUCH_POLL_MS);
			if (!guix_touch_active)
				continue;        /* woken by gui stop -> re-park at the top */
			settle = GUIX_TOUCH_SETTLE;   /* open the post-wake poll window */
		}

		/* A touch is "valid" only when the controller reports a point inside the
		   panel (filters the 0xFFF/0xFFF first-sample described above). */
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
			settle   = 0;
			/* A held finger emits no INT edges, so poll to follow a drag.  This
			   ~60 Hz beat runs ONLY while a finger is down; on release the loop
			   drops back to the idle wake wait above. */
			tx_thread_sleep(GUIX_TOUCH_POLL_MS);
		} else if (was_down) {
			guix_send_pen(GX_EVENT_PEN_UP, last_x, last_y);
			was_down = false;
			settle   = 0;
			/* Drop the trailing INT edges of the just-finished press so the next
			   idle wait blocks instead of waking once on a stale post. */
			touch_evt_drain();
		} else if (--settle != 0) {
			/* Woken but no valid touch yet (first-sample 0xFFF, a slightly late
			   frame, or a read error): keep polling the short window. */
			tx_thread_sleep(GUIX_TOUCH_POLL_MS);
		} else {
			/* Window expired with no touch -> give up; the next loop falls into
			   the idle wake wait.  Drop any edges that accumulated meanwhile. */
			touch_evt_drain();
		}
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
