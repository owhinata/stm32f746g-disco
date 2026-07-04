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

#include "camera.h"            /* enum camera_res */
#include "models/blazeface.h"  /* struct bf_det */

struct frame_desc;             /* svc/frame_pipeline.h (external-feed push) */

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

/** Stop live inference and release the camera (bounded wait for teardown).
 *  Only stops a camera-OWNING stream (`ai stream`/`ai run`); returns -3 if a GUIX
 *  overlay external feed owns the session (tear that down via `gui overlay off`). */
int  nn_camera_stop(void);

/* ---- external-feed mode (GUIX face-detect overlay, issue #83) -------------
 * The camera-owning path above takes the DCMI itself; the external-feed path lets
 * an owner that ALREADY holds the camera (the GUIX live preview) push its frames
 * into the same inference worker/decode/dets pipeline.  The two modes are mutually
 * exclusive (single camera owner) and share the single nn session guard. */

/**
 * Start inference in external-feed mode: open the model, latch its input geometry,
 * claim the nn session and start the worker -- but do NOT take the camera.  The
 * caller (which owns the camera) then pushes frames with nn_camera_feed().  Bounded
 * (no worker-park wait), so it is safe to call under a short caller lock.  Returns 0
 * or <0 (-2 busy/tearing down, -3 model, -4 geometry, -5 objects, -6 nn session busy).
 */
int  nn_camera_feed_start(void);

/**
 * Push one RGB565 camera frame @p f into the external-feed pipeline (no-op unless a
 * feed is running).  Synchronous copy: preprocesses into a free staging buffer and
 * wakes the worker; drops if none free.  Must be called with @p f's data valid
 * (pinned by the caller, who releases the pin afterwards).
 */
void nn_camera_feed(const struct frame_desc *f);

/**
 * Non-blocking teardown of an external-feed session (idempotent; no-op unless a feed
 * is running).  Requests the worker to stop and release the nn session; safe to call
 * from any thread (incl. the camera producer during async teardown) since it never
 * waits on the worker.  The session is released asynchronously by the worker (or by
 * this caller if the worker never entered its run loop).
 */
void nn_camera_feed_abort(void);

/** True while a stream is running. */
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
