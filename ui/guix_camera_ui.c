/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    guix_camera_ui.c
 * @brief   Camera GUIX app: live-preview screen + settings screen (issues #61/#68).
 *          See guix_camera_ui.h.
 *
 * #61 merged the former guix_app (widget tree) and guix_camera (frame sink +
 * preview controller) into this single ui/ module.  #68 splits the UI into two
 * full-panel screens that are siblings under the root (the #55 multi-screen
 * show/hide idiom):
 *
 *   - preview_screen: a clean live image (a GX_ICON drawing the QVGA view buffer
 *     native 1:1 centred, black border) with NO visible widgets on top -- tapping
 *     anywhere switches to the settings screen.  Keeping the live image clean
 *     (no widgets composited on top) sidesteps the #59 B2 copy-forward, which
 *     re-stamps the camera rect every flip and would draw over any overlay.
 *   - settings_screen: a static dark panel (no live image) holding the 9 OV5640
 *     image-quality controls + a Back button.  Because the live image is hidden
 *     here, the controls are always readable and never composite-race the camera
 *     rect.
 *
 * The quality controls call the shared port/camera API (camera_set_*), which
 * applies the SDE registers live over I2C; the same control layer backs both this
 * UI and the `camera set` shell commands.  A `camera set` made while the settings
 * screen was closed (e.g. during a `gui stop`) is picked up because opening the
 * settings screen re-reads the live values (controls_sync).
 *
 * Threading: the screen transitions and the button handlers that call camera_set_*
 * all run on the GUIX system thread.  camera_set_* take the camera op-lock and do
 * a short blocking I2C write -- safe during streaming (the SDE bus is independent
 * of the DCMI capture path), it just blocks GUIX dispatch for the write.
 *
 * Known limitation (#70): changing image quality in a dark scene can drop the
 * preview fps, because the camera re-clamps the sensor VTS to the (stretched)
 * AEC exposure on every settings apply (#67).  This is a camera-side behaviour
 * shared with `camera set`; its fix is tracked separately in #70.
 */
#include "guix_camera_ui.h"

#include "guix_glue.h"           /* guix_start/stop/is_up, post_root_event       */
#include "guix_display.h"        /* CAM_VIEW_*, guix_display_cam_* (B2/view)      */
#include "ltdc_display.h"        /* LTDC_LCD_WIDTH / HEIGHT                       */

#include "camera.h"              /* camera_subscribe / camera_set_x / ranges, enums */
#include "frame_pipeline.h"      /* struct frame_sink / frame_desc, FRAME_*      */
#include "nn_camera.h"           /* nn_camera_running / nn_camera_dets_get (bbox, #83) */

#include "gx_api.h"
#include "tx_api.h"

#define LOG_TAG "guix"
#include "log.h"

#include <stddef.h>
#include <string.h>

/* User events posted to the GUIX root (target = root). */
#define GX_EVENT_CAMERA_FRAME      (GX_FIRST_USER_EVENT + 0)  /* new view frame   */
#define GX_EVENT_CAMERA_AUTOSTART  (GX_FIRST_USER_EVENT + 1)  /* subscribe preview */
#define GX_EVENT_CAMERA_GEOM       (GX_FIRST_USER_EVENT + 2)  /* base res changed  */

/* ---- Theme: colour + font + pixelmap tables (resource IDs = table indices). -- */
#define RGB565(r, g, b) ((GX_COLOR)((((r) >> 3) << 11) | (((g) >> 2) << 5) | \
                                    ((b) >> 3)))
enum { C_BG = 0, C_OVL, C_TEXT, C_BTN, C_BTN_SEL };
static GX_CONST GX_COLOR guix_color_table[] = {
	RGB565(0x00, 0x00, 0x00),   /* C_BG     black border around the centred image */
	RGB565(0x10, 0x18, 0x28),   /* C_OVL    dark settings-panel background         */
	RGB565(0xff, 0xff, 0xff),   /* C_TEXT   white                                  */
	RGB565(0x29, 0x5f, 0xa6),   /* C_BTN    steel-blue button face                 */
	RGB565(0x40, 0xc0, 0xff),   /* C_BTN_SEL pressed-button face                   */
};

extern GX_CONST GX_FONT _gx_system_font_8bpp;
enum { F_TEXT = 0 };
static GX_CONST GX_FONT *guix_font_table[] = { &_gx_system_font_8bpp };

#define PIX_CAMERA  1u
static GX_PIXELMAP *guix_pixelmap_table[2];

/* ---- Widget IDs (must be < 256 for GX_SIGNAL). ---- */
#define ID_PREVIEW       0x10u   /* preview screen (live image)                   */
#define ID_SETTINGS      0x20u   /* settings screen (controls)                    */
#define ID_CAM_ICON      0x31u
#define ID_BIP_MINUS     0x40u   /* +0..3: brightness/contrast/saturation/hue     */
#define ID_BIP_PLUS      0x44u   /* +0..3                                         */
#define ID_CYC           0x48u   /* +0..4: awb/effect/flip/zoom/night             */
#define ID_BACK          0x50u
#define ID_RES           0x51u   /* resolution cycle (stop->set->restart, #69)    */

