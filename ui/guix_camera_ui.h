/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    guix_camera_ui.h
 * @brief   Camera GUIX app (issues #61/#68): the camera UI that merges the former
 *          guix_app (widget tree) and guix_camera (frame sink + preview
 *          controller) into one application-layer module.
 *
 * This is the presentation layer (ui/), above port/guix.  The board boots with
 * the UI ON (#60) showing the live camera preview (RGB565, default QVGA #84,
 * drawn native 1:1 centred on the 480x272 panel).  The UI has two full-screen
 * pages (#68): the clean live preview, and a settings page reached by tapping the
 * image that holds the OV5640 image-quality controls, a preview-resolution
 * selector (qqvga/qvga, #69/#84) and Back (see guix_camera_ui.c).  Lifecycle:
 *
 *   camera_ui_init()   register the GUIX widget-tree builder with guix_glue
 *                      (boot-safe: no GUIX/camera I/O).  Call once from
 *                      tx_application_define().
 *   camera_ui_start()  bring GUIX up (or resume) and request the live preview.
 *                      Shared by the boot path (#60) and `gui start`.  Boot-safe:
 *                      it only starts GUIX and posts a one-shot autostart event;
 *                      the camera probe/configure (blocking I2C) runs LATER on the
 *                      GUIX system thread, never in tx_application_define().
 *   camera_ui_stop()   stop the preview, blank the screen and hand the display
 *                      back to `lcd` (`gui stop`).  Thread context only.
 *
 * Why the deferral: camera_preview_start() probes the OV5640 over I2C (blocking),
 * which must not run before the scheduler.  camera_ui_start() therefore posts
 * GX_EVENT_CAMERA_AUTOSTART; the GUIX thread runs the actual start once scheduling
 * is live.  A small volatile flag protocol serialises that GUIX-thread start
 * against a shell-thread `gui stop`.
 */
#ifndef GUIX_CAMERA_UI_H
#define GUIX_CAMERA_UI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Register the GUIX widget-tree builder with guix_glue.  No GUIX/camera I/O, so
 *  it is safe to call from tx_application_define() before the scheduler.  Call
 *  once at boot, before any camera_ui_start(). */
void camera_ui_init(void);

/** Bring the GUIX camera UI up (or resume it) and request the live preview.
 *  Shared by the boot path (#60) and `gui start`.  Boot-safe.  Returns 0 on
 *  success, or a negative GUIX bring-up error (GUIX_ERR_STATE: display down). */
int camera_ui_start(void);

/** Stop the live preview, blank the screen and hand the display back to `lcd`
 *  (`gui stop`).  Thread context only.  Idempotent.  Returns 0. */
int camera_ui_stop(void);

/** Enable/disable the on-preview face-detect bbox overlay (issue #83, Epic #80 P4).
 *  Enabling feeds the live preview frames into the NN inference worker and draws the
 *  returned face boxes onto the image; it needs a running preview and claims the
 *  single NN session (mutually exclusive with `ai bench` / `ai stream`).  Serialized
 *  against the async camera teardown.  Returns 0 on success, -1 if no preview is
 *  running, or a negative feed-start error.  Idempotent. */
int camera_ui_overlay_set(bool on);

/** True while the face-detect overlay is enabled. */
bool camera_ui_overlay_get(void);

#ifdef __cplusplus
}
#endif

#endif /* GUIX_CAMERA_UI_H */
