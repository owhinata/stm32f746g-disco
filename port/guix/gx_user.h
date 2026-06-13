/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    gx_user.h
 * @brief   GUIX build configuration for STM32F746G-DISCO (issue #55, Epic #48).
 *
 * Pulled in by gx_port.h when GX_INCLUDE_USER_DEFINE_FILE is defined (set for
 * the `threadx` target in CMakeLists.txt), exactly like port/filex/fx_user.h
 * and port/levelx/lx_user.h.  Overrides only what this board needs; everything
 * else keeps the GUIX defaults.
 */
#ifndef GX_USER_H
#define GX_USER_H

/*
 * GUIX system thread priority.  The GUIX default (GX_SYSTEM_THREAD_PRIORITY 16,
 * gx_api.h) collides with this project's shell instance (cli priority 16), so
 * move GUIX to 14: below the camera producer / LED heartbeat (10) and the IWDG
 * petter (5), above the shell (16) and its background workers (17).  GUIX is
 * event-driven -- its thread sleeps on the event queue whenever there is no
 * input or animation -- so sitting above the shell does not starve the console.
 */
#define GX_SYSTEM_THREAD_PRIORITY   14

/*
 * GUIX system thread stack.  Widget-tree traversal plus glyph rendering nests a
 * fair bit; 8 KiB gives margin over the 4 KiB default (tune down once measured).
 */
#define GX_THREAD_STACK_SIZE        8192

/*
 * Single managed + visible full-screen (480x272) canvas bound to the LTDC
 * double buffer.  The tear-free buffer toggle (port/guix/guix_display.c) relies
 * on GUIX calling the toggle ONCE per accumulated-dirty frame, which holds for a
 * single managed visible canvas with the partial frame buffer disabled.  So we
 * deliberately leave GX_ENABLE_CANVAS_PARTIAL_FRAME_BUFFER undefined (default
 * off) and never create a second visible canvas.
 */

#endif /* GX_USER_H */