/* Prompts are NON-transparent (own opaque dark fill) so the text is always
   readable, and left/centre aligned for clarity. */
#define LABEL_STYLE   (GX_STYLE_ENABLED | \
                       GX_STYLE_HALIGN_LEFT | GX_STYLE_VALIGN_CENTER)
#define VALUE_STYLE   (GX_STYLE_ENABLED | \
                       GX_STYLE_HALIGN_CENTER | GX_STYLE_VALIGN_CENTER)
#define BUTTON_STYLE  (GX_STYLE_BORDER_RAISED | GX_STYLE_ENABLED | \
                       GX_STYLE_HALIGN_CENTER | GX_STYLE_VALIGN_CENTER)

/* ---- Static widget instances. ---- */
static GX_WINDOW preview_screen;  /* live image, black fill; tap -> settings       */
static GX_ICON   cam_icon;        /* camera view buffer, native 320x240            */
static GX_WINDOW settings_screen; /* static control panel, dark fill               */

#define N_BIP  4                  /* brightness, contrast, saturation, hue         */
#define N_CYC  5                  /* awb, effect, flip, zoom, night                */
static GX_PROMPT        bip_label[N_BIP];
static GX_TEXT_BUTTON   bip_minus[N_BIP], bip_plus[N_BIP];
static GX_NUMERIC_PROMPT bip_value[N_BIP];
static GX_PROMPT        cyc_label[N_CYC];
static GX_TEXT_BUTTON   cyc_btn[N_CYC];
static GX_PROMPT        res_label;
static GX_TEXT_BUTTON   btn_res;          /* preview resolution cycle (#69)        */
static GX_TEXT_BUTTON   btn_back;

/* true while the settings screen is up (preview hidden).  The CAMERA_FRAME
   handler then skips the (covered) icon redraw; the producer keeps streaming. */
static bool settings_active;

static void camera_ui_autostart(void);   /* forward: called by the root handler */
static void apply_view_geometry(int idx); /* forward: CAMERA_GEOM re-sync (root)   */
static void controls_sync(void);         /* forward: re-read settings into caches */
static void controls_update_display(void);
static void enter_preview(void);         /* forward: Back / autostart -> preview  */
static void cycle_resolution(void);      /* forward: settings -> next resolution   */

/* ---- Control metadata. enum setters are wrapped so the table is int(*)(int). - */
static int set_awb_i(int v)    { return camera_set_awb((enum camera_awb)v); }
static int set_effect_i(int v) { return camera_set_effect((enum camera_effect)v); }
static int set_flip_i(int v)   { return camera_set_flip((enum camera_flip)v); }

static GX_CONST struct {
	int (*set)(int);
	int min, max, step;
	GX_CONST GX_CHAR *label;
} bip_meta[N_BIP] = {
	{ camera_set_brightness, CAM_LEVEL_MIN, CAM_LEVEL_MAX, 1, "Brightness" },
	{ camera_set_contrast,   CAM_LEVEL_MIN, CAM_LEVEL_MAX, 1, "Contrast"   },
	{ camera_set_saturation, CAM_LEVEL_MIN, CAM_LEVEL_MAX, 1, "Saturation" },
	{ camera_set_hue,        CAM_HUE_MIN * 30, CAM_HUE_MAX * 30, 30, "Hue (deg)" },
};
static int bip_cur[N_BIP];        /* current applied value (level, or hue degrees) */

static GX_CONST GX_CHAR *GX_CONST awb_names[]  = { "Auto", "Sunny", "Office",
                                                   "Home", "Cloudy" };
static GX_CONST GX_CHAR *GX_CONST fx_names[]   = { "None", "B&W", "Sepia",
                                                   "Negative", "Blue", "Red",
                                                   "Green" };
static GX_CONST GX_CHAR *GX_CONST flip_names[] = { "None", "Mirror", "Flip",
                                                   "Both" };
static GX_CONST GX_CHAR *GX_CONST zoom_names[] = { "1x", "2x", "4x", "8x" };
static GX_CONST GX_CHAR *GX_CONST night_names[]= { "Off", "On" };
static GX_CONST int zoom_vals[] = { 1, 2, 4, 8 };

static GX_CONST struct {
	int (*set)(int);
	GX_CONST GX_CHAR *GX_CONST *names;
	GX_CONST int *values;             /* NULL: applied value == index             */
	int count;
	GX_CONST GX_CHAR *label;
} cyc_meta[N_CYC] = {
	{ set_awb_i,    awb_names,   NULL,      5, "White balance" },
	{ set_effect_i, fx_names,    NULL,      7, "Effect" },
	{ set_flip_i,   flip_names,  NULL,      4, "Flip"   },
	{ camera_set_zoom, zoom_names, zoom_vals, 4, "Zoom"  },
	{ camera_set_night, night_names, NULL,   2, "Night mode" },
};
static int cyc_cur[N_CYC];        /* current index into names[]                    */

/* ---- preview resolution (#69): the GUI cycles these two streamable RGB565 modes
   (format/fps stay fixed).  Both are 4:3 like the sensor's ISP FOV, so neither
   stretches; 480x272 (16:9) was removed in #84 (horizontal stretch + SDRAM
   overrun).  geometry is centred on the 480x272 panel; the view buffer below is
   sized for the largest (QVGA) so a switch never reallocates -- only the live
   geometry passed to guix_display changes. */
