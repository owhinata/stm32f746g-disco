/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    guix_display.h
 * @brief   GUIX 565rgb display driver bound to the LTDC double buffer (issue #55).
 *
 * Wires GUIX's software RGB565 display driver onto the existing tear-free LTDC
 * double buffer (port/ltdc), with DMA2D acceleration on the two paths the basic
 * widget UI actually exercises: solid fills (horizontal_line_draw -> DMA2D R2M)
 * and the per-frame double-buffer copy-forward in the buffer toggle (DMA2D M2M).
 * The remaining primitives (canvas copy/blend, pixelmap draw/blend, block move)
 * stay on GUIX's verified software 565rgb driver -- they are either unused by a
 * single managed canvas without images (canvas/pixelmap) or inherently overlap
 * within one buffer (block move), so DMA2D would fall back to software anyway.
 *
 * All DMA2D work runs under ltdc_lock (the shared frame mutex) because DMA2D is
 * a single engine also driven by the `lcd` command; while GUIX owns the display
 * the `lcd` draw/flip path is disabled (ltdc_gui_take), so the two never race.
 */
#ifndef GUIX_DISPLAY_H
#define GUIX_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "gx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Live-preview geometry (issue #56): QVGA RGB565 drawn native 1:1, centred on
   the 480x272 panel.  Lives here (the port display layer) rather than in the ui
   camera app because the buffer-toggle B2 paths below consume it -- keeping it in
   port preserves the port <- ui dependency direction (#43/#61). */
#define CAM_VIEW_W  320   /* boot-default preview geometry (QVGA, centred) */
#define CAM_VIEW_H  240
#define CAM_VIEW_X  80    /* (480 - 320) / 2 */
#define CAM_VIEW_Y  16    /* (272 - 240) / 2 */

/* Largest preview the UI can select (#69/#84): QVGA (320x240) -- 480x272 was
   removed (horizontal stretch from the sensor's 4:3 FOV + SDRAM overrun, #84).
   The view buffer is sized to this so a resolution switch never reallocates; the
   live geometry is passed to guix_display_cam_preview_begin() per session. */
#define CAM_VIEW_W_MAX  320
#define CAM_VIEW_H_MAX  240

/** GUIX display-driver setup callback passed to gx_display_create(): lays down
 *  the software 565rgb driver, then installs the DMA2D-accelerated overrides and
 *  the tear-free buffer toggle.  Returns GX_SUCCESS. */
UINT guix_display_driver_setup(GX_DISPLAY *display);

/** Fill the whole back buffer with one RGB565 colour via DMA2D (owner path, runs
 *  under ltdc_lock).  Used by guix_stop() to blank the screen -- the public
 *  ltdc_fill() is a no-op while GUIX owns the display. */
void guix_display_fill_back(uint16_t rgb565);

/** DMA2D M2M copy of a @p w x @p h RGB565 block (u16 row offsets @p dst_off /
 *  @p src_off), serialized on ltdc_lock.  Returns 0 on success, -1 on failure.
 *  Used by the camera live-preview sink (#56) to copy a ring slot into its
 *  private view buffer; both must be DMA2D-coherent (non-cacheable SDRAM). */
int guix_display_copy_rgb565(uint16_t *dst, const uint16_t *src, uint32_t w,
                             uint32_t h, uint32_t dst_off, uint32_t src_off);

/* ---- camera live-preview copy-forward elimination (#59 Lever B2) ---------- */

/** Arm the preview-path copy-forward optimization for a session: register the
 *  camera view buffer (whose content is the latest captured frame) and the
 *  on-panel camera-rect geometry @p w x @p h at (@p x, @p y) -- variable per
 *  preview resolution (#69).  Both LTDC buffers start "stale" so the first frames
 *  are corrected before present.  Returns 0, or -1 if the geometry is invalid
 *  (NULL buffer / zero size / outside the 480x272 panel), leaving B2 disarmed.
 *  Call from the camera UI (ui/) before the stream starts; runs under ltdc_lock. */
int guix_display_cam_preview_begin(uint16_t *view_buf, uint16_t w, uint16_t h,
                                   uint16_t x, uint16_t y);

/** Disarm the optimization at preview teardown (runs under ltdc_lock). */
void guix_display_cam_preview_end(void);

/** Gate the buffer-toggle B2 paths to when the camera preview is on display.  Call
 *  with true when the preview starts and false when it stops (#61: the camera is
 *  the sole screen, so this tracks the preview lifecycle; pre-#61 it tracked the
 *  camera screen SHOW/HIDE).  Runs under ltdc_lock. */
void guix_display_cam_set_visible(bool on);

/** Store a freshly captured frame (@p src, the current preview resolution RGB565,
 *  #69) into the view buffer via DMA2D and mark BOTH LTDC buffers stale, atomically
 *  under ltdc_lock.  Returns 0 on a completed copy, -1 otherwise. */
int guix_display_cam_view_store(const uint16_t *src);

/** Draw a 2-px RGB565 rectangle outline into the camera view buffer, scaled from a
 *  normalized [0,1] box @p nx,@p ny,@p nw,@p nh (issue #83 face-detect overlay).  The
 *  box rides the view buffer, so it is composited into the live image the same way as
 *  the frame itself (#59 B2) and is overwritten by the next store.  No-op if no
 *  preview session is armed (cam_view_data NULL).  Runs under ltdc_lock and marks both
 *  LTDC buffers stale.  Call after guix_display_cam_view_store(), on the producer. */
void guix_display_cam_overlay_box(float nx, float ny, float nw, float nh,
                                  uint16_t rgb565);

#ifdef __cplusplus
}
#endif

#endif /* GUIX_DISPLAY_H */
