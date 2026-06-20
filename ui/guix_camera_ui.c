/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    guix_camera_ui.c
 * @brief   Camera live-preview GUIX app (issue #61).  See guix_camera_ui.h.
 *
 * Merges the former port/guix/guix_app.c (hand-coded widget tree) and
 * port/guix/guix_camera.c (frame_pipeline push sink + preview controller) into a
 * single ui/ (presentation) module.  The UI is one screen: a GX_ICON drawing the
 * camera view buffer native 1:1, centred on the 480x272 panel.  No demo screens,
 * no on-screen widgets (control widgets come in #68).
 *
 * The widget tree is built by camera_ui_build(), registered with guix_glue via
 * guix_register_app_builder() so the port-layer bring-up never depends on this
 * ui-layer module (dependency inversion, #43/#61).  The live preview is started
 * on the GUIX system thread via GX_EVENT_CAMERA_AUTOSTART -- the camera probe is
 * blocking I2C and must not run in tx_application_define() (boot-safe #60).
 */
#include "guix_camera_ui.h"

#include "guix_glue.h"           /* guix_start/stop/is_up, post_root_event       */
#include "guix_display.h"        /* CAM_VIEW_*, guix_display_cam_* (B2/view)      */
#include "ltdc_display.h"        /* LTDC_LCD_WIDTH / HEIGHT                       */

#include "camera.h"              /* camera_preview_start/stop, camera_frame_put  */
#include "frame_pipeline.h"      /* struct frame_sink / frame_desc, FRAME_*      */

#include "gx_api.h"
#include "tx_api.h"

#define LOG_TAG "guix"
#include "log.h"

#include <stddef.h>
#include <string.h>

/* User events posted to the GUIX root (target = root) -- a NULL-target custom
   event is not routed to the root handler in GUIX v6.5.1, so guix_post_root_event
   always targets the root window. */
#define GX_EVENT_CAMERA_FRAME      (GX_FIRST_USER_EVENT + 0)  /* new view frame   */
#define GX_EVENT_CAMERA_AUTOSTART  (GX_FIRST_USER_EVENT + 1)  /* start preview    */

/* ---- Theme: colour + font + pixelmap tables (resource IDs = table indices). -- */
#define RGB565(r, g, b) ((GX_COLOR)((((r) >> 3) << 11) | (((g) >> 2) << 5) | \
                                    ((b) >> 3)))
enum { C_BG = 0 };
static GX_CONST GX_COLOR guix_color_table[] = {
	RGB565(0x00, 0x00, 0x00),   /* C_BG  black border around the centred image  */
};

/* Built-in GUIX font (compiled from lib/guix/common/src/gx_system_font_8bpp.c).
   No widget uses text yet, but the table is registered so #68 can add labels. */
extern GX_CONST GX_FONT _gx_system_font_8bpp;
enum { F_TEXT = 0 };
static GX_CONST GX_FONT *guix_font_table[] = {
	&_gx_system_font_8bpp,        /* F_TEXT */
};

#define PIX_CAMERA  1u            /* camera live-preview view buffer (index 1)    */
static GX_PIXELMAP *guix_pixelmap_table[2];

/* ---- Widget IDs + static instances. ---- */
#define ID_SCREEN    0x10u
#define ID_CAM_ICON  0x31u

static GX_WINDOW screen;          /* sole screen: full panel, black fill          */
static GX_ICON   cam_icon;        /* camera view buffer, native 320x240           */

/* ---- Camera view buffer + frame_pipeline push sink ----------------------- */
/* Private RGB565 view buffer in MPU non-cacheable SDRAM bank0 (.sdram.fixed,
   #40/#65): a DMA2D-coherent source for the icon's pixelmap draw and destination
   for the per-frame slot copy.  Section attribute kept verbatim so the #65 bank
   separation (linker ASSERT: .sdram.fixed in bank0) is preserved. */
static uint16_t cam_view_buf[CAM_VIEW_W * CAM_VIEW_H]
	__attribute__((aligned(32), section(".sdram.fixed")));
static GX_PIXELMAP cam_view_pixmap;
static bool        cam_view_inited;

/* Coalesce: at most one CAMERA_FRAME event outstanding.  Set in consume()
   (producer thread) on a successful send; cleared in the root handler (GUIX
   thread) -- a failed send just retries next frame instead of latching. */
static volatile int cam_redraw_pending;

/* In-flight consume() count for the stop drain (our own, not the pipeline's). */
static volatile int cam_sink_inflight;

/* Preview lifecycle flags, touched by the GUIX thread (autostart) and the shell
   thread (camera_ui_stop): see the start/stop protocol in camera_ui_autostart().
   volatile so neither thread caches a stale value across the cam_lock blocking
   inside camera_preview_start(). */
static volatile int preview_running;     /* live preview is up                    */
static volatile int start_in_progress;   /* autostart is inside camera_preview_start */
static volatile int stop_requested;      /* a gui stop arrived; cancel/undo start */

static struct frame_sink guix_cam_sink;  /* defined below; consume() refers to it */

static int cam_sink_open(void *ctx, enum frame_format fmt, uint16_t w, uint16_t h)
{
	(void)ctx;
	if (fmt != FRAME_FMT_RGB565 || w != CAM_VIEW_W || h != CAM_VIEW_H)
		return -1;                       /* only native QVGA RGB565 */
	return 0;
}

/* Runs on the producer thread (prio 10), outside the pipeline lock, with the slot
   pre-pinned once on our behalf.  Synchronous: copy -> put -> wake.  No pin is
   held across threads, so the stop drain needs at most one consume(). */
static int cam_sink_consume(void *ctx, const struct frame_desc *f)
{
	int pending = cam_redraw_pending;   /* snapshot for B1 coalescing (#59) */
	int rc = 0;

	(void)ctx;
	cam_sink_inflight++;

	/* B1 (#59): while a repaint is still outstanding the GUIX thread has not yet
	   consumed the previous view, so copying another frame into it is wasted
	   DMA2D/SDRAM bandwidth.  Coalesce: store only when no redraw is pending.
	   guix_display_cam_view_store does the slot->view DMA2D copy AND marks both
	   LTDC buffers stale atomically under ltdc_lock (B2). */
	if (!pending)
		rc = guix_display_cam_view_store((const uint16_t *)f->data);

	camera_frame_put(&guix_cam_sink, f);   /* release the pre-pinned slot */

	/* Wake the GUIX thread to repaint the icon.  dirty_mark happens there (root
	   handler), not here -- a non-GUIX thread must not touch the dirty list. */
	if (rc == 0 && !pending) {
		if (guix_post_root_event(GX_EVENT_CAMERA_FRAME) == GUIX_OK)
			cam_redraw_pending = 1;
	}

	cam_sink_inflight--;
	return rc;
}

/* Called by frame_pipeline_detach() -- on a normal stop AND on the producer's
   async teardown (DCMI overrun).  In both cases the preview is no longer running,
   so clear the flag here: otherwise an async teardown would leave preview_running
   latched and the next `gui start` autostart would no-op (the stream is gone but
   the UI still thinks it is up).  Clearing it lets `gui start` re-arm the preview. */
static void cam_sink_close(void *ctx)
{
	(void)ctx;
	preview_running = 0;
}

static struct frame_sink guix_cam_sink = {
	.name    = "guix",
	.ctx     = NULL,
	.policy  = FRAME_POLICY_DROP,
	.open    = cam_sink_open,
	.consume = cam_sink_consume,
	.close   = cam_sink_close,
};

/* The GX_PIXELMAP wrapping the view buffer; first call blanks it to black so
   nothing garbage shows before the first frame. */
static GX_PIXELMAP *camera_pixmap(void)
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

/* ---- preview start / stop -------------------------------------------------- */

/* Stop the stream, drain any in-flight consume(), and disarm the view path.
   Idempotent; safe from the shell thread (gui stop) or the GUIX thread (autostart
   rollback).  camera_preview_stop() is a no-op when we are not the owner. */
static void preview_teardown(void)
{
	int pending, i;

	pending = camera_preview_stop(&guix_cam_sink);

	/* If a frame was pre-pinned for us at detach, the producer's consume() may not
	   have run cam_sink_inflight++ yet; yield once so the higher-prio producer
	   enters that consume() before we sample the counter. */
	if (pending > 0)
		tx_thread_sleep(1);
	for (i = 0; i < 100 && cam_sink_inflight > 0; i++)
		tx_thread_sleep(1);
	if (cam_sink_inflight > 0)
		LOG_WRN("camera preview drain timeout (inflight=%d)", cam_sink_inflight);

	guix_display_cam_set_visible(false);
	guix_display_cam_preview_end();
	cam_redraw_pending = 0;
}

/*
 * Start the live preview ON THE GUIX SYSTEM THREAD (root handler for
 * GX_EVENT_CAMERA_AUTOSTART).  camera_preview_start() probes/configures the
 * OV5640 over I2C (blocking) -- doing it here keeps tx_application_define()
 * (#60 boot) free of blocking I/O.
 *
 * Concurrency vs a shell-thread `gui stop` (camera_ui_stop):
 *   - duplicate / post-stop guard: stop_requested || !guix_is_up() ||
 *     start_in_progress || preview_running -> no-op.  The handler never clears
 *     stop_requested (camera_ui_start clears it BEFORE posting), so a pending
 *     AUTOSTART that survives a gui stop is dropped here.
 *   - a stop that arrives WHILE we block in camera_preview_start(): camera_ui_stop
 *     calls camera_preview_stop() which serialises on cam_lock and tears the
 *     stream down once we release it; we re-check stop_requested after a
 *     successful start and roll back without latching preview_running.
 */
static void camera_ui_autostart(void)
{
	int rc;

	if (stop_requested || !guix_is_up() || start_in_progress || preview_running)
		return;
	start_in_progress = 1;

	/* Set preview_running BEFORE attaching the sink, and after this point only
	   ever CLEAR it (here on failure/rollback, or in cam_sink_close on stop/async
	   teardown).  This closes the race where an async teardown's close() (which
	   clears the flag) could be overwritten by a trailing "preview_running = 1":
	   the flag is never written to 1 again once the sink can be detached. */
	preview_running = 1;

	/* Arm the B2 view path before any frame can arrive; the camera screen is the
	   sole screen so the preview is visible whenever GUIX is up. */
	guix_display_cam_set_visible(true);
	guix_display_cam_preview_begin(cam_view_buf);
	cam_redraw_pending = 0;

	rc = camera_preview_start(&guix_cam_sink);   /* probe/configure + stream */
	if (rc != 0) {
		preview_running = 0;
		guix_display_cam_set_visible(false);
		guix_display_cam_preview_end();
		LOG_ERR("camera preview start failed (%d)", rc);
		start_in_progress = 0;
		return;
	}

	/* Started OK.  If a gui stop raced us during the probe, undo now (preview_running
	   is cleared by preview_teardown's detach->close and explicitly below). */
	if (stop_requested) {
		preview_teardown();
		preview_running = 0;
		start_in_progress = 0;
		LOG_INF("camera preview start raced gui stop; rolled back");
		return;
	}

	start_in_progress = 0;
	LOG_INF("camera preview on");
}

/* ---- GUIX root event handler ---------------------------------------------- */
/*
 * Catches the camera control events posted via guix_post_root_event() and runs
 * them on the GUIX thread.  CAMERA_FRAME repaints the live image (the sink
 * already copied it into the view buffer); CAMERA_AUTOSTART starts the preview.
 * Everything else defers to the default root processing so pen/draw routing is
 * intact.
 */
static UINT camera_ui_root_event(GX_WIDGET *widget, GX_EVENT *event_ptr)
{
	switch (event_ptr->gx_event_type) {
	case GX_EVENT_CAMERA_FRAME:
		cam_redraw_pending = 0;                       /* mark drawn */
		gx_system_dirty_mark((GX_WIDGET *)&cam_icon);
		return GX_SUCCESS;
	case GX_EVENT_CAMERA_AUTOSTART:
		camera_ui_autostart();
		return GX_SUCCESS;
	default:
		return gx_window_root_event_process((GX_WINDOW_ROOT *)widget, event_ptr);
	}
}

/* ---- widget-tree builder (registered with guix_glue) ---------------------- */
static void rect_set(GX_RECTANGLE *r, INT l, INT t, INT rt, INT b)
{
	r->gx_rectangle_left   = (GX_VALUE)l;
	r->gx_rectangle_top    = (GX_VALUE)t;
	r->gx_rectangle_right  = (GX_VALUE)rt;
	r->gx_rectangle_bottom = (GX_VALUE)b;
}

/*
 * Build the single camera screen on @p root.  Called once by guix_first_start()
 * via the registered builder (display/root passed as void* to keep guix_glue.h
 * GUIX-type-free).  The caller does gx_widget_show(root) afterwards.  Returns 0
 * on success, or the first failing GUIX status.
 */
static int camera_ui_build(void *display_v, void *root_v)
{
	GX_DISPLAY     *display = (GX_DISPLAY *)display_v;
	GX_WINDOW_ROOT *root    = (GX_WINDOW_ROOT *)root_v;
	GX_RECTANGLE    size;
	UINT            status;

	gx_display_color_table_set(display, (GX_COLOR *)guix_color_table,
	    (INT)(sizeof guix_color_table / sizeof guix_color_table[0]));
	gx_display_font_table_set(display, (GX_FONT **)guix_font_table,
	    (UINT)(sizeof guix_font_table / sizeof guix_font_table[0]));
	guix_pixelmap_table[0]          = GX_NULL;
	guix_pixelmap_table[PIX_CAMERA] = camera_pixmap();
	gx_display_pixelmap_table_set(display, guix_pixelmap_table,
	    (UINT)(sizeof guix_pixelmap_table / sizeof guix_pixelmap_table[0]));

	/* Sole screen: full panel, black fill (the border around the centred image). */
	rect_set(&size, 0, 0, (INT)LTDC_LCD_WIDTH - 1, (INT)LTDC_LCD_HEIGHT - 1);
	status = gx_window_create(&screen, "cam_screen", root,
	                          GX_STYLE_BORDER_NONE | GX_STYLE_ENABLED,
	                          ID_SCREEN, &size);
	if (status != GX_SUCCESS)
		return (int)status;
	screen.gx_widget_normal_fill_color = C_BG;

	/* QVGA camera image, native 1:1, centred.  BORDER_NONE (not ENABLED) so it is
	   not selectable.  Its pixelmap is the view buffer the sink refreshes. */
	status = gx_icon_create(&cam_icon, "cam", &screen, PIX_CAMERA,
	                        GX_STYLE_BORDER_NONE, ID_CAM_ICON,
	                        CAM_VIEW_X, CAM_VIEW_Y);
	if (status != GX_SUCCESS)
		return (int)status;

	/* Route the camera control events through the root handler so frame repaint /
	   autostart run on the GUIX thread. */
	root->gx_widget_event_process_function = camera_ui_root_event;
	return 0;
}

/* ---- public lifecycle ----------------------------------------------------- */

void camera_ui_init(void)
{
	guix_register_app_builder(camera_ui_build);   /* boot-safe: no I/O */
}

int camera_ui_start(void)
{
	int rc = guix_start();

	if (rc != GUIX_OK)
		return rc;

	/* Clear the stop flag BEFORE posting so a fresh start re-enables the autostart
	   path; the handler never clears it (else a pending AUTOSTART that survived a
	   gui stop would still start the preview). */
	stop_requested = 0;
	if (guix_post_root_event(GX_EVENT_CAMERA_AUTOSTART) != GUIX_OK)
		LOG_WRN("camera autostart post failed; use 'gui start' to retry");
	return GUIX_OK;
}

int camera_ui_stop(void)
{
	/* Signal an in-flight autostart to roll back, then always tear the preview
	   down: camera_preview_stop() serialises on cam_lock (waits out a probe in
	   progress) and is a no-op when we do not own the stream. */
	stop_requested = 1;
	preview_teardown();
	preview_running = 0;
	(void)guix_stop();
	return 0;
}