static GX_CONST struct {
	enum camera_res res;
	uint16_t w, h;
	GX_CONST GX_CHAR *name;
} res_tbl[] = {
	{ CAM_RES_QQVGA,   160, 120, "160x120" },
	{ CAM_RES_QVGA,    320, 240, "320x240" },
};
#define N_RES        ((int)(sizeof res_tbl / sizeof res_tbl[0]))
#define RES_VIEW_X(i)  ((uint16_t)((LTDC_LCD_WIDTH  - res_tbl[i].w) / 2))
#define RES_VIEW_Y(i)  ((uint16_t)((LTDC_LCD_HEIGHT - res_tbl[i].h) / 2))
/* GUI preview boots at QVGA (320x240, index 1), centred on the panel (#84); the
   selection then persists across gui stop/start.  The shell capture/stream path
   keeps its own QVGA default (`mode`), independent of this GUI preview resolution. */
static int cur_res_idx = 1;       /* index into res_tbl; default QVGA (#84)         */

/* ---- Camera view buffer + frame_pipeline push sink (#56/#61) -------------- */
static uint16_t cam_view_buf[CAM_VIEW_W_MAX * CAM_VIEW_H_MAX]
	__attribute__((aligned(32), section(".sdram.fixed")));
static GX_PIXELMAP cam_view_pixmap;
static bool        cam_view_inited;
static volatile int cam_redraw_pending;
static volatile int cam_sink_inflight;

/* Preview lifecycle flags (GUIX + shell threads).  Since Epic #99 Phase 1 (#100)
   the GUI preview is a *subscriber* of the base capture, not its owner.
   preview_running is set by cam_sink_open when the sink attaches and cleared by
   cam_sink_close (a base stop / DCMI overrun / cascade); a base stop freezes the
   last frame (preview kept visible) and a base restart re-attaches so frames
   resume.  The base owns the DCMI overrun auto-recovery now (camera.c #100), so
   there is no GUI backoff.  stop_requested gates a `gui stop` racing the subscribe. */
static volatile int preview_running;
static volatile int stop_requested;

/* Latest base geometry delivered to the sink (res_tbl index), latched by
   cam_sink_open off the GUIX thread; the CAMERA_GEOM handler re-syncs the GUIX
   pixmap/widget geometry to it on the GUIX thread when the base res differs. */
static volatile int cam_delivered_idx = -1;

/* Box outline colour: bright green (RGB565), high contrast on a camera image. */
#define OVERLAY_BOX_COLOR  0x07E0u

/* Map a delivered frame geometry to a res_tbl index, or -1 if unknown. */
static int res_idx_from_w(uint16_t w)
{
	for (int i = 0; i < N_RES; i++)
		if (res_tbl[i].w == w)
			return i;
	return -1;
}

static struct frame_sink guix_cam_sink;

/* Sink attach (any thread: base start on the producer/shell, or a live subscribe).
   The GUI adapts to whatever streamable RGB565 geometry the base publishes (QQVGA
   / QVGA): arm the display blit geometry (thread-safe, ltdc-locked) and latch the
   delivered res index; if it differs from the current GUIX geometry, post
   CAMERA_GEOM so the GUIX thread re-syncs the pixmap/icon.  Rejects a non-RGB565 or
   unknown geometry (would not fit cam_view_buf). */
static int cam_sink_open(void *ctx, enum frame_format fmt, uint16_t w, uint16_t h)
{
	int idx;

	(void)ctx;
	if (fmt != FRAME_FMT_RGB565)
		return -1;
	idx = res_idx_from_w(w);
	if (idx < 0 || res_tbl[idx].h != h)
		return -1;                          /* not a preview geometry we can render */

	if (guix_display_cam_preview_begin(cam_view_buf, w, h,
	                                   RES_VIEW_X(idx), RES_VIEW_Y(idx)) != 0)
		return -1;
	cam_delivered_idx = idx;
	preview_running = 1;
	if (idx != cur_res_idx)
		(void)guix_post_root_event(GX_EVENT_CAMERA_GEOM);
	return 0;
}

/* Draw the latest face detections as bounding boxes into the view buffer (GUIX
   thread, right after the frame store).  The dets are normalized [0,1] to the
   full preview frame, which maps 1:1 onto the view buffer, so they scale directly.
   nn_camera_dets_get() returns 0 when inference is not running or has no detection,
   so the boxes clear when `ai stream stop` runs or a face is lost. */
static void overlay_draw_boxes(void)
{
	struct bf_det d[BF_MAX_DET];
	int n = nn_camera_dets_get(d, BF_MAX_DET);

	for (int i = 0; i < n; i++)
		guix_display_cam_overlay_box(d[i].x, d[i].y, d[i].w, d[i].h,
		                             OVERLAY_BOX_COLOR);
}

