/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_camera.h
 * @brief   Live camera -> NN inference glue (issue #81, Epic #80).
 *
 * Bridges the camera frame pipeline (port/camera) to the nn inference API
 * (port/nn/nn.h): a synchronous copy push sink resizes + converts each RGB565
 * camera frame into the model's int8 input, and a low-priority (prio 18)
 * best-effort worker thread runs inference on it.  Follows the nx_mjpeg.c
 * eth_sink lifecycle (thread created once + parked, single-owner camera claim,
 * producer_dead auto-stop) and the codex-reviewed double-buffer ownership rule
 * (the buffer the worker feeds to nn_run() is never written by the sink).
 *
 * Drives `ai stream start|stop` and `ai run`.  Model-specific detection decode
 * (BlazeFace anchors + NMS) is layered above in port/nn/models (issue #81 task
 * 8); with the `null` backend this loop runs end-to-end with 0 detections.
 */
#ifndef NN_CAMERA_H
#define NN_CAMERA_H

#include <stdbool.h>
#include <stdint.h>

#include "camera.h"   /* enum camera_res */

#ifdef __cplusplus
extern "C" {
#endif

struct nn_camera_stats {
	bool     running;
	uint8_t  res;          /**< enum camera_res of the active stream */
	uint32_t frames;       /**< frames delivered to the sink          */
	uint32_t drops;        /**< frames dropped (no free stage buffer)  */
	uint32_t infers;       /**< inferences completed                   */
	uint32_t errors;       /**< nn_run() failures                      */
	uint32_t last_us;      /**< latency of the last inference (us)     */
	uint32_t fps_x100;     /**< average inference rate x100 since start */
	uint32_t detections;   /**< detections from the last inference (task 8) */
};

/**
 * Start live camera inference at resolution @p res (RGB565; must be a small
 * streamable mode QQVGA/QVGA/480x272, per camera_preview_start()).  Takes single
 * camera ownership -- refused if a GUIX preview / plain stream / mjpeg already
 * owns the DCMI.  Non-blocking.  Returns 0 or <0.
 */
int  nn_camera_start(enum camera_res res);

/** Stop live inference and release the camera (bounded wait for teardown). */
int  nn_camera_stop(void);

/** True while a stream is running. */
bool nn_camera_running(void);

/** Snapshot current stats (any time). */
void nn_camera_stats_get(struct nn_camera_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* NN_CAMERA_H */
