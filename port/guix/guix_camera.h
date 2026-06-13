/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    guix_camera.h
 * @brief   Camera -> GUIX live preview: frame_pipeline push sink + controller
 *          (issue #56, last sub-Issue of Epic #48).
 *
 * The DCMI streaming producer (#46, port/camera) publishes QVGA RGB565 frames
 * into its SDRAM ring; this module attaches a push sink to that pipeline whose
 * consume() copies each frame (DMA2D M2M, under ltdc_lock) into a private
 * non-cacheable SDRAM "view buffer" and wakes the GUIX thread.  A GX_ICON on a
 * dedicated camera screen draws that view buffer at native 320x240 (no scaling)
 * centred on the 480x272 panel -- guix_pixelmap_draw blits it (DMA2D M2M) into
 * the tear-free back buffer.  The view buffer decouples GUIX redraws (touch,
 * screen change) from the ring slot lifetime, so a slot is never read by the UI
 * after it is recycled.
 *
 * Control is shell-driven (`gui camera on|off`); the on-screen Back button on
 * the camera screen also stops the preview.  Heavy work (sensor probe/configure
 * in guix_camera_on) runs on the shell thread; guix_camera_off only does a
 * bounded drain, so the Back button may call it from the GUIX thread.
 */
#ifndef GUIX_CAMERA_H
#define GUIX_CAMERA_H

#include "gx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* User events posted to the GUIX root (target = root) for the camera preview.
   Custom events with a NULL target are not delivered to the root handler in
   GUIX v6.5.1, so guix_post_root_event() always targets the root window. */
#define GX_EVENT_CAMERA_FRAME  (GX_FIRST_USER_EVENT + 0)  /* new frame in view buf */
#define GX_EVENT_CAMERA_SHOW   (GX_FIRST_USER_EVENT + 1)  /* show the camera screen */
#define GX_EVENT_CAMERA_HIDE   (GX_FIRST_USER_EVENT + 2)  /* return to main screen  */

/* Live-preview geometry: QVGA, drawn native 1:1, centred on 480x272. */
#define CAM_VIEW_W  320
#define CAM_VIEW_H  240
#define CAM_VIEW_X  80    /* (480 - 320) / 2 */
#define CAM_VIEW_Y  16    /* (272 - 240) / 2 */

/** The GX_PIXELMAP wrapping the private RGB565 view buffer (its data is updated
 *  in place each frame by the preview sink; the icon redraws the same pixmap).
 *  guix_app registers it in the display pixelmap table.  First call also blanks
 *  the view buffer to black so nothing garbage shows before the first frame. */
GX_PIXELMAP *guix_camera_pixmap(void);

/** Clear the per-frame redraw-coalesce flag; called by the root event handler
 *  (GUIX thread) right before it marks the camera icon dirty. */
void guix_camera_mark_drawn(void);

/** Start the live preview: ensure GUIX is up, start streaming + attach the sink
 *  (takes camera preview ownership), and switch to the camera screen.  Shell
 *  thread only (does the sensor probe/configure).  Returns GUIX_OK or a negative
 *  GUIX_ERR* / camera error mapped to GUIX_ERR. */
int guix_camera_on(void);

/** Stop the live preview: return to the main screen, stop the stream + detach
 *  the sink, and drain any in-flight consume().  Safe from the shell thread or
 *  the GUIX thread (Back button).  Idempotent. */
int guix_camera_off(void);

#ifdef __cplusplus
}
#endif

#endif /* GUIX_CAMERA_H */