static int cam_sink_consume(void *ctx, const struct frame_desc *f)
{
	int pending = cam_redraw_pending;
	int rc = 0;

	(void)ctx;
	cam_sink_inflight++;
	if (!pending)
		rc = guix_display_cam_view_store((const uint16_t *)f->data);
	camera_frame_put(&guix_cam_sink, f);
	if (rc == 0 && !pending) {
		/* Box drawing is deferred to the GUIX thread's CAMERA_FRAME handler, off the
		   producer's DCMI-servicing critical path -- taking ltdc_lock per box here
		   (prio 10) delayed the DMA re-arm and caused overruns (#83). */
		if (guix_post_root_event(GX_EVENT_CAMERA_FRAME) == GUIX_OK)
			cam_redraw_pending = 1;
	}
	cam_sink_inflight--;
	return rc;
}

/* Base detached this subscriber: a normal `camera stream stop`, a DCMI overrun, or
   a cascade.  A base *stop*, not a GUI stop (contract #100.2): just clear
   preview_running so the last frame freezes on screen -- the preview stays visible
   and resumes when the base re-attaches (cam_sink_open re-arms).  Non-blocking, no
   camera API re-entry, so it is safe under the camera driver lock.  The base owns
   the overrun auto-recovery now (camera.c #100), so there is nothing to restart or
   tear down here (the AI subscriber's own close handles its bbox/session). */
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

static GX_PIXELMAP *camera_pixmap(void)
{
	if (!cam_view_inited) {
		memset(cam_view_buf, 0, sizeof cam_view_buf);
		cam_view_pixmap.gx_pixelmap_format        = GX_COLOR_FORMAT_565RGB;
		cam_view_pixmap.gx_pixelmap_flags         = 0;
		cam_view_pixmap.gx_pixelmap_data          = (GX_UBYTE *)cam_view_buf;
		cam_view_pixmap.gx_pixelmap_aux_data      = GX_NULL;
		cam_view_pixmap.gx_pixelmap_aux_data_size = 0;
		/* width/height/data_size are set per selected resolution (#69). */
		cam_view_pixmap.gx_pixelmap_width  = res_tbl[cur_res_idx].w;
		cam_view_pixmap.gx_pixelmap_height = res_tbl[cur_res_idx].h;
		cam_view_pixmap.gx_pixelmap_data_size =
			(ULONG)res_tbl[cur_res_idx].w * res_tbl[cur_res_idx].h * 2u;
		cam_view_inited = true;
	}
	return &cam_view_pixmap;
}

/* ---- screen transitions (GUIX thread) ------------------------------------- */

/* preview -> settings: re-read live values, stop the #59 B2 camera copy-forward
   (so the producer stops re-stamping the camera rect over the static panel), then
   swap screens.  Re-sync because the shell `camera set` (e.g. after a `gui stop`)
   may have changed settings since this screen was last shown.  Dirty the whole
   screen (not just what gx_widget_show marks) so BOTH LTDC buffers are fully
   repainted via the toggle's copy-forward -- otherwise a later partial redraw
   (e.g. a button press) can flip to a buffer still holding the old contents. */
static void enter_settings(void)
{
	controls_sync();
	controls_update_display();
	guix_display_cam_set_visible(false);
	gx_widget_hide((GX_WIDGET *)&preview_screen);
	gx_widget_show((GX_WIDGET *)&settings_screen);
	settings_active = true;
	gx_system_dirty_mark((GX_WIDGET *)&settings_screen);
}

/* settings -> preview (Back / boot reset): swap screens, re-arm the B2 copy-
   forward iff the preview is running, and force a full preview repaint so the
   black border AND the live image are restored on both LTDC buffers at once (a
   cam_icon-only dirty would leave the border holding stale/uncovered pixels).  At
   boot this runs before the preview starts (preview_running == 0 -> B2 stays off;
   the autostart enables it). */
static void enter_preview(void)
{
	gx_widget_hide((GX_WIDGET *)&settings_screen);
	gx_widget_show((GX_WIDGET *)&preview_screen);
	settings_active = false;
	guix_display_cam_set_visible(preview_running != 0);
	gx_system_dirty_mark((GX_WIDGET *)&preview_screen);
}

/* ---- control button handlers (GUIX thread) -------------------------------- */

static void bip_adjust(int i, int dir)
{
	int nv = bip_cur[i] + dir * bip_meta[i].step;

	if (nv < bip_meta[i].min)
		nv = bip_meta[i].min;
	if (nv > bip_meta[i].max)
		nv = bip_meta[i].max;
	if (nv != bip_cur[i] && bip_meta[i].set(nv) == CAM_OK) {
		bip_cur[i] = nv;
		gx_numeric_prompt_value_set(&bip_value[i], nv);
	}
}

static void cyc_next(int i)
{
	int next = (cyc_cur[i] + 1) % cyc_meta[i].count;
	int v    = cyc_meta[i].values ? cyc_meta[i].values[next] : next;

	if (cyc_meta[i].set(v) == CAM_OK) {
		cyc_cur[i] = next;
		gx_text_button_text_set(&cyc_btn[i], cyc_meta[i].names[next]);
	}
}

