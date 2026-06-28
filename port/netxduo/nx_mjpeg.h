/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nx_mjpeg.h
 * @brief   MJPEG-over-HTTP camera streaming server (issue #49 P5).
 *
 * Streams the OV5640 hardware-JPEG frames to a browser as
 * `multipart/x-mixed-replace` over a NetX TCP server socket (port 80) -- the
 * "eth_sink" consumer of the camera frame pipeline (#46/#47) and the headline
 * network sink for the camera (the 30 fps JPEG path's real consumer).
 *
 * `net mjpeg start [res]` takes the camera in JPEG mode (refused if a GUIX
 * preview or a plain `camera stream` already owns the DCMI -- #73 ownership) and
 * listens on :80; point a browser at http://<board-ip>/ for the live stream.
 * `net mjpeg stop` releases the camera.  One client at a time (N=1).
 */
#ifndef NX_MJPEG_H
#define NX_MJPEG_H

#include <stdbool.h>
#include <stdint.h>

#include "camera.h"          /* enum camera_res */

#ifdef __cplusplus
extern "C" {
#endif

/** Snapshot for `net mjpeg stats`. */
struct nx_mjpeg_stats {
	bool     running;        /**< nx_mjpeg_start() active                          */
	bool     client;         /**< a browser is currently connected                 */
	uint8_t  res;            /**< enum camera_res of the stream                     */
	uint32_t conns;          /**< clients accepted since start                      */
	uint32_t sent_frames;    /**< JPEG frames delivered                            */
	uint32_t sent_bytes;     /**< JPEG payload bytes delivered (wraps at 4 GB)      */
	uint32_t drop_busy;      /**< dropped: HTTP still sending the previous frame    */
	uint32_t drop_oversized; /**< dropped: frame larger than the copy buffer        */
	uint32_t send_err;       /**< TCP send failures (window/disconnect)             */
	uint32_t pool_fail;      /**< packet pool allocation failures                   */
};

/**
 * Take the camera in JPEG at @p res, attach the MJPEG sink, and listen on :80.
 * Returns 0, or a negative: -1 (net down), -2 (already running), -3 (listen /
 * thread failed), or a CAM_ERR_* from camera_mjpeg_start (e.g. CAM_ERR_BUSY when
 * a GUIX preview / plain stream owns the camera, CAM_ERR_PARAM for res > VGA).
 */
int  nx_mjpeg_start(enum camera_res res);

/** Stop the server and release the camera.  0 ok, -1 not running, -2 teardown
 *  timed out. */
int  nx_mjpeg_stop(void);

/** Fill @p out (may be NULL) and return true while the server is running. */
bool nx_mjpeg_stats_get(struct nx_mjpeg_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_MJPEG_H */
