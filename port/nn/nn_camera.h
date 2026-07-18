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
 * eth_sink lifecycle (thread created once + parked) and the codex-reviewed
 * double-buffer ownership rule (the buffer the worker feeds to nn_run() is never
 * written by the sink).  Since Epic #99 Phase 1 (#100) nncam is a plain camera
 * *subscriber*: `ai stream start/stop` enable/disable it and it attaches to the
 * base capture (`camera stream`) only while the base runs.  A base detach (stop /
 * DCMI overrun / cascade) PAUSES it (frames stop) but keeps it enabled and holding
 * the nn session; it re-attaches when the base restarts.
 *
 * Drives `ai stream start|stop` and `ai run`.  Model-specific detection decode
 * (BlazeFace anchors + NMS) is layered above in port/nn/models (issue #81 task
 * 8); with the `null` backend this loop runs end-to-end with 0 detections.
 */
#ifndef NN_CAMERA_H
#define NN_CAMERA_H

#include <stdbool.h>
#include <stdint.h>

#include "camera.h"            /* enum camera_res */
#include "models/blazeface.h"  /* struct bf_det */

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
 * Enable live camera inference (`ai stream start`).  @p res is a display hint
 * only -- the input adapts to whatever geometry the base capture publishes
 * (#100).  Claims the single nn session (refused -6 if `ai bench`/another stream
 * holds it) and registers nncam as an RGB565 subscriber of the base: it attaches
 * immediately if the base is already running, otherwise it stays enabled + idle
 * and attaches at the next `camera stream start`.  Non-blocking.  Returns 0 or <0
 * (-2 already running, -3 model, -4 geometry, -5 objects, -6 nn session busy).
 */
int  nn_camera_start(enum camera_res res);

/** Disable live inference (`ai stream stop`): unsubscribe from the base (which
 *  keeps running for other subscribers) and release the nn session after the
 *  worker parks.  Bounded wait; returns 0, -1 (not running), or -2 (worker still
 *  mid-inference -- the session is released by the worker as it exits). */
int  nn_camera_stop(void);

/** True while inference is enabled. */
bool nn_camera_running(void);

/** Snapshot current stats (any time). */
void nn_camera_stats_get(struct nn_camera_stats *out);

/** Copy the latest detections into @p out[0..max); returns the count copied. */
int nn_camera_dets_get(struct bf_det *out, int max);

/** Runtime float32 input normalization: 1 = [-1,1], 0 = [0,1] (tuning, no reflash). */
void nn_camera_set_norm(int signed_range);
int  nn_camera_get_norm(void);

#ifdef __cplusplus
}
#endif

#endif /* NN_CAMERA_H */