/* Dispatch a settings child button's CLICKED signal; returns true if handled. */
static bool settings_signal(ULONG type)
{
	int i;

	for (i = 0; i < N_BIP; i++) {
		if (type == GX_SIGNAL(ID_BIP_MINUS + i, GX_EVENT_CLICKED)) {
			bip_adjust(i, -1); return true;
		}
		if (type == GX_SIGNAL(ID_BIP_PLUS + i, GX_EVENT_CLICKED)) {
			bip_adjust(i, +1); return true;
		}
	}
	for (i = 0; i < N_CYC; i++) {
		if (type == GX_SIGNAL(ID_CYC + i, GX_EVENT_CLICKED)) {
			cyc_next(i); return true;
		}
	}
	if (type == GX_SIGNAL(ID_RES, GX_EVENT_CLICKED)) { cycle_resolution(); return true; }
	if (type == GX_SIGNAL(ID_BACK, GX_EVENT_CLICKED)) { enter_preview(); return true; }
	return false;
}

/* ---- event handlers ------------------------------------------------------- */

/* Preview screen: a tap (cam_icon is BORDER_NONE so it does not take the touch,
   so PEN_DOWN reaches the screen) opens the settings screen. */
static UINT preview_screen_event(GX_WIDGET *widget, GX_EVENT *event_ptr)
{
	if (event_ptr->gx_event_type == GX_EVENT_PEN_DOWN) {
		enter_settings();
		return GX_SUCCESS;
	}
	return gx_window_event_process((GX_WINDOW *)widget, event_ptr);
}

/* Settings screen: routes its buttons' CLICKED signals; everything else (incl. a
   background tap) defers to the default window handler so the buttons hit-test. */
static UINT settings_screen_event(GX_WIDGET *widget, GX_EVENT *event_ptr)
{
	if (settings_signal(event_ptr->gx_event_type))
		return GX_SUCCESS;
	return gx_window_event_process((GX_WINDOW *)widget, event_ptr);
}

/* Root handler: camera control events posted from the producer/shell, run on the
   GUIX thread.  (Set on the root by the builder.) */
static UINT camera_ui_root_event(GX_WIDGET *widget, GX_EVENT *event_ptr)
{
	switch (event_ptr->gx_event_type) {
	case GX_EVENT_CAMERA_FRAME:
		/* Always clear the coalesce flag first so the producer keeps copying/
		   posting; only skip the icon redraw while the settings screen covers it. */
		cam_redraw_pending = 0;
		if (!settings_active) {
			/* Draw the face boxes here (GUIX thread) rather than on the producer:
			   the producer just stored this frame into cam_view_buf, so stamping the
			   boxes on top now -- before the icon redraw reads it -- keeps the boxes
			   off the DCMI-servicing critical path (#83).  Drawn whenever inference is
			   running; overlay_draw_boxes() is a no-op with no detections, so the box
			   clears when `ai stream stop` runs or a face is lost. */
			if (nn_camera_running())
				overlay_draw_boxes();
			gx_system_dirty_mark((GX_WIDGET *)&cam_icon);
		}
		return GX_SUCCESS;
	case GX_EVENT_CAMERA_AUTOSTART:
		camera_ui_autostart();
		return GX_SUCCESS;
	case GX_EVENT_CAMERA_GEOM:
		/* The base publishes a different resolution than the current GUIX geometry
		   (a `camera res` + restart from the shell, say).  Re-sync the pixmap/icon
		   on the GUIX thread to the delivered geometry. */
		{
			int idx = cam_delivered_idx;
			if (idx >= 0 && idx != cur_res_idx) {
				cur_res_idx = idx;
				apply_view_geometry(idx);
				gx_text_button_text_set(&btn_res, res_tbl[idx].name);
				guix_display_cam_set_visible(!settings_active);
				gx_system_dirty_mark((GX_WIDGET *)&preview_screen);
			}
		}
		return GX_SUCCESS;
	default:
		return gx_window_root_event_process((GX_WINDOW_ROOT *)widget, event_ptr);
	}
}

/* ---- preview start / stop -------------------------------------------------- */

/* Unsubscribe the preview sink from the base (the base keeps running for other
   subscribers) and blank the preview.  Used by `gui stop`. */
static void preview_teardown(void)
{
	int pending, i;

	pending = camera_unsubscribe(&guix_cam_sink);
	if (pending > 0)
		tx_thread_sleep(1);
	for (i = 0; i < 100 && cam_sink_inflight > 0; i++)
		tx_thread_sleep(1);
	if (cam_sink_inflight > 0)
		LOG_WRN("camera preview drain timeout (inflight=%d)", cam_sink_inflight);
	preview_running = 0;
	guix_display_cam_set_visible(false);
	guix_display_cam_preview_end();
	cam_redraw_pending = 0;
}

/* Rebuild the GUIX view pixmap + camera-icon geometry for res_tbl[idx] (#69).
   Runs on the GUIX thread.  The view buffer is max-sized (QVGA), so only the
   pixmap width/height/data_size and the icon rectangle change -- the data pointer
   is unchanged.  Safe while the preview screen is hidden (settings up): Back's
   full-screen dirty repaints the icon at its new size and the exposed border. */
