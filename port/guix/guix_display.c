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
#include "guix_display.h"
#include "ltdc_display.h"

#include "gx_display.h"          /* _gx_display_driver_565rgb_setup proto */

#include "stm32f7xx_hal.h"

#define LOG_TAG "guix"
#include "log.h"

#include <stdint.h>

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
	    HAL_DMA2D_ConfigLayer(&hdma2d_gui, 1) == HAL_OK &&
	    HAL_DMA2D_Start(&hdma2d_gui, rgb565_to_argb8888(color),
	                    (uint32_t)(uintptr_t)dst, w, h) == HAL_OK)
		HAL_DMA2D_PollForTransfer(&hdma2d_gui, 30);
	ltdc_unlock_frame();
}

/* DMA2D memory-to-memory copy of a w x h RGB565 block with separate u16 row
   offsets for dst/src.  Source and destination must NOT overlap.  Serialized on
   ltdc_lock. */
static void guix_dma2d_blit(uint16_t *dst, const uint16_t *src, uint32_t w,
                            uint32_t h, uint32_t dst_off, uint32_t src_off)
{
	if (dst == NULL || src == NULL || w == 0u || h == 0u)
		return;

	ltdc_lock_frame();
	hdma2d_gui.Instance          = DMA2D;
	hdma2d_gui.Init.Mode         = DMA2D_M2M;
	hdma2d_gui.Init.ColorMode    = DMA2D_OUTPUT_RGB565;
	hdma2d_gui.Init.OutputOffset = dst_off;
	hdma2d_gui.LayerCfg[1].AlphaMode      = DMA2D_NO_MODIF_ALPHA;
	hdma2d_gui.LayerCfg[1].InputAlpha     = 0xFFu;
	hdma2d_gui.LayerCfg[1].InputColorMode = DMA2D_INPUT_RGB565;
	hdma2d_gui.LayerCfg[1].InputOffset    = src_off;
	if (HAL_DMA2D_Init(&hdma2d_gui) == HAL_OK &&
	    HAL_DMA2D_ConfigLayer(&hdma2d_gui, 1) == HAL_OK &&
	    HAL_DMA2D_Start(&hdma2d_gui, (uint32_t)(uintptr_t)src,
	                    (uint32_t)(uintptr_t)dst, w, h) == HAL_OK)
		HAL_DMA2D_PollForTransfer(&hdma2d_gui, 30);
	ltdc_unlock_frame();
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
	    HAL_DMA2D_ConfigLayer(&hdma2d_gui, 1) == HAL_OK &&
	    HAL_DMA2D_BlendingStart(&hdma2d_gui, (uint32_t)(uintptr_t)src,
	                            (uint32_t)(uintptr_t)dst,
	                            (uint32_t)(uintptr_t)dst, w, h) == HAL_OK)
		HAL_DMA2D_PollForTransfer(&hdma2d_gui, 30);
	ltdc_unlock_frame();
}

void guix_display_fill_back(uint16_t rgb565)
{
	uint16_t *back = ltdc_back_buffer();

	if (back != NULL)
		guix_dma2d_fill(back, rgb565, LTDC_LCD_WIDTH, LTDC_LCD_HEIGHT, 0u);
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

	if (ltdc_gui_flip() != LTDC_OK || !ltdc_is_up()) {
		ltdc_unlock_frame();
		return;                       /* display faulted: leave it be */
	}

	/* Repoint the canvas at the freshly-freed back buffer. */
	back  = ltdc_back_buffer();
	front = ltdc_framebuffer();       /* what we just presented */
	canvas->gx_canvas_memory = (GX_COLOR *)back;

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
	guix_dma2d_blit((uint16_t *)context->gx_draw_context_memory +
	                    (uint32_t)clip->gx_rectangle_top * (uint32_t)pitch +
	                    (uint32_t)clip->gx_rectangle_left,
	                (const uint16_t *)pmp->gx_pixelmap_data +
	                    (uint32_t)(clip->gx_rectangle_top - ypos) *
	                        (uint32_t)pmp->gx_pixelmap_width +
	                    (uint32_t)(clip->gx_rectangle_left - xpos),
	                (uint32_t)w, (uint32_t)h,
	                (uint32_t)(pitch - w),
	                (uint32_t)(pmp->gx_pixelmap_width - w));
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
