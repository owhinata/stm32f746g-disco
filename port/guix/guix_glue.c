/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    guix_glue.c
 * @brief   GUIX bring-up / lifecycle on ThreadX (issue #55).  See guix_glue.h.
 */
#include "guix_glue.h"
#include "guix_display.h"
#include "guix_touch.h"
#include "guix_app.h"

#include "ltdc_display.h"
#include "touch.h"

#include "gx_api.h"

#define LOG_TAG "guix"
#include "log.h"

#include <string.h>

#include "gx_user.h"             /* GX_SYSTEM_THREAD_PRIORITY for `gui info` */

static GX_DISPLAY     guix_display;
static GX_CANVAS      guix_canvas;
static GX_WINDOW_ROOT guix_root;

static bool guix_inited;          /* gx_system_initialize() done (one-shot) */
static bool guix_active;          /* UI currently running                   */

static int guix_first_start(void)
{
	GX_RECTANGLE size;
	uint16_t    *back;

	/* Take the display FIRST, then capture the back buffer: ownership disables
	   the `lcd` draw/flip path, so a backgrounded `lcd anim &` cannot move
	   ltdc_front between this read and the GUIX thread's first buffer toggle --
	   the canvas would otherwise be bound to a now-front (visible) buffer.
	   Every failure path releases ownership before returning. */
	ltdc_gui_take(true);
	back = ltdc_back_buffer();

	if (gx_system_initialize() != GX_SUCCESS) {
		LOG_ERR("gx_system_initialize failed");
		goto fail;
	}
	if (gx_display_create(&guix_display, "f746", guix_display_driver_setup,
	                      LTDC_LCD_WIDTH, LTDC_LCD_HEIGHT) != GX_SUCCESS) {
		LOG_ERR("gx_display_create failed");
		goto fail;
	}
	if (gx_canvas_create(&guix_canvas, "canvas", &guix_display,
	                     GX_CANVAS_MANAGED | GX_CANVAS_VISIBLE,
	                     LTDC_LCD_WIDTH, LTDC_LCD_HEIGHT, (GX_COLOR *)back,
	                     (ULONG)LTDC_LCD_WIDTH * LTDC_LCD_HEIGHT * 2u)
	    != GX_SUCCESS) {
		LOG_ERR("gx_canvas_create failed");
		goto fail;
	}
	size.gx_rectangle_left   = 0;
	size.gx_rectangle_top    = 0;
	size.gx_rectangle_right  = (GX_VALUE)(LTDC_LCD_WIDTH - 1);
	size.gx_rectangle_bottom = (GX_VALUE)(LTDC_LCD_HEIGHT - 1);
	if (gx_window_root_create(&guix_root, "root", &guix_canvas,
	                          GX_STYLE_NONE, 0, &size) != GX_SUCCESS) {
		LOG_ERR("gx_window_root_create failed");
		goto fail;
	}
	if (guix_app_create(&guix_display, &guix_root) != GX_SUCCESS) {
		LOG_ERR("guix_app_create failed");
		goto fail;
	}

	gx_widget_show(&guix_root);          /* show the whole tree (visible + clips) */
	guix_app_show_screen(0);             /* then hide screen 1, leaving screen 0  */
	if (gx_system_start() != GX_SUCCESS) {
		LOG_ERR("gx_system_start failed");
		goto fail;
	}
	guix_inited = true;
	return GUIX_OK;

fail:
	ltdc_gui_take(false);
	return GUIX_ERR;
}

/* Wake the (sleeping) GUIX system thread and force a full repaint -- used on a
   restart, where gx_widget_show() only marks dirty but posts no event. */
static void guix_force_redraw(void)
{
	GX_EVENT ev;

	gx_system_dirty_mark(&guix_root);
	memset(&ev, 0, sizeof ev);
	ev.gx_event_type           = GX_EVENT_REDRAW;
	ev.gx_event_display_handle = guix_display.gx_display_handle;
	(void)gx_system_event_send(&ev);
}

int guix_start(void)
{
	if (!ltdc_is_up()) {
		LOG_ERR("LTDC display down -- cannot start GUIX");
		return GUIX_ERR_STATE;
	}
	if (guix_active)
		return GUIX_OK;                  /* already running */

	if (!guix_inited) {
		int rc = guix_first_start();

		if (rc != GUIX_OK)
			return rc;
	} else {
		/* Restart: re-take ownership, re-establish the canvas-on-back-buffer
		   invariant (the manual blank in guix_stop() moved ltdc_front), re-show
		   and force a repaint. */
		ltdc_gui_take(true);
		guix_canvas.gx_canvas_memory = (GX_COLOR *)ltdc_back_buffer();
		gx_widget_show(&guix_root);
		guix_force_redraw();
	}

	/* Input is best-effort: the UI still shows if the touch bus is down. */
	if (touch_is_up())
		(void)guix_touch_start(guix_display.gx_display_handle);
	else
		LOG_WRN("FT5336 down -- GUIX UI will not respond to touch");

	guix_active = true;
	LOG_INF("GUIX up (display handle %lu, system-thread prio %u)",
	        (unsigned long)guix_display.gx_display_handle,
	        (unsigned)GX_SYSTEM_THREAD_PRIORITY);
	return GUIX_OK;
}

int guix_stop(void)
{
	if (!guix_active)
		return GUIX_OK;

	guix_touch_set_active(false);        /* park input (outside any I2C op) */
	gx_widget_hide(&guix_root);          /* GUIX stops compositing the UI   */

	/* Blank the screen via the owner DMA2D path (public ltdc_fill() is a no-op
	   while GUIX owns the display), present it, then resync the canvas onto the
	   new back buffer so a later restart's first draw lands off-screen. */
	guix_display_fill_back(LTDC_RGB565_BLACK);
	(void)ltdc_gui_flip();
	guix_canvas.gx_canvas_memory = (GX_COLOR *)ltdc_back_buffer();

	ltdc_gui_take(false);                /* hand the display back to `lcd`  */
	guix_active = false;
	LOG_INF("GUIX stopped");
	return GUIX_OK;
}

bool guix_is_up(void)
{
	return guix_active;
}

void guix_get_info(struct guix_info *info)
{
	if (info == NULL)
		return;
	info->inited         = guix_inited;
	info->active         = guix_active;
	info->touch          = touch_is_up();
	info->thread_prio    = (uint32_t)GX_SYSTEM_THREAD_PRIORITY;
	info->display_handle = (uint32_t)guix_display.gx_display_handle;
	info->canvas_mem     = (uintptr_t)guix_canvas.gx_canvas_memory;
}