static void apply_view_geometry(int idx)
{
	uint16_t w = res_tbl[idx].w, h = res_tbl[idx].h;
	uint16_t x = RES_VIEW_X(idx), y = RES_VIEW_Y(idx);
	GX_RECTANGLE rc;

	cam_view_pixmap.gx_pixelmap_width     = w;
	cam_view_pixmap.gx_pixelmap_height    = h;
	cam_view_pixmap.gx_pixelmap_data_size = (ULONG)w * h * 2u;
	rc.gx_rectangle_left   = (GX_VALUE)x;
	rc.gx_rectangle_top    = (GX_VALUE)y;
	rc.gx_rectangle_right  = (GX_VALUE)(x + w - 1);
	rc.gx_rectangle_bottom = (GX_VALUE)(y + h - 1);
	gx_widget_resize(&cam_icon, &rc);
}

/* Subscribe the preview sink to the base capture (GUIX thread).  The GUI is a
   subscriber now (#100): this does NOT start the base -- it attaches iff the base
   is already streaming RGB565 (cam_sink_open arms the blit geometry), otherwise it
   stays enabled + idle and attaches at the next `camera stream start`.  @p visible
   controls whether the camera rect is shown (false while under the settings screen).
   Returns 0, or <0 on a hard subscribe failure. */
static int preview_subscribe(bool visible)
{
	int rc;

	if (stop_requested || !guix_is_up())
		return -1;
	guix_display_cam_set_visible(visible);
	cam_redraw_pending = 0;
	rc = camera_subscribe(&guix_cam_sink, CAM_FMT_RGB565);
	if (rc != 0) {
		guix_display_cam_set_visible(false);
		LOG_ERR("camera preview subscribe failed (%d)", rc);
		return rc;
	}
	return 0;
}

/* Boot-only: bring the base capture up once so the board shows a live preview out
   of the box.  Set to 1 after the first autostart; thereafter `gui start`/`stop`
   only toggle the subscription -- the base runs until an explicit `camera stream
   stop` (the user's #100 choice). */
static int base_autostarted;

/* GUIX-thread preview bring-up (GX_EVENT_CAMERA_AUTOSTART).  Also forces the UI
   back to the preview screen on every (re)start -- this runs on the GUIX thread
   after guix_start's gx_widget_show(root) (which would otherwise leave BOTH screens
   visible), so it is the right place to hide settings_screen that the show
   re-exposed (a non-GUIX thread must not touch the widget tree). */
static void camera_ui_autostart(void)
{
	enter_preview();                /* preview shown, settings hidden, B2 per state */
	apply_view_geometry(cur_res_idx);
	if (preview_subscribe(true) != 0)
		return;
	/* Start the base ONCE at boot.  The camera probe (blocking I2C) runs here on the
	   GUIX thread, never before the scheduler.  A failure (no sensor) just leaves the
	   preview subscribed + idle until `camera stream start`. */
	if (!base_autostarted) {
		base_autostarted = 1;
		if (camera_stream_start(0, 0, 0) == 0)
			LOG_INF("base capture on (boot); camera preview on");
		else
			LOG_INF("camera preview subscribed; base off "
			        "(start with 'camera stream start')");
	} else {
		LOG_INF("camera preview subscribed");
	}
}

/* settings: cycle to the next preview resolution.  The base owns the resolution
   now (#100), so change it by stopping the base (cascade -- other subscribers pause
   and re-attach), setting the new res while off, then restarting.  Runs on the GUIX
   thread.  camera_stream_stop() is async, so wait until the producer is idle before
   set_format (else CAM_ERR_BUSY, #69). */
static void cycle_resolution(void)
{
	int next = (cur_res_idx + 1) % N_RES;
	bool was_streaming = camera_streaming() != 0;
	int i;

	if (was_streaming) {
		camera_stream_stop();                 /* cascade */
		for (i = 0; i < 100 && camera_streaming(); i++)
			tx_thread_sleep(1);
		if (camera_streaming()) {
			LOG_WRN("resolution change aborted: base did not stop");
			return;
		}
	}
	if (camera_set_format(res_tbl[next].res, CAM_FMT_RGB565) != 0) {
		LOG_WRN("resolution change: set_format failed");
		if (was_streaming)
			(void)camera_stream_start(0, 0, 0);
		return;
	}
	cur_res_idx = next;
	apply_view_geometry(next);
	gx_text_button_text_set(&btn_res, res_tbl[next].name);
	if (was_streaming && camera_stream_start(0, 0, 0) != 0)
		LOG_WRN("resolution change: base restart failed");
}

/* ---- widget-tree builder (registered with guix_glue) ---------------------- */
static void rect_set(GX_RECTANGLE *r, INT l, INT t, INT rt, INT b)
{
	r->gx_rectangle_left   = (GX_VALUE)l;
	r->gx_rectangle_top    = (GX_VALUE)t;
	r->gx_rectangle_right  = (GX_VALUE)rt;
	r->gx_rectangle_bottom = (GX_VALUE)b;
}

static UINT mk_prompt(GX_PROMPT *p, GX_WIDGET *parent, GX_CONST GX_CHAR *text,
                      INT l, INT t, INT rt, INT b)
{
	GX_RECTANGLE rc;
	UINT status;

	rect_set(&rc, l, t, rt, b);
	status = gx_prompt_create(p, "p", parent, 0, LABEL_STYLE, 0, &rc);
	if (status != GX_SUCCESS)
		return status;
	p->gx_widget_normal_fill_color = C_OVL;
	gx_prompt_text_set(p, text);
	gx_prompt_font_set(p, F_TEXT);
	gx_prompt_text_color_set(p, C_TEXT, C_TEXT, C_TEXT);
	return GX_SUCCESS;
}

