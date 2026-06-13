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

#include <stdint.h>

#include "gx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/** GUIX display-driver setup callback passed to gx_display_create(): lays down
 *  the software 565rgb driver, then installs the DMA2D-accelerated overrides and
 *  the tear-free buffer toggle.  Returns GX_SUCCESS. */
UINT guix_display_driver_setup(GX_DISPLAY *display);

/** Fill the whole back buffer with one RGB565 colour via DMA2D (owner path, runs
 *  under ltdc_lock).  Used by guix_stop() to blank the screen -- the public
 *  ltdc_fill() is a no-op while GUIX owns the display. */
void guix_display_fill_back(uint16_t rgb565);

#ifdef __cplusplus
}
#endif

#endif /* GUIX_DISPLAY_H */
