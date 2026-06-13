/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    guix_camera.c
 * @brief   Camera -> GUIX live preview sink + controller (issue #56).  See
 *          guix_camera.h for the design.
 */
#include "guix_camera.h"
#include "guix_glue.h"
#include "guix_display.h"

#include "camera.h"
#include "frame_pipeline.h"      /* struct frame_sink / frame_desc, FRAME_* */

#include "tx_api.h"

#define LOG_TAG "guix"
#include "log.h"

#include <string.h>

/* Private RGB565 view buffer in MPU non-cacheable SDRAM (.sdram, #40): a
   DMA2D-coherent source for the icon's pixelmap draw and a DMA2D-coherent
   destination for the per-frame slot copy.  Owned solely by this module, so
   the GUIX icon always has a valid image regardless of ring slot recycling. */
static uint16_t cam_view_buf[CAM_VIEW_W * CAM_VIEW_H]
	__attribute__((aligned(32), section(".sdram")));
static GX_PIXELMAP cam_view_pixmap;
static bool        cam_view_inited;

/* Coalesce: at most one CAMERA_FRAME event outstanding at a time.  Set in
   consume() (producer thread) only on a successful event send; cleared in the
   root handler (GUIX thread) -- so a failed send simply retries next frame
   instead of latching the flag and freezing updates. */
static volatile int cam_redraw_pending;

/* In-flight consume() count, maintained by this sink for the off() drain.  The
   pipeline's own _pins is core-internal; we track our own so off() can wait
   until no consume() is running before the stream is re-init'd. */
static volatile int cam_sink_inflight;

/* ---- frame_pipeline push sink ------------------------------------------- */

static struct frame_sink guix_cam_sink;   /* defined below; consume() refers to it */

static int cam_sink_open(void *ctx, enum frame_format fmt, uint16_t w, uint16_t h)
{
	(void)ctx;
	if (fmt != FRAME_FMT_RGB565 || w != CAM_VIEW_W || h != CAM_VIEW_H)
		return -1;                       /* only native QVGA RGB565 */
	return 0;
}

/* Runs on the producer thread (prio 10), outside the pipeline lock, with the
   slot pre-pinned once on our behalf.  Synchronous: copy -> put -> wake.  No
   pin is held across threads, so off() drains in one consume() at most. */
static int cam_sink_consume(void *ctx, const struct frame_desc *f)
{
	int rc;

	(void)ctx;
	cam_sink_inflight++;

	/* slot -> view buffer (DMA2D M2M under ltdc_lock; serialised against the
	   GUIX icon draw so the view is never half-overwritten while displayed). */
	rc = guix_display_copy_rgb565(cam_view_buf, (const uint16_t *)f->data,
	                              CAM_VIEW_W, CAM_VIEW_H, 0u, 0u);

	camera_frame_put(&guix_cam_sink, f);   /* release the pre-pinned slot */

	/* Wake the GUIX thread to repaint the icon.  dirty_mark happens there (the
	   root handler), not here -- a non-GUIX thread must not touch the dirty
	   list.  On a copy failure keep the last good frame and skip the wake. */
	if (rc == 0 && !cam_redraw_pending) {
		if (guix_post_root_event(GX_EVENT_CAMERA_FRAME) == GUIX_OK)
			cam_redraw_pending = 1;
	}

	cam_sink_inflight--;
	return rc;
}

static void cam_sink_close(void *ctx)
{
	(void)ctx;
}

static struct frame_sink guix_cam_sink = {
	.name    = "guix",
	.ctx     = NULL,
	.policy  = FRAME_POLICY_DROP,
	.open    = cam_sink_open,
	.consume = cam_sink_consume,
	.close   = cam_sink_close,
};

/* ---- pixelmap ------------------------------------------------------------ */

GX_PIXELMAP *guix_camera_pixmap(void)
{
	if (!cam_view_inited) {
		memset(cam_view_buf, 0, sizeof cam_view_buf);   /* black until 1st frame */
		cam_view_pixmap.gx_pixelmap_format        = GX_COLOR_FORMAT_565RGB;
		cam_view_pixmap.gx_pixelmap_flags         = 0;
		cam_view_pixmap.gx_pixelmap_data          = (GX_UBYTE *)cam_view_buf;
		cam_view_pixmap.gx_pixelmap_data_size     = sizeof cam_view_buf;
		cam_view_pixmap.gx_pixelmap_aux_data      = GX_NULL;
		cam_view_pixmap.gx_pixelmap_aux_data_size = 0;
		cam_view_pixmap.gx_pixelmap_width         = CAM_VIEW_W;
		cam_view_pixmap.gx_pixelmap_height        = CAM_VIEW_H;
		cam_view_inited = true;
	}
	return &cam_view_pixmap;
}

void guix_camera_mark_drawn(void)
{
	cam_redraw_pending = 0;
}

/* ---- controller ---------------------------------------------------------- */

int guix_camera_on(void)
{
	int rc;

	if (!guix_is_up()) {
		rc = guix_start();
		if (rc != GUIX_OK)
			return rc;
	}

	cam_redraw_pending = 0;
	rc = camera_preview_start(&guix_cam_sink);   /* probe/configure + stream */
	if (rc != 0) {
		LOG_ERR("camera preview start failed (%d)", rc);
		return GUIX_ERR;
	}

	/* Switch to the camera screen on the GUIX thread.  If the event cannot be
	   queued the screen would never switch (preview running but invisible), so
	   roll the stream back rather than leave that inconsistency. */
	if (guix_post_root_event(GX_EVENT_CAMERA_SHOW) != GUIX_OK) {
		LOG_ERR("camera preview: SHOW event post failed; rolling back");
		(void)guix_camera_off();
		return GUIX_ERR;
	}
	LOG_INF("camera preview on");
	return GUIX_OK;
}

int guix_camera_off(void)
{
	int pending, i;

	/* Return to the main screen first (icon hidden), then quiesce the stream. */
	(void)guix_post_root_event(GX_EVENT_CAMERA_HIDE);

	/* Stop new deliveries + release ownership + ask the producer to tear down
	   (owner-only; a no-op if an async OVR teardown already released it).
	   pending = pins still held for our sink at detach. */
	pending = camera_preview_stop(&guix_cam_sink);

	/* If a frame was pre-pinned for us at detach, the producer's consume() may
	   not have run cam_sink_inflight++ yet; yield once so the higher-prio
	   producer enters that consume() before we sample the counter. */
	if (pending > 0)
		tx_thread_sleep(1);

	/* Drain a consume() in flight on the producer thread (prio 10, higher than
	   shell/GUIX so it completes promptly).  Bounded. */
	for (i = 0; i < 100 && cam_sink_inflight > 0; i++)
		tx_thread_sleep(1);
	if (cam_sink_inflight > 0)
		LOG_WRN("camera preview drain timeout (inflight=%d)", cam_sink_inflight);

	cam_redraw_pending = 0;
	LOG_INF("camera preview off");
	return GUIX_OK;
}
