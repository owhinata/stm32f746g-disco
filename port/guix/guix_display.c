/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    guix_display.c
 * @brief   GUIX 565rgb display driver on the LTDC double buffer (issue #55).
 *
 * See guix_display.h for the design.  Key facts verified against GUIX v6.5.1:
 *   - The 565rgb software driver writes the *native* 16-bit colour straight to
 *     memory, so the GX_COLOR handed to horizontal_line_draw is already a packed
 *     RGB565 value in its low 16 bits.  For a DMA2D register-to-memory fill it
 *     must still be expanded to ARGB8888 -- the F7 HAL interprets the R2M colour
 *     as ARGB8888 and re-packs it to the output format (same quirk as #53).
 *   - gx_draw_context_pitch is in PIXELS (it is set from canvas->
 *     gx_canvas_x_resolution and used as a USHORT* row increment), not bytes.
 *   - GX_RECTANGLE edges are all inclusive.
 *   - gx_canvas_memory is a GX_COLOR* the 565 driver casts to USHORT*; repointing
 *     it between the two LTDC buffers in the toggle is exactly how GUIX hardware
 *     double buffering is meant to work.
 */
#include "guix_display.h"        /* CAM_VIEW_* preview geometry (#59 B2) */
#include "ltdc_display.h"

#include "gx_display.h"          /* _gx_display_driver_565rgb_setup proto */

#include "stm32f7xx_hal.h"

#define LOG_TAG "guix"
#include "log.h"

#include <stdint.h>
#include <string.h>             /* memcpy: corrective camera-rect CPU fallback (#59) */

/* DMA2D handle private to GUIX.  DMA2D is one engine shared with ltdc_display.c;
   we serialize on ltdc_lock (ltdc_lock_frame/unlock_frame) and fully reconfigure
   the engine each op, so a separate handle is safe.  The DMA2D clock is enabled
   by ltdc_init(), which always runs before GUIX starts. */
static DMA2D_HandleTypeDef hdma2d_gui;

/* Saved software driver pointers we fall back to for cases DMA2D will not take
   (partially-transparent brush, compressed/palette/alpha-plane pixelmaps,
   overlapping block moves, cacheable-SRAM sources).  Captured in
   guix_display_driver_setup right after _gx_display_driver_565rgb_setup(). */
static VOID (*sw_horizontal_line_draw)(GX_DRAW_CONTEXT *context, INT x1, INT x2,
                                       INT ypos, INT width, GX_COLOR color);
static VOID (*sw_pixelmap_draw)(GX_DRAW_CONTEXT *context, INT x, INT y,
                                GX_PIXELMAP *pmp);
static VOID (*sw_pixelmap_blend)(GX_DRAW_CONTEXT *context, INT x, INT y,
                                 GX_PIXELMAP *pmp, GX_UBYTE alpha);
static VOID (*sw_canvas_copy)(GX_CANVAS *canvas, GX_CANVAS *composite);
static VOID (*sw_canvas_blend)(GX_CANVAS *canvas, GX_CANVAS *composite);

/* GUIX rectangle utilities used by the canvas composite overrides. */
extern UINT    _gx_utility_rectangle_shift(GX_RECTANGLE *r, GX_VALUE dx, GX_VALUE dy);
extern GX_BOOL _gx_utility_rectangle_overlap_detect(GX_RECTANGLE *a, GX_RECTANGLE *b,
                                                    GX_RECTANGLE *overlap);

/* DMA2D may read a source only where the data is guaranteed coherent without a
   cache clean: the MPU non-cacheable SDRAM (frame buffers, camera frames,
   GUIX canvases placed there) or read-only Flash (const Studio pixelmaps).  A
   pixelmap in cacheable, CPU-written SRAM could have dirty D-cache lines DMA2D
   would miss, so those fall back to the software path. */
static bool guix_dma2d_src_ok(const void *p)
{
	uintptr_t a = (uintptr_t)p;

	if (a >= 0xC0000000u && a < 0xC0800000u)   /* external SDRAM (non-cacheable) */
		return true;
	if (a >= 0x08000000u && a < 0x08100000u)   /* internal Flash (read-only)     */
		return true;
	return false;
}

/* RGB565 (low 16 bits) -> ARGB8888 (0x00RRGGBB) for DMA2D R2M (see header / #53). */
static uint32_t rgb565_to_argb8888(uint16_t c)
{
	uint32_t r = (uint32_t)(c >> 11) & 0x1Fu;
	uint32_t g = (uint32_t)(c >> 5) & 0x3Fu;
	uint32_t b = (uint32_t)c & 0x1Fu;

	r = (r << 3) | (r >> 2);          /* 5 -> 8 */
	g = (g << 2) | (g >> 4);          /* 6 -> 8 */
	b = (b << 3) | (b >> 2);          /* 5 -> 8 */
	return (r << 16) | (g << 8) | b;
}

/* Run an already-configured 2-operand DMA2D transfer on hdma2d_gui (R2M fill or
   M2M blit -- same HAL_DMA2D_Start/_Start_IT signature) to completion: large
   transfers block on the completion IRQ, small ones poll (#64; threshold
   LTDC_DMA2D_IT_MIN_PIXELS).  Guarantees DMA2D is left IDLE on failure -- the
   camera sink's CPU fallback (cam_rect_refresh) relies on it.  Caller holds
   ltdc_lock.  Returns true on a clean completion. */
static bool guix_dma2d_run(uint32_t pdata, uint32_t dst, uint32_t w, uint32_t h)
{
	if ((uint64_t)w * h >= LTDC_DMA2D_IT_MIN_PIXELS) {
		ltdc_dma2d_arm_it(&hdma2d_gui);
		if (HAL_DMA2D_Start_IT(&hdma2d_gui, pdata, dst, w, h) != HAL_OK) {
			ltdc_dma2d_disarm_it(&hdma2d_gui);    /* leaves the engine idle */
			return false;
		}
		return ltdc_dma2d_wait_it(&hdma2d_gui, 30);   /* disarms -> idle */
	}
	if (HAL_DMA2D_Start(&hdma2d_gui, pdata, dst, w, h) != HAL_OK)
		return false;
	if (HAL_DMA2D_PollForTransfer(&hdma2d_gui, 30) == HAL_OK)
		return true;
	/* Poll timeout returns without clearing CR.START -- the engine may still be
	   writing the destination.  Abort so a failed transfer always leaves DMA2D
	   idle (cam_rect_refresh's CPU follow-up relies on it, and it keeps the next
	   op's re-init clean, #59). */
	if ((DMA2D->CR & DMA2D_CR_START) != 0u)
		(void)HAL_DMA2D_Abort(&hdma2d_gui);
	return false;
}

/* DMA2D register-to-memory fill of a w x h RGB565 block; @p line_off u16 skipped
   at each row end (= dst stride in px - w).  Serialized on ltdc_lock. */
static void guix_dma2d_fill(uint16_t *dst, uint16_t color, uint32_t w,
                            uint32_t h, uint32_t line_off)
{
	if (dst == NULL || w == 0u || h == 0u)
		return;

	ltdc_lock_frame();
	hdma2d_gui.Instance          = DMA2D;
	hdma2d_gui.Init.Mode         = DMA2D_R2M;
	hdma2d_gui.Init.ColorMode    = DMA2D_OUTPUT_RGB565;
	hdma2d_gui.Init.OutputOffset = line_off;
	if (HAL_DMA2D_Init(&hdma2d_gui) == HAL_OK &&
	    HAL_DMA2D_ConfigLayer(&hdma2d_gui, 1) == HAL_OK)
		(void)guix_dma2d_run(rgb565_to_argb8888(color),
		                     (uint32_t)(uintptr_t)dst, w, h);
	ltdc_unlock_frame();
}

/* DMA2D memory-to-memory copy of a w x h RGB565 block with separate u16 row
   offsets for dst/src.  Source and destination must NOT overlap.  Serialized on
   ltdc_lock.  Returns 0 on a completed transfer, -1 on bad args or any DMA2D
   failure (init/config/start/poll) -- the camera preview sink needs the status
   to keep the last good frame on failure; the GUIX draw paths ignore it. */
static int guix_dma2d_blit(uint16_t *dst, const uint16_t *src, uint32_t w,
                           uint32_t h, uint32_t dst_off, uint32_t src_off)
{
	int rc = -1;

	if (dst == NULL || src == NULL || w == 0u || h == 0u)
		return -1;

	ltdc_lock_frame();
	hdma2d_gui.Instance          = DMA2D;
	hdma2d_gui.Init.Mode         = DMA2D_M2M;
	hdma2d_gui.Init.ColorMode    = DMA2D_OUTPUT_RGB565;
	hdma2d_gui.Init.OutputOffset = dst_off;
	hdma2d_gui.LayerCfg[1].AlphaMode      = DMA2D_NO_MODIF_ALPHA;
	hdma2d_gui.LayerCfg[1].InputAlpha     = 0xFFu;
	hdma2d_gui.LayerCfg[1].InputColorMode = DMA2D_INPUT_RGB565;
	hdma2d_gui.LayerCfg[1].InputOffset    = src_off;
	/* guix_dma2d_run completes the transfer (IT for large, poll for small) and
	   leaves DMA2D idle on failure, so cam_rect_refresh's CPU fallback is safe. */
	if (HAL_DMA2D_Init(&hdma2d_gui) == HAL_OK &&
	    HAL_DMA2D_ConfigLayer(&hdma2d_gui, 1) == HAL_OK &&
	    guix_dma2d_run((uint32_t)(uintptr_t)src, (uint32_t)(uintptr_t)dst, w, h))
		rc = 0;
	ltdc_unlock_frame();
	return rc;
}

/* DMA2D memory-to-memory-with-blending of a w x h block over an RGB565
   destination (read-modify-write).  Foreground (layer 1) is @p src in @p fg_cmode
   with alpha mode @p fg_amode / @p fg_alpha; background (layer 0) is the RGB565
   destination itself; output is RGB565.  Serialized on ltdc_lock.  RM0385 §9.3.6
   (blender) + §9.3.5 (alpha modes) + §9.3.7 (output PFC). */
static void guix_dma2d_blend(uint16_t *dst, const void *src, uint32_t w,
                             uint32_t h, uint32_t dst_off, uint32_t src_off,
                             uint32_t fg_cmode, uint32_t fg_amode, uint8_t fg_alpha)
{
	if (dst == NULL || src == NULL || w == 0u || h == 0u)
		return;

	ltdc_lock_frame();
	hdma2d_gui.Instance          = DMA2D;
	hdma2d_gui.Init.Mode         = DMA2D_M2M_BLEND;
	hdma2d_gui.Init.ColorMode    = DMA2D_OUTPUT_RGB565;
	hdma2d_gui.Init.OutputOffset = dst_off;
	/* Foreground = source. */
	hdma2d_gui.LayerCfg[1].AlphaMode      = fg_amode;
	hdma2d_gui.LayerCfg[1].InputAlpha     = fg_alpha;
	hdma2d_gui.LayerCfg[1].InputColorMode = fg_cmode;
	hdma2d_gui.LayerCfg[1].InputOffset    = src_off;
	/* Background = destination (read-modify-write), same offset as output. */
	hdma2d_gui.LayerCfg[0].AlphaMode      = DMA2D_NO_MODIF_ALPHA;
	hdma2d_gui.LayerCfg[0].InputAlpha     = 0xFFu;
	hdma2d_gui.LayerCfg[0].InputColorMode = DMA2D_INPUT_RGB565;
	hdma2d_gui.LayerCfg[0].InputOffset    = dst_off;
	if (HAL_DMA2D_Init(&hdma2d_gui) == HAL_OK &&
	    HAL_DMA2D_ConfigLayer(&hdma2d_gui, 0) == HAL_OK &&
	    HAL_DMA2D_ConfigLayer(&hdma2d_gui, 1) == HAL_OK) {
		/* Blend has three operands (BlendingStart), so it cannot use
		   guix_dma2d_run; inline the same size-thresholded poll/IT choice (#64).
		   Blends are icons/overlays, almost always below the threshold -> poll. */
		if ((uint64_t)w * h >= LTDC_DMA2D_IT_MIN_PIXELS) {
			ltdc_dma2d_arm_it(&hdma2d_gui);
			if (HAL_DMA2D_BlendingStart_IT(&hdma2d_gui,
			        (uint32_t)(uintptr_t)src, (uint32_t)(uintptr_t)dst,
			        (uint32_t)(uintptr_t)dst, w, h) == HAL_OK)
				(void)ltdc_dma2d_wait_it(&hdma2d_gui, 30);
			else
				ltdc_dma2d_disarm_it(&hdma2d_gui);
		} else if (HAL_DMA2D_BlendingStart(&hdma2d_gui,
		               (uint32_t)(uintptr_t)src, (uint32_t)(uintptr_t)dst,
		               (uint32_t)(uintptr_t)dst, w, h) == HAL_OK) {
			HAL_DMA2D_PollForTransfer(&hdma2d_gui, 30);
		}
	}
	ltdc_unlock_frame();
}

void guix_display_fill_back(uint16_t rgb565)
{
	uint16_t *back = ltdc_back_buffer();

	if (back != NULL)
		guix_dma2d_fill(back, rgb565, LTDC_LCD_WIDTH, LTDC_LCD_HEIGHT, 0u);
}

int guix_display_copy_rgb565(uint16_t *dst, const uint16_t *src, uint32_t w,
                             uint32_t h, uint32_t dst_off, uint32_t src_off)
{
	return guix_dma2d_blit(dst, src, w, h, dst_off, src_off);
}

/*
 * DMA2D-accelerated horizontal line / solid fill.  GUIX fills widget rectangles
 * by calling this once per scanline-band; @p width is the band height.  The
 * software driver pre-clips, so the inputs are already inside the canvas -- we
 * mirror its contract exactly and only swap the inner pixel loop for a DMA2D R2M
 * rectangle fill.  A partially transparent brush (alpha != 0xff) falls back to
 * the saved software routine, which does the per-pixel blend.
 */
static VOID guix_horizontal_line_draw(GX_DRAW_CONTEXT *context, INT xstart,
                                      INT xend, INT ypos, INT width,
                                      GX_COLOR color)
{
	INT len   = xend - xstart + 1;
	INT pitch = context->gx_draw_context_pitch;   /* pixels per row */
	uint16_t *dst;

#if defined(GX_BRUSH_ALPHA_SUPPORT)
	GX_UBYTE alpha = context->gx_draw_context_brush.gx_brush_alpha;

	if (alpha == 0)
		return;                                /* fully transparent: nothing */
	if (alpha != 0xffu) {
		sw_horizontal_line_draw(context, xstart, xend, ypos, width, color);
		return;                                /* per-pixel blend in software */
	}
#endif
	if (len <= 0 || width <= 0)
		return;

	dst = (uint16_t *)context->gx_draw_context_memory +
	      (uint32_t)ypos * (uint32_t)pitch + (uint32_t)xstart;
	guix_dma2d_fill(dst, (uint16_t)color, (uint32_t)len, (uint32_t)width,
	                (uint32_t)(pitch - len));
}

/* ===== #59 Lever B2: camera live-preview copy-forward elimination ===========
 *
 * The camera icon (CAM_VIEW_W x CAM_VIEW_H at CAM_VIEW_X,CAM_VIEW_Y) is fully
 * repainted from cam_view_buf every preview frame, so forwarding it in the
 * buffer toggle is wasted SDRAM bandwidth (one ~150 KB DMA2D blit per displayed
 * frame) and a contributor to the DCMI DMA FIFO errors of #59.  We skip that
 * copy but must never present a STALE camera rect (a later non-camera flip could
 * otherwise show an older frame -> visible regression).  Correctness is enforced
 * at PRESENT time by a per-buffer freshness flag:
 *
 *   cam_buf_stale[i] == false   <=>   ltdc_fb[i]'s camera rect already holds the
 *                                     latest cam_view_buf content.
 *
 *   - a new view frame (guix_display_cam_view_store) makes BOTH buffers stale;
 *   - a full-rect camera pixelmap draw clears the drawn (back) buffer;
 *   - before each flip, if the buffer about to be presented is still stale we do
 *     a corrective view->buffer copy first.
 *
 * Every stale-flag transition is kept in the SAME ltdc_lock section as the DMA2D
 * copy it reflects, so the producer thread cannot advance cam_view_buf between a
 * copy and its flag update (a false-negative would present stale pixels). */
static bool      cam_preview_active;   /* session armed (begin..end): store() ok  */
static bool      cam_visible;          /* camera screen on display (SHOW..HIDE):
                                          gates the toggle's corrective + skip so
                                          we never stamp camera pixels onto, or
                                          drop a forward-copy on, another screen  */
static uint16_t *cam_view_data;        /* cam_view_buf base (registered by begin) */
static bool      cam_buf_stale[2];     /* per LTDC buffer; invariant above        */
/* Camera icon geometry on the panel (set by begin(), #69 variable resolution).
   Defaults to the QVGA centred placement; rect/blit helpers read these instead of
   the compile-time CAM_VIEW_* constants so the preview can run at qqvga/qvga/
   480x272.  CAM_RECT bounds are cam_x..cam_x+cam_w-1 / cam_y..cam_y+cam_h-1. */
static uint16_t  cam_x = CAM_VIEW_X, cam_y = CAM_VIEW_Y;
static uint16_t  cam_w = CAM_VIEW_W, cam_h = CAM_VIEW_H;

/* True if screen-space rect r lies fully inside the camera icon rect. */
static bool rect_in_cam(const GX_RECTANGLE *r)
{
	return r->gx_rectangle_left   >= (GX_VALUE)cam_x &&
	       r->gx_rectangle_top    >= (GX_VALUE)cam_y &&
	       r->gx_rectangle_right  <= (GX_VALUE)(cam_x + cam_w - 1) &&
	       r->gx_rectangle_bottom <= (GX_VALUE)(cam_y + cam_h - 1);
}

/* True if screen-space clip rect r fully COVERS the camera icon rect. */
static bool rect_covers_cam(const GX_RECTANGLE *r)
{
	return r->gx_rectangle_left   <= (GX_VALUE)cam_x &&
	       r->gx_rectangle_top    <= (GX_VALUE)cam_y &&
	       r->gx_rectangle_right  >= (GX_VALUE)(cam_x + cam_w - 1) &&
	       r->gx_rectangle_bottom >= (GX_VALUE)(cam_y + cam_h - 1);
}

/* Corrective copy of the full camera rect (cam_view_buf -> @p fb at the icon
   position).  Caller holds ltdc_lock.  Tries DMA2D first and, on the rare DMA2D
   failure, falls back to a row-by-row CPU copy so the presented camera rect is
   ALWAYS made current -- the toggle therefore never has to skip the flip (which
   would strand that cycle's non-camera draws).  Both buffers are MPU
   non-cacheable SDRAM, so the CPU copy is coherent with no cache maintenance.
   Returns 0 when the rect was refreshed, -1 only on a NULL buffer (cannot happen
   while cam_visible, since begin() set cam_view_data). */
static int cam_rect_refresh(uint16_t *fb)
{
	uint16_t *dst;
	const uint16_t *src;
	uint32_t y;

	if (fb == NULL || cam_view_data == NULL)
		return -1;
	dst = fb + (uint32_t)cam_y * LTDC_LCD_WIDTH + cam_x;
	if (guix_dma2d_blit(dst, cam_view_data, cam_w, cam_h,
	                    LTDC_LCD_WIDTH - cam_w, 0u) == 0)
		return 0;
	/* DMA2D failed.  guix_dma2d_blit aborts a timed-out transfer, so the engine
	   is normally idle now; only run the CPU copy once that is confirmed (START
	   clear).  If the abort itself failed the engine is wedged (hard DMA2D
	   fault) -- do NOT race it with a CPU write; report failure and leave the
	   buffer stale. */
	if ((DMA2D->CR & DMA2D_CR_START) != 0u)
		return -1;
	src = cam_view_data;
	for (y = 0; y < cam_h; y++) {
		memcpy(dst, src, (size_t)cam_w * sizeof(uint16_t));
		dst += LTDC_LCD_WIDTH;
		src += cam_w;
	}
	return 0;
}

/* Register the view buffer + camera-rect geometry and arm B2 for a preview
   session (#69 variable resolution).  Both LTDC buffers start stale so the first
   frames are corrected before present.  Returns 0 on success, -1 if the geometry
   is invalid (NULL buffer, zero size, or out of the 480x272 panel) -- in which
   case B2 is left DISARMED (cam_view_data NULL) so a bad offset can never reach
   the DMA2D blits.  Runs under ltdc_lock. */
int guix_display_cam_preview_begin(uint16_t *view_buf, uint16_t w, uint16_t h,
                                   uint16_t x, uint16_t y)
{
	if (view_buf == NULL || w == 0u || h == 0u ||
	    (uint32_t)x + w > LTDC_LCD_WIDTH || (uint32_t)y + h > LTDC_LCD_HEIGHT)
		return -1;
	ltdc_lock_frame();
	cam_view_data      = view_buf;
	cam_x              = x;
	cam_y              = y;
	cam_w              = w;
	cam_h              = h;
	cam_buf_stale[0]   = true;
	cam_buf_stale[1]   = true;
	cam_preview_active = true;
	ltdc_unlock_frame();
	return 0;
}

void guix_display_cam_preview_end(void)
{
	ltdc_lock_frame();
	cam_preview_active = false;
	cam_visible        = false;
	cam_view_data      = NULL;
	ltdc_unlock_frame();
}

/* Gate the buffer-toggle B2 paths to when the camera screen is actually on
   display (set by the SHOW/HIDE root events on the GUIX thread).  Turning it on
   marks both buffers stale so the first presented frame is corrected -- the
   camera rect currently holds whatever the previous screen drew there. */
void guix_display_cam_set_visible(bool on)
{
	ltdc_lock_frame();
	cam_visible = on;
	if (on) {
		cam_buf_stale[0] = true;
		cam_buf_stale[1] = true;
	}
	ltdc_unlock_frame();
}

/* Store a freshly captured frame (cam_w x cam_h RGB565, the current preview
   resolution #69) into the view buffer and mark BOTH LTDC buffers stale,
   atomically under ltdc_lock (#59 B2).  Returns 0 on a completed DMA2D copy,
   -1 otherwise. */
int guix_display_cam_view_store(const uint16_t *src)
{
	int rc;

	ltdc_lock_frame();
	if (cam_view_data == NULL || !cam_preview_active) {
		ltdc_unlock_frame();
		return -1;
	}
	rc = guix_dma2d_blit(cam_view_data, src, cam_w, cam_h, 0u, 0u);
	if (rc == 0) {
		cam_buf_stale[0] = true;
		cam_buf_stale[1] = true;
	}
	ltdc_unlock_frame();
	return rc;
}

/* Fill the inclusive rectangle [x0,x1]x[y0,y1] (clamped to the view buffer wxh) with
 * @p c in cam_view_data.  Caller holds ltdc_lock and has checked cam_view_data.  The
 * view buffer is packed at cam_w stride starting at pixel (0,0). */
static void cam_view_fill(int x0, int y0, int x1, int y1, int w, int h, uint16_t c)
{
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > w - 1) x1 = w - 1;
	if (y1 > h - 1) y1 = h - 1;
	for (int yy = y0; yy <= y1; yy++) {
		uint16_t *row = cam_view_data + (uint32_t)yy * (uint32_t)w;
		for (int xx = x0; xx <= x1; xx++)
			row[xx] = c;
	}
}

/* Draw a 2-px RGB565 rectangle outline into the camera view buffer, scaled from a
 * normalized [0,1] box (issue #83).  See the header.  The box rides the view buffer,
 * so it is composited into the live image via the same B2 paths as the frame and is
 * overwritten by the next store.  cam_view_buf is non-cacheable SDRAM, so these CPU
 * writes are coherent with the DMA2D reads that later blit it. */
void guix_display_cam_overlay_box(float nx, float ny, float nw, float nh,
                                  uint16_t rgb565)
{
	const int T = 2;                        /* outline thickness (px)                */

	ltdc_lock_frame();
	if (cam_view_data != NULL && cam_w > 0u && cam_h > 0u) {
		int w = (int)cam_w, h = (int)cam_h;
		int l = (int)(nx * (float)w);
		int t = (int)(ny * (float)h);
		int r = (int)((nx + nw) * (float)w);
		int b = (int)((ny + nh) * (float)h);

		if (l < 0) l = 0;
		if (t < 0) t = 0;
		if (r > w - 1) r = w - 1;
		if (b > h - 1) b = h - 1;
		if (r >= l && b >= t) {
			/* Four edge bands (corners overwritten twice, same colour). */
			cam_view_fill(l, t, r, t + T - 1, w, h, rgb565);   /* top    */
			cam_view_fill(l, b - T + 1, r, b, w, h, rgb565);   /* bottom */
			cam_view_fill(l, t, l + T - 1, b, w, h, rgb565);   /* left   */
			cam_view_fill(r - T + 1, t, r, b, w, h, rgb565);   /* right  */
			cam_buf_stale[0] = true;
			cam_buf_stale[1] = true;
		}
	}
	ltdc_unlock_frame();
}

/*
 * Tear-free buffer toggle (GUIX calls this once per refreshed frame).  GUIX has
 * just composited the accumulated dirty region into the back buffer (= the
 * canvas memory).  Present it, repoint the canvas at the new back buffer, and
 * copy the dirty rectangle forward from the just-presented front into the new
 * back so BOTH buffers stay identical -- next frame GUIX again only redraws the
 * changed region on top of correct prior content.  Both buffers start cleared to
 * black (ltdc_init), so the "identical everywhere outside the running dirty
 * set" invariant holds from the first frame.
 */
static VOID guix_buffer_toggle(GX_CANVAS *canvas, GX_RECTANGLE *dirty)
{
	uint16_t *front, *back;
	INT x, y, right, bottom, w, h;

	ltdc_lock_frame();

	/* The canvas must be on the current back buffer; if not, ownership was
	   broken (should never happen behind ltdc_gui_take).  Be defensive: skip. */
	if ((uint16_t *)canvas->gx_canvas_memory != ltdc_back_buffer()) {
		LOG_WRN("buffer toggle: canvas not on back buffer, skipping flip");
		ltdc_unlock_frame();
		return;
	}

	/* #59 B2: ensure the buffer about to be presented holds the latest camera
	   frame BEFORE the flip.  In steady-state preview the full-rect pixelmap
	   draw already cleared this buffer's stale flag, so the corrective copy only
	   fires on a non-camera cycle whose camera rect has fallen behind. */
	if (cam_visible) {
		uint8_t cur = (uint8_t)!ltdc_active_buffer();

		/* Make the buffer about to be presented hold the latest camera frame.
		   cam_rect_refresh falls back to a CPU copy if DMA2D fails, so it
		   succeeds whenever the view buffer is valid; clear stale only then.  We
		   never skip the flip here -- doing so would strand this cycle's
		   non-camera draws in the back buffer, and the normal copy-forward below
		   keeps the two buffers consistent outside the camera rect (#59). */
		if (cam_buf_stale[cur] &&
		    cam_rect_refresh((uint16_t *)canvas->gx_canvas_memory) == 0)
			cam_buf_stale[cur] = false;
	}

	if (ltdc_gui_flip() != LTDC_OK || !ltdc_is_up()) {
		ltdc_unlock_frame();
		return;                       /* display faulted: leave it be */
	}

	/* Repoint the canvas at the freshly-freed back buffer. */
	back  = ltdc_back_buffer();
	front = ltdc_framebuffer();       /* what we just presented */
	canvas->gx_canvas_memory = (GX_COLOR *)back;

	/* #59 B2: when the live preview is on and the whole dirty region is the
	   camera icon, skip the copy-forward -- that rect is repainted from
	   cam_view_buf every frame and its two-buffer consistency is maintained by
	   the stale tracking above, so forwarding it is wasted bandwidth. */
	if (cam_visible && rect_in_cam(dirty)) {
		ltdc_unlock_frame();
		return;
	}

	/* Copy the dirty rectangle forward (front -> new back), clipped to panel. */
	x      = dirty->gx_rectangle_left;
	y      = dirty->gx_rectangle_top;
	right  = dirty->gx_rectangle_right;
	bottom = dirty->gx_rectangle_bottom;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (right > (INT)LTDC_LCD_WIDTH - 1)
		right = (INT)LTDC_LCD_WIDTH - 1;
	if (bottom > (INT)LTDC_LCD_HEIGHT - 1)
		bottom = (INT)LTDC_LCD_HEIGHT - 1;
	w = right - x + 1;
	h = bottom - y + 1;
	if (w > 0 && h > 0 && front != NULL && back != NULL) {
		uint32_t off = (uint32_t)y * LTDC_LCD_WIDTH + (uint32_t)x;

		guix_dma2d_blit(back + off, front + off, (uint32_t)w, (uint32_t)h,
		                LTDC_LCD_WIDTH - (uint32_t)w,
		                LTDC_LCD_WIDTH - (uint32_t)w);
	}

	ltdc_unlock_frame();
}

/*
 * DMA2D pixelmap draw (uncompressed, opaque RGB565 only).  GUIX pre-clips, so the
 * clip rectangle bounds the on-screen region; the source starts at the matching
 * offset inside the pixelmap.  Everything else (compressed, alpha-plane,
 * transparent, palette, ARGB formats, partial brush alpha, cacheable-SRAM
 * source) defers to the software 565rgb driver.  This is the path #56 uses to
 * blit camera frames (RGB565 in SDRAM). RM0385 §9.3.3 (M2M).
 */
static VOID guix_pixelmap_draw(GX_DRAW_CONTEXT *context, INT xpos, INT ypos,
                               GX_PIXELMAP *pmp)
{
	GX_RECTANGLE *clip = context->gx_draw_context_clip;
	INT pitch = context->gx_draw_context_pitch;
	INT w, h;

#if defined(GX_BRUSH_ALPHA_SUPPORT)
	if (context->gx_draw_context_brush.gx_brush_alpha != 0xffu) {
		sw_pixelmap_draw(context, xpos, ypos, pmp);
		return;
	}
#endif
	if (pmp->gx_pixelmap_format != GX_COLOR_FORMAT_565RGB ||
	    (pmp->gx_pixelmap_flags & (GX_PIXELMAP_COMPRESSED | GX_PIXELMAP_ALPHA |
	                               GX_PIXELMAP_TRANSPARENT)) ||
	    !guix_dma2d_src_ok(pmp->gx_pixelmap_data)) {
		sw_pixelmap_draw(context, xpos, ypos, pmp);
		return;
	}
	w = clip->gx_rectangle_right - clip->gx_rectangle_left + 1;
	h = clip->gx_rectangle_bottom - clip->gx_rectangle_top + 1;
	if (w <= 0 || h <= 0)
		return;
	{
		uint16_t *dst = (uint16_t *)context->gx_draw_context_memory +
		                (uint32_t)clip->gx_rectangle_top * (uint32_t)pitch +
		                (uint32_t)clip->gx_rectangle_left;
		const uint16_t *src = (const uint16_t *)pmp->gx_pixelmap_data +
		                (uint32_t)(clip->gx_rectangle_top - ypos) *
		                    (uint32_t)pmp->gx_pixelmap_width +
		                (uint32_t)(clip->gx_rectangle_left - xpos);
		uint32_t dst_off = (uint32_t)(pitch - w);
		uint32_t src_off = (uint32_t)(pmp->gx_pixelmap_width - w);

		/* #59 B2: the camera live-preview icon draws cam_view_buf into the back
		   buffer.  Keep the blit and the stale-flag clear in one ltdc_lock
		   section so the producer cannot advance cam_view_buf in between (a
		   false-negative would later present stale pixels).  Only a clip that
		   fully covers the icon rect counts as a complete repaint. */
		if (cam_preview_active &&
		    (uint16_t *)pmp->gx_pixelmap_data == cam_view_data) {
			ltdc_lock_frame();
			/* Clear the stale flag only when the copy actually COMPLETED and it
			   covered the whole icon rect -- otherwise the buffer's camera rect
			   is not really the latest view and must stay stale (#59). */
			if (guix_dma2d_blit(dst, src, (uint32_t)w, (uint32_t)h,
			                    dst_off, src_off) == 0 && rect_covers_cam(clip))
				cam_buf_stale[!ltdc_active_buffer()] = false;
			ltdc_unlock_frame();
		} else {
			(void)guix_dma2d_blit(dst, src, (uint32_t)w, (uint32_t)h,
			                      dst_off, src_off);
		}
	}
}

/*
 * DMA2D pixelmap blend.  Accelerates an opaque RGB565 image blended at a global
 * alpha (FG RGB565, replace-alpha = global) and an ARGB4444 image with per-pixel
 * alpha optionally scaled by the global alpha (combine-alpha).  565-with-separate-
 * alpha-plane, compressed, palette and cacheable-SRAM sources defer to software.
 * RM0385 §9.3.6 (blender) / §9.3.5 (alpha modes).
 */
static VOID guix_pixelmap_blend(GX_DRAW_CONTEXT *context, INT xpos, INT ypos,
                                GX_PIXELMAP *pmp, GX_UBYTE alpha)
{
	GX_RECTANGLE *clip = context->gx_draw_context_clip;
	INT pitch = context->gx_draw_context_pitch;
	uint32_t cmode, amode;
	INT w, h;

	if ((pmp->gx_pixelmap_flags & GX_PIXELMAP_COMPRESSED) ||
	    !guix_dma2d_src_ok(pmp->gx_pixelmap_data)) {
		sw_pixelmap_blend(context, xpos, ypos, pmp, alpha);
		return;
	}
	if (pmp->gx_pixelmap_format == GX_COLOR_FORMAT_565RGB &&
	    !(pmp->gx_pixelmap_flags & GX_PIXELMAP_ALPHA)) {
		cmode = DMA2D_INPUT_RGB565;
		amode = DMA2D_REPLACE_ALPHA;            /* whole image at global alpha */
	} else if (pmp->gx_pixelmap_format == GX_COLOR_FORMAT_4444ARGB) {
		cmode = DMA2D_INPUT_ARGB4444;
		amode = DMA2D_COMBINE_ALPHA;            /* per-pixel * global alpha     */
	} else {
		sw_pixelmap_blend(context, xpos, ypos, pmp, alpha);
		return;
	}
	w = clip->gx_rectangle_right - clip->gx_rectangle_left + 1;
	h = clip->gx_rectangle_bottom - clip->gx_rectangle_top + 1;
	if (w <= 0 || h <= 0)
		return;
	guix_dma2d_blend((uint16_t *)context->gx_draw_context_memory +
	                     (uint32_t)clip->gx_rectangle_top * (uint32_t)pitch +
	                     (uint32_t)clip->gx_rectangle_left,
	                 (const uint16_t *)pmp->gx_pixelmap_data +
	                     (uint32_t)(clip->gx_rectangle_top - ypos) *
	                         (uint32_t)pmp->gx_pixelmap_width +
	                     (uint32_t)(clip->gx_rectangle_left - xpos),
	                 (uint32_t)w, (uint32_t)h,
	                 (uint32_t)(pitch - w),
	                 (uint32_t)(pmp->gx_pixelmap_width - w),
	                 cmode, amode, alpha);
}

/*
 * NOTE on block move (intra-canvas scroll): GUIX's block move copies only the
 * still-valid portion of the block to the shifted position, and every non-empty
 * valid move overlaps source and destination within the one buffer.  DMA2D M2M
 * is not a memmove, so it cannot safely accelerate this -- the block move is left
 * on GUIX's direction-safe software 565rgb driver (not overridden).
 */

/*
 * DMA2D canvas copy / blend (multi-canvas compositing).  Mirrors the software
 * geometry: the source canvas, shifted by its display offset, is intersected
 * with the composite's dirty area and the overlap is copied (M2M) or blended at
 * the source canvas's global alpha (M2M_BLEND).  NOTE: a single managed visible
 * canvas (this firmware's design, gx_user.h) never composites canvases, so these
 * paths are not exercised here -- they are provided for completeness and any
 * future multi-canvas use.
 */
static VOID guix_canvas_copy(GX_CANVAS *canvas, GX_CANVAS *composite)
{
	GX_RECTANGLE dirty, overlap;
	INT w, h;

	/* DMA2D reads the source canvas and writes the composite; both must be in
	   coherent memory (non-cacheable SDRAM / read-only Flash).  A cacheable-SRAM
	   canvas defers to the software path. */
	if (!guix_dma2d_src_ok(canvas->gx_canvas_memory) ||
	    !guix_dma2d_src_ok(composite->gx_canvas_memory)) {
		sw_canvas_copy(canvas, composite);
		return;
	}
	dirty.gx_rectangle_left   = 0;
	dirty.gx_rectangle_top    = 0;
	dirty.gx_rectangle_right  = (GX_VALUE)(canvas->gx_canvas_x_resolution - 1);
	dirty.gx_rectangle_bottom = (GX_VALUE)(canvas->gx_canvas_y_resolution - 1);
	_gx_utility_rectangle_shift(&dirty, canvas->gx_canvas_display_offset_x,
	                            canvas->gx_canvas_display_offset_y);
	if (!_gx_utility_rectangle_overlap_detect(&dirty,
	                                          &composite->gx_canvas_dirty_area,
	                                          &overlap))
		return;
	w = overlap.gx_rectangle_right - overlap.gx_rectangle_left + 1;
	h = overlap.gx_rectangle_bottom - overlap.gx_rectangle_top + 1;
	guix_dma2d_blit((uint16_t *)composite->gx_canvas_memory +
	                    (uint32_t)overlap.gx_rectangle_top *
	                        (uint32_t)composite->gx_canvas_x_resolution +
	                    (uint32_t)overlap.gx_rectangle_left,
	                (const uint16_t *)canvas->gx_canvas_memory +
	                    (uint32_t)(overlap.gx_rectangle_top - dirty.gx_rectangle_top) *
	                        (uint32_t)canvas->gx_canvas_x_resolution +
	                    (uint32_t)(overlap.gx_rectangle_left - dirty.gx_rectangle_left),
	                (uint32_t)w, (uint32_t)h,
	                (uint32_t)(composite->gx_canvas_x_resolution - w),
	                (uint32_t)(canvas->gx_canvas_x_resolution - w));
}

static VOID guix_canvas_blend(GX_CANVAS *canvas, GX_CANVAS *composite)
{
	GX_RECTANGLE dirty, overlap;
	INT w, h;

	if (!guix_dma2d_src_ok(canvas->gx_canvas_memory) ||
	    !guix_dma2d_src_ok(composite->gx_canvas_memory)) {
		sw_canvas_blend(canvas, composite);
		return;
	}
	dirty.gx_rectangle_left   = 0;
	dirty.gx_rectangle_top    = 0;
	dirty.gx_rectangle_right  = (GX_VALUE)(canvas->gx_canvas_x_resolution - 1);
	dirty.gx_rectangle_bottom = (GX_VALUE)(canvas->gx_canvas_y_resolution - 1);
	_gx_utility_rectangle_shift(&dirty, canvas->gx_canvas_display_offset_x,
	                            canvas->gx_canvas_display_offset_y);
	if (!_gx_utility_rectangle_overlap_detect(&dirty,
	                                          &composite->gx_canvas_dirty_area,
	                                          &overlap))
		return;
	w = overlap.gx_rectangle_right - overlap.gx_rectangle_left + 1;
	h = overlap.gx_rectangle_bottom - overlap.gx_rectangle_top + 1;
	guix_dma2d_blend((uint16_t *)composite->gx_canvas_memory +
	                     (uint32_t)overlap.gx_rectangle_top *
	                         (uint32_t)composite->gx_canvas_x_resolution +
	                     (uint32_t)overlap.gx_rectangle_left,
	                 (const uint16_t *)canvas->gx_canvas_memory +
	                     (uint32_t)(overlap.gx_rectangle_top - dirty.gx_rectangle_top) *
	                         (uint32_t)canvas->gx_canvas_x_resolution +
	                     (uint32_t)(overlap.gx_rectangle_left - dirty.gx_rectangle_left),
	                 (uint32_t)w, (uint32_t)h,
	                 (uint32_t)(composite->gx_canvas_x_resolution - w),
	                 (uint32_t)(canvas->gx_canvas_x_resolution - w),
	                 DMA2D_INPUT_RGB565, DMA2D_REPLACE_ALPHA,
	                 canvas->gx_canvas_alpha);
}

UINT guix_display_driver_setup(GX_DISPLAY *display)
{
	/* Lay down GUIX's software 565rgb driver, binding our tear-free toggle. */
	_gx_display_driver_565rgb_setup(display, GX_NULL, guix_buffer_toggle);

	/* Save the software routines used as fallbacks for cases DMA2D cannot take. */
	sw_horizontal_line_draw = display->gx_display_driver_horizontal_line_draw;
	sw_pixelmap_draw        = display->gx_display_driver_pixelmap_draw;
	sw_pixelmap_blend       = display->gx_display_driver_pixelmap_blend;
	sw_canvas_copy          = display->gx_display_driver_canvas_copy;
	sw_canvas_blend         = display->gx_display_driver_canvas_blend;

	/* Install the DMA2D-accelerated overrides (#55 full DMA2D: solid fill,
	   pixelmap draw/blend, canvas copy/blend; the per-frame double-buffer
	   copy-forward is in guix_buffer_toggle).  block_move stays on the software
	   driver (intra-canvas moves overlap; DMA2D M2M is not a memmove). */
	display->gx_display_driver_horizontal_line_draw = guix_horizontal_line_draw;
	display->gx_display_driver_pixelmap_draw        = guix_pixelmap_draw;
	display->gx_display_driver_pixelmap_blend       = guix_pixelmap_blend;
	display->gx_display_driver_canvas_copy          = guix_canvas_copy;
	display->gx_display_driver_canvas_blend         = guix_canvas_blend;

	return GX_SUCCESS;
}
