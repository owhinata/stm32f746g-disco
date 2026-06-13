/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    guix_app.h
 * @brief   Hand-coded GUIX demo UI: two screens + button + transition (issue #55).
 *
 * No GUIX Studio: the colour/font tables and the widget tree are built by hand.
 * Text uses GUIX's built-in _gx_system_font_8bpp (compiled from the submodule,
 * common/src/gx_system_font_8bpp.c), so no generated font resource is needed.
 * Screen 0 shows a title + a "Next" button; tapping it switches to screen 1
 * (a "Back" button returns).  All widgets are static -- no dynamic allocation,
 * so GUIX needs no memory allocator.
 */
#ifndef GUIX_APP_H
#define GUIX_APP_H

#include "gx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Install the theme (colour + font tables) on @p display and build the two
 *  screens under @p root (screen 0 visible, screen 1 hidden).  Call once, before
 *  gx_widget_show(root) / gx_system_start().  Returns GX_SUCCESS or the first
 *  failing GUIX status. */
UINT guix_app_create(GX_DISPLAY *display, GX_WINDOW_ROOT *root);

/** Select the active screen (0 or 1): shows it and hides the other.  Call with 0
 *  after gx_widget_show(root) to establish the initial screen; the in-UI button
 *  handlers use it to switch.  Must run on the GUIX thread (or before
 *  gx_system_start()). */
void guix_app_show_screen(int n);

#ifdef __cplusplus
}
#endif

#endif /* GUIX_APP_H */