static UINT mk_button(GX_TEXT_BUTTON *btn, GX_WIDGET *parent, GX_CONST GX_CHAR *text,
                      USHORT id, INT l, INT t, INT rt, INT b)
{
	GX_RECTANGLE rc;
	UINT status;

	rect_set(&rc, l, t, rt, b);
	status = gx_text_button_create(btn, "b", parent, 0, BUTTON_STYLE, id, &rc);
	if (status != GX_SUCCESS)
		return status;
	gx_text_button_text_set(btn, text);
	gx_text_button_font_set(btn, F_TEXT);
	gx_text_button_text_color_set(btn, C_TEXT, C_TEXT, C_TEXT);
	btn->gx_widget_normal_fill_color   = C_BTN;
	btn->gx_widget_selected_fill_color = C_BTN_SEL;
	return GX_SUCCESS;
}

static UINT mk_numeric(GX_NUMERIC_PROMPT *np, GX_WIDGET *parent, INT val,
                       INT l, INT t, INT rt, INT b)
{
	GX_RECTANGLE rc;
	UINT status;

	rect_set(&rc, l, t, rt, b);
	status = gx_numeric_prompt_create(np, "n", parent, 0, VALUE_STYLE, 0, &rc);
	if (status != GX_SUCCESS)
		return status;
	((GX_PROMPT *)np)->gx_widget_normal_fill_color = C_OVL;
	gx_prompt_font_set((GX_PROMPT *)np, F_TEXT);
	gx_prompt_text_color_set((GX_PROMPT *)np, C_TEXT, C_TEXT, C_TEXT);
	gx_numeric_prompt_value_set(np, val);
	return GX_SUCCESS;
}

/* Seed the current-value caches and widget displays from the camera settings. */
static void controls_sync(void)
{
	struct camera_settings s;

	if (camera_get_settings(&s) != 0)
		memset(&s, 0, sizeof s);

	bip_cur[0] = s.brightness;
	bip_cur[1] = s.contrast;
	bip_cur[2] = s.saturation;
	bip_cur[3] = (int)s.hue * 30;          /* index -> degrees */

	cyc_cur[0] = s.awb;
	cyc_cur[1] = s.effect;
	cyc_cur[2] = s.flip;
	cyc_cur[3] = (s.zoom == 8) ? 3 : (s.zoom == 4) ? 2 : (s.zoom == 2) ? 1 : 0;
	cyc_cur[4] = s.night ? 1 : 0;
}

/* Push the cached current values onto the settings widgets.  Only valid after the
   widgets are created (called from enter_settings, never at first sync). */
static void controls_update_display(void)
{
	int i;

	for (i = 0; i < N_BIP; i++)
		gx_numeric_prompt_value_set(&bip_value[i], bip_cur[i]);
	for (i = 0; i < N_CYC; i++)
		gx_text_button_text_set(&cyc_btn[i], cyc_meta[i].names[cyc_cur[i]]);
	gx_text_button_text_set(&btn_res, res_tbl[cur_res_idx].name);
}

static int camera_ui_build(void *display_v, void *root_v)
{
	GX_DISPLAY     *display = (GX_DISPLAY *)display_v;
	GX_WINDOW_ROOT *root    = (GX_WINDOW_ROOT *)root_v;
	GX_WIDGET      *st;
	GX_RECTANGLE    size;
	UINT            status;
	INT             i, y;

	gx_display_color_table_set(display, (GX_COLOR *)guix_color_table,
	    (INT)(sizeof guix_color_table / sizeof guix_color_table[0]));
	gx_display_font_table_set(display, (GX_FONT **)guix_font_table,
	    (UINT)(sizeof guix_font_table / sizeof guix_font_table[0]));
	guix_pixelmap_table[0]          = GX_NULL;
	guix_pixelmap_table[PIX_CAMERA] = camera_pixmap();
	gx_display_pixelmap_table_set(display, guix_pixelmap_table,
	    (UINT)(sizeof guix_pixelmap_table / sizeof guix_pixelmap_table[0]));

	controls_sync();

	/* ---------------- Preview screen: full panel, black fill ---------------- */
	rect_set(&size, 0, 0, (INT)LTDC_LCD_WIDTH - 1, (INT)LTDC_LCD_HEIGHT - 1);
	status = gx_window_create(&preview_screen, "preview", root,
	                          GX_STYLE_BORDER_NONE | GX_STYLE_ENABLED,
	                          ID_PREVIEW, &size);
	if (status != GX_SUCCESS)
		return (int)status;
	/* Set BOTH normal and selected fill: an ENABLED window is selectable, and
	   gx_window_create defaults selected_fill to GX_COLOR_ID_WINDOW_FILL (2) --
	   which in this custom 5-colour table is C_TEXT (white).  When GUIX draws the
	   screen in its selected state (it is the focused widget) the unset selected
	   fill would otherwise paint the whole background white. */
	preview_screen.gx_widget_normal_fill_color      = C_BG;
	preview_screen.gx_widget_selected_fill_color    = C_BG;
	preview_screen.gx_widget_event_process_function = preview_screen_event;

	/* Camera image (native, centred for the boot resolution; resized per selection
	   by apply_view_geometry).  BORDER_NONE (not ENABLED) so it does not intercept
	   the tap that opens the settings screen. */
	status = gx_icon_create(&cam_icon, "cam", &preview_screen, PIX_CAMERA,
	                        GX_STYLE_BORDER_NONE, ID_CAM_ICON,
	                        RES_VIEW_X(cur_res_idx), RES_VIEW_Y(cur_res_idx));
	if (status != GX_SUCCESS)
		return (int)status;

	/* ---------------- Settings screen: full panel, dark fill ---------------- */
	rect_set(&size, 0, 0, (INT)LTDC_LCD_WIDTH - 1, (INT)LTDC_LCD_HEIGHT - 1);
	status = gx_window_create(&settings_screen, "settings", root,
	                          GX_STYLE_BORDER_NONE | GX_STYLE_ENABLED,
	                          ID_SETTINGS, &size);
	if (status != GX_SUCCESS)
		return (int)status;
	settings_screen.gx_widget_normal_fill_color      = C_OVL;
	settings_screen.gx_widget_selected_fill_color    = C_OVL;  /* see preview note */
	settings_screen.gx_widget_event_process_function = settings_screen_event;
	st = (GX_WIDGET *)&settings_screen;

	status = GX_SUCCESS;
	/* Two columns so the 9 rows are roomy and easy to press: the 4 bipolar
	   controls on the left, the 5 cycle controls on the right.  In each row the
	   label sits immediately left of its control (grouped, not split across the
	   panel).  Row pitch 46 with 32-px-tall widgets (font is 18 px) gives a
	   comfortable touch target and a clear 14-px gap between rows. */
#define ROW_Y(i)  (6 + (i) * 46)
#define ROW_H     32
	/* Left column: bipolar controls -- label | [-] | value | [+]. */
	for (i = 0; i < N_BIP; i++) {
		y = ROW_Y(i);
		status |= mk_prompt(&bip_label[i], st, bip_meta[i].label,
		                    6, y, 112, y + ROW_H);
		status |= mk_button(&bip_minus[i], st, "-",
		                    (USHORT)(ID_BIP_MINUS + i), 116, y, 148, y + ROW_H);
		status |= mk_numeric(&bip_value[i], st, bip_cur[i], 152, y, 184, y + ROW_H);
		status |= mk_button(&bip_plus[i], st, "+",
		                    (USHORT)(ID_BIP_PLUS + i), 188, y, 220, y + ROW_H);
	}
	/* Right column: cycle controls -- label | value-button. */
	for (i = 0; i < N_CYC; i++) {
		y = ROW_Y(i);
		status |= mk_prompt(&cyc_label[i], st, cyc_meta[i].label,
		                    242, y, 366, y + ROW_H);
		status |= mk_button(&cyc_btn[i], st, cyc_meta[i].names[cyc_cur[i]],
		                    (USHORT)(ID_CYC + i), 370, y, 476, y + ROW_H);
	}
	/* Left column row 4 (below the 4 bipolar controls): preview resolution (#69).
	   A dedicated control, not a cyc_meta entry -- changing resolution needs
	   stop -> set_format -> restart (a live re-format is BUSY), not a live setter. */
	y = ROW_Y(N_BIP);
	status |= mk_prompt(&res_label, st, "Resolution", 6, y, 112, y + ROW_H);
	status |= mk_button(&btn_res, st, res_tbl[cur_res_idx].name,
	                    ID_RES, 116, y, 220, y + ROW_H);
	/* Back centred along the bottom. */
	status |= mk_button(&btn_back, st, "Back", ID_BACK, 190, 230, 290, 266);
#undef ROW_Y
#undef ROW_H
	if (status != GX_SUCCESS)
		return (int)status;

	/* Route the camera control events through the root handler. */
	root->gx_widget_event_process_function = camera_ui_root_event;
	/* Visibility is left to the AUTOSTART handler: it hides settings_screen (that
	   gx_widget_show(root) would otherwise leave visible) on the GUIX thread after
	   the show, so the clip is computed against a visible root (#55 idiom). */
	return 0;
}

/* ---- public lifecycle ----------------------------------------------------- */

void camera_ui_init(void)
{
	guix_register_app_builder(camera_ui_build);
}

int camera_ui_start(void)
{
	int rc = guix_start();

	if (rc != GUIX_OK)
		return rc;
	stop_requested = 0;
	if (guix_post_root_event(GX_EVENT_CAMERA_AUTOSTART) != GUIX_OK)
		LOG_WRN("camera autostart post failed; use 'gui start' to retry");
	return GUIX_OK;
}

int camera_ui_stop(void)
{
	/* Unsubscribe the preview from the base (the base keeps running -- `gui stop` is
	   a subscription toggle, not a base stop, #100) and blank the screen.  The AI
	   subscriber is independent: it keeps running (bbox just stops being drawn). */
	stop_requested = 1;
	preview_teardown();
	(void)guix_stop();
	return 0;
}
