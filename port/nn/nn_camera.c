/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_camera.c
 * @brief   Live camera -> NN inference glue (issue #81, Epic #80).  See nn_camera.h.
 *
 * Design (codex-reviewed, issue #81):
 *   - A SYNCHRONOUS copy push sink (like nx_mjpeg.c eth_sink): consume() runs in
 *     the camera producer thread, resizes + converts the RGB565 frame into an
 *     int8 staging buffer, then camera_frame_put()s the pin immediately, so the
 *     pipeline in-flight count is always 0 and the camera's async teardown stays
 *     correct.  The nearest-neighbour resize reads only WxH sampled source pixels
 *     (cheaper than copying the whole frame), keeping producer-thread load low.
 *   - NN-input ownership (the BLOCKING codex fix): the sink NEVER writes a buffer
 *     the worker is using.  Two staging buffers with a FREE/FILLING/READY/RUNNING
 *     state machine under a short TX_MUTEX; the worker copies a READY stage into
 *     the model input (the only writer of nn_input()->data) and runs inference.
 *     No free stage -> drop (FRAME_POLICY_DROP).
 *   - Worker priority 18 (below BG-17/CLI-16/GUIX-14/net-12/camera-10): fully
 *     best-effort, so inference never starves the DCMI ring, UI, net, or the CLI
 *     that must deliver `ai stream stop`.  Inference is monolithic (no mid-run
 *     yield), so a lower-than-CLI priority is what guarantees stop reaches us.
 */
#include <string.h>          /* memcpy */

#include "tx_api.h"

#include "nn.h"
#include "nn_camera.h"
#include "models/blazeface.h" /* BlazeFace decode (model-specific post-process) */
#include "camera.h"          /* camera_preview_start / camera_preview_stop / camera_frame_put */
#include "frame_pipeline.h"  /* struct frame_sink / frame_desc / FRAME_POLICY_* */

#include "stm32f7xx_hal.h"   /* HAL_GetTick, HAL_RCC_GetHCLKFreq */

#define LOG_TAG "nncam"
#include "log.h"

/* Max model input: BlazeFace-128 is 128x128x3.  Two staging buffers in the NN
 * arena (.sdram.ai, bank3).  Sized for the worst case -- a FLOAT32 128x128x3
 * input (BlazeFace) = 196608 B/buffer; int8 models use a quarter.  Larger models
 * would bump these bounds. */
#define NNCAM_IN_MAX_W    128u
#define NNCAM_IN_MAX_H    128u
#define NNCAM_IN_MAX_C    3u
#define NNCAM_STAGE_BYTES (NNCAM_IN_MAX_W * NNCAM_IN_MAX_H * NNCAM_IN_MAX_C * 4u)
#define NNCAM_STAGE_N     2

/* Float32 input normalization.  BlazeFace's model card says [-1,1], but its ST
 * config says rescale 1/255 -> [0,1] -- and [0,1] is what actually detects faces
 * on hardware (maxscore 288 vs 11), so ST retrained with [0,1].  Default [0,1];
 * `ai norm 1` flips to [-1,1] at runtime for other models. */
#ifndef NNCAM_NORM_SIGNED
#define NNCAM_NORM_SIGNED 0
#endif

#define NNCAM_WORKER_PRIORITY 18          /* full best-effort (below BG-17)        */
#define NNCAM_WORKER_STACK    8192u
#define NNCAM_POLL_TICKS      100u        /* sem wait -> stop latency               */

/* Staging buffers live in the NN arena (bank3, .sdram.ai).  Raw bytes: hold either
 * int8 or float32 preprocessed input depending on the model's input dtype. */
static uint8_t nncam_stage[NNCAM_STAGE_N][NNCAM_STAGE_BYTES]
	__attribute__((aligned(32), section(".sdram.ai")));

enum { ST_FREE = 0, ST_FILLING, ST_READY, ST_RUNNING };
static uint8_t nncam_state[NNCAM_STAGE_N];

static TX_THREAD    nncam_thread;
static UCHAR        nncam_stack[NNCAM_WORKER_STACK];
static TX_MUTEX     nncam_lock;           /* guards nncam_state[]                   */
static TX_SEMAPHORE nncam_sem;            /* consume posts a READY stage             */

static struct frame_sink nncam_sink;
static struct nn_model  *nncam_model;

static volatile int nncam_run;            /* requested running (start=1, stop=0)    */
static volatile int nncam_active;         /* worker is in the run loop (set by worker) */
static volatile int nncam_producer_dead;  /* camera producer torn down (close cb)   */
static volatile int nncam_holds_session;  /* the stream currently holds the nn session */
static int          nncam_created;        /* worker/objects created once            */
static uint8_t      nncam_res;            /* enum camera_res of the active stream    */

/* Release the nn session iff this stream still holds it (exactly once per stream;
 * called from both nn_camera_stop() and the worker's producer_dead auto-stop). */
static void nncam_release_session(void)
{
	int held;

	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	held = nncam_holds_session;
	nncam_holds_session = 0;
	tx_mutex_put(&nncam_lock);
	if (held)
		nn_session_release();
}

/* Model input geometry, latched at start from the input tensor. */
static uint16_t nncam_in_w, nncam_in_h, nncam_in_c;
static uint32_t nncam_in_bytes;
static uint8_t  nncam_in_dtype;   /* enum nn_dtype of the model input */

/* Latest detections (BlazeFace), guarded by nncam_lock; read via nn_camera_dets_get(). */
static struct bf_det nncam_dets[BF_MAX_DET];
static int           nncam_ndet;

/* Float32 input normalization, runtime-tunable (ai norm) to settle the [-1,1] vs
 * [0,1] ambiguity on hardware without reflashing. */
static int nncam_norm_signed = NNCAM_NORM_SIGNED;

static struct {
	uint32_t frames;
	uint32_t drops;
	uint32_t infers;
	uint32_t errors;
	uint32_t last_us;
	uint32_t detections;
	uint32_t start_tick;
} nnstat;

/* ---- RGB565 -> model input: nearest-neighbour resize + convert ------------ */

/* Convert one camera frame @p f (RGB565) into @p dst (HxWxC), formatted for the
 * model input dtype.  Reads only the sampled source pixels.
 *   - FLOAT32: RGB in [-1,1] (NNCAM_NORM_SIGNED) or [0,1] -- BlazeFace.
 *   - INT8   : uint8 - 128 (symmetric ~[-1,1], scale 1/128) -- MNIST/null stub. */
static void nncam_preprocess(const struct frame_desc *f, void *dst)
{
	const uint8_t *base = (const uint8_t *)f->data;
	uint32_t stride = f->stride ? f->stride : (uint32_t)f->width * 2u;
	uint16_t ow = nncam_in_w, oh = nncam_in_h, oc = nncam_in_c;
	uint16_t sw = f->width, sh = f->height;
	int is_f32 = (nncam_in_dtype == NN_DTYPE_FLOAT32);

	for (uint16_t oy = 0; oy < oh; oy++) {
		uint16_t sy = (uint16_t)((uint32_t)oy * sh / oh);
		const uint8_t *row = base + (uint32_t)sy * stride;
		for (uint16_t ox = 0; ox < ow; ox++) {
			uint16_t sx = (uint16_t)((uint32_t)ox * sw / ow);
			uint16_t px = (uint16_t)(row[sx * 2] | ((uint16_t)row[sx * 2 + 1] << 8));
			uint8_t rgb[3];
			uint32_t o = ((uint32_t)oy * ow + ox) * oc;

			rgb[0] = (uint8_t)(((px >> 11) & 0x1Fu) * 255u / 31u);
			rgb[1] = (uint8_t)(((px >> 5) & 0x3Fu) * 255u / 63u);
			rgb[2] = (uint8_t)((px & 0x1Fu) * 255u / 31u);

			if (is_f32) {
				float *o32 = (float *)dst + o;
				for (uint16_t c = 0; c < oc && c < 3; c++)
					o32[c] = nncam_norm_signed
					       ? (float)rgb[c] / 127.5f - 1.0f   /* [-1,1] */
					       : (float)rgb[c] / 255.0f;         /* [0,1]  */
			} else {
				int8_t *o8 = (int8_t *)dst + o;
				for (uint16_t c = 0; c < oc && c < 3; c++)
					o8[c] = (int8_t)((int)rgb[c] - 128);
			}
		}
	}
}

/* ---- frame-pipeline sink (synchronous copy) ------------------------------- */

static int nncam_open(void *ctx, enum frame_format fmt, uint16_t w, uint16_t h)
{
	(void)ctx; (void)w; (void)h;

	if (fmt != FRAME_FMT_RGB565)
		return -1;                          /* preview delivers RGB565 only        */

	/* Fresh session on attach (single reset point, like eth_open). */
	nncam_producer_dead = 0;
	nncam_ndet = 0;                     /* drop stale detections from a prior session */
	for (int i = 0; i < NNCAM_STAGE_N; i++)
		nncam_state[i] = ST_FREE;
	memset(&nnstat, 0, sizeof nnstat);
	nnstat.start_tick = HAL_GetTick();
	while (tx_semaphore_get(&nncam_sem, TX_NO_WAIT) == TX_SUCCESS)
		;
	return 0;
}

static int nncam_consume(void *ctx, const struct frame_desc *f)
{
	int i = -1;

	(void)ctx;

	if (!nncam_run) {
		camera_frame_put(&nncam_sink, f);
		return 0;
	}

	/* Claim a FREE staging buffer (short critical section). */
	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	for (int k = 0; k < NNCAM_STAGE_N; k++) {
		if (nncam_state[k] == ST_FREE) { nncam_state[k] = ST_FILLING; i = k; break; }
	}
	tx_mutex_put(&nncam_lock);

	if (i < 0) {                            /* no free buffer -> drop this frame    */
		nnstat.drops++;
		camera_frame_put(&nncam_sink, f);
		return 0;
	}

	/* Resize + convert while the slot is still pinned (reads f->data). */
	nncam_preprocess(f, nncam_stage[i]);
	camera_frame_put(&nncam_sink, f);       /* release the pin immediately (in-flight 0) */

	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	nncam_state[i] = ST_READY;
	tx_mutex_put(&nncam_lock);

	nnstat.frames++;
	(void)tx_semaphore_put(&nncam_sem);     /* wake the worker                      */
	return 0;
}

static void nncam_close(void *ctx)
{
	(void)ctx;
	nncam_producer_dead = 1;                /* async teardown (e.g. DCMI overrun)   */
}

/* ---- inference worker ----------------------------------------------------- */

/* Run one inference from a READY stage, if any.  Returns 1 if an inference ran. */
static int nncam_step(void)
{
	uint32_t hclk = HAL_RCC_GetHCLKFreq();
	uint32_t mhz = hclk / 1000000u ? hclk / 1000000u : 1u;
	int j = -1;
	int rc;

	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	for (int k = 0; k < NNCAM_STAGE_N; k++) {
		if (nncam_state[k] == ST_READY) { nncam_state[k] = ST_RUNNING; j = k; break; }
	}
	tx_mutex_put(&nncam_lock);

	if (j < 0)
		return 0;

	/* Copy the READY stage into the model input (worker is the sole writer of
	 * nn_input()->data), then free the stage so the sink can reuse it during the
	 * inference -- the double-buffer benefit. */
	{
		struct nn_tensor *in = nn_input(nncam_model, 0);
		if (in && in->data)
			memcpy(in->data, nncam_stage[j], nncam_in_bytes);
	}
	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	nncam_state[j] = ST_FREE;
	tx_mutex_put(&nncam_lock);

	rc = nn_run(nncam_model);
	if (rc != 0) {
		nnstat.errors++;
		return 1;
	}
	nnstat.infers++;
	nnstat.last_us = nn_last_cycles(nncam_model) / mhz;

	/* Model-specific decode (BlazeFace).  A safe no-op (returns <0) for other
	 * models, so this stays model-agnostic at the sink level.  Publish the boxes
	 * under the lock for nn_camera_dets_get(). */
	{
		struct bf_det tmp[BF_MAX_DET];
		int nd = blazeface_decode(nncam_model, tmp, BF_MAX_DET);

		tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
		if (nd < 0) {
			nncam_ndet = 0;
		} else {
			nncam_ndet = nd;
			for (int d = 0; d < nd; d++)
				nncam_dets[d] = tmp[d];
		}
		tx_mutex_put(&nncam_lock);
		nnstat.detections = (nd > 0) ? (uint32_t)nd : 0u;
	}
	return 1;
}

static void nncam_entry(ULONG arg)
{
	(void)arg;
	for (;;) {
		while (!nncam_run)
			tx_thread_sleep(20);            /* parked until started                */

		/* Acknowledge the start ONLY once we are actually in the run loop.  If a
		 * stop raced ahead (nncam_run already 0), we fall straight back to park
		 * without ever setting nncam_active -- so nn_camera_stop() never waits on
		 * an active flag the worker will not clear (the BLOCKING stuck race). */
		nncam_active = 1;
		LOG_INF("inference running (prio %u)", (unsigned)NNCAM_WORKER_PRIORITY);
		while (nncam_run && !nncam_producer_dead) {
			if (tx_semaphore_get(&nncam_sem, NNCAM_POLL_TICKS) != TX_SUCCESS)
				continue;                   /* timeout -> re-check run/dead         */
			(void)nncam_step();
		}
		if (nncam_producer_dead) {
			LOG_WRN("camera producer stopped -- auto-stopping inference");
			nncam_run = 0;
			nncam_release_session();        /* async teardown owns the release      */
		}
		nncam_active = 0;                   /* parked; nn_camera_stop() waits on this */
		LOG_INF("inference stopped (%lu infers)", (unsigned long)nnstat.infers);
	}
}

/* ---- public API ----------------------------------------------------------- */

int nn_camera_start(enum camera_res res)
{
	struct nn_tensor *in;
	int rc;

	if (nncam_run || nncam_active)
		return -2;                          /* running or still tearing down        */

	/* Open the model + latch its input geometry (bounds-checked). */
	if (nn_model_open(&nncam_model) != 0)
		return -3;
	in = nn_input(nncam_model, 0);
	if (!in || in->ndim < 3)
		return -4;
	nncam_in_h = in->dims[1];
	nncam_in_w = in->dims[2];
	nncam_in_c = (in->ndim >= 4) ? in->dims[3] : 1;
	nncam_in_bytes = in->bytes;
	nncam_in_dtype = in->dtype;
	if (nncam_in_w > NNCAM_IN_MAX_W || nncam_in_h > NNCAM_IN_MAX_H ||
	    nncam_in_c > NNCAM_IN_MAX_C || nncam_in_bytes > NNCAM_STAGE_BYTES)
		return -4;                          /* model input exceeds the staging bound */

	/* Create ThreadX objects exactly once (idempotent across start/stop). */
	if (!nncam_created) {
		if (tx_mutex_create(&nncam_lock, "nncam", TX_INHERIT) != TX_SUCCESS)
			return -5;
		if (tx_semaphore_create(&nncam_sem, "nncam", 0) != TX_SUCCESS) {
			tx_mutex_delete(&nncam_lock);
			return -5;
		}
		nncam_sink.name    = "nncam";
		nncam_sink.policy  = FRAME_POLICY_DROP;
		nncam_sink.open    = nncam_open;
		nncam_sink.consume = nncam_consume;
		nncam_sink.close   = nncam_close;
		if (tx_thread_create(&nncam_thread, "nn-worker", nncam_entry, 0,
		                     nncam_stack, sizeof nncam_stack,
		                     NNCAM_WORKER_PRIORITY, NNCAM_WORKER_PRIORITY,
		                     TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS) {
			tx_semaphore_delete(&nncam_sem);
			tx_mutex_delete(&nncam_lock);
			return -5;
		}
		nncam_created = 1;
	}

	/* Claim the single inference session: refused (-6) if `ai bench` or another
	 * stream/run is already using the non-reentrant singleton model. */
	if (nn_session_try_acquire() != 0)
		return -6;

	/* Take the camera in RGB565 + attach the sink (calls nncam_open, which resets
	 * the session).  Only after this succeeds do we claim the lifecycle; the worker
	 * sets nncam_active itself once it enters the run loop. */
	rc = camera_preview_start(&nncam_sink, res);
	if (rc != 0) {
		nn_session_release();
		return rc;
	}

	nncam_res = (uint8_t)res;
	nncam_holds_session = 1;
	nncam_run = 1;
	return 0;
}

int nn_camera_stop(void)
{
	if (!nncam_run && !nncam_active)
		return -1;

	nncam_run = 0;
	(void)tx_semaphore_put(&nncam_sem);     /* wake the worker if waiting           */
	camera_preview_stop(&nncam_sink);       /* detach + stop camera (in-flight = 0) */

	for (int i = 0; i < 30 && nncam_active; i++)
		tx_thread_sleep(100);

	/* If the worker is still mid-nn_run() after the wait, do NOT release the
	 * session here -- that would let another activity acquire the model while the
	 * worker still reads it.  camera_preview_stop()'s detach set producer_dead, so
	 * the worker WILL release via its producer_dead path once nn_run() returns. */
	if (nncam_active)
		return -2;

	/* Worker parked (or never entered the loop): release the session iff the worker
	 * did not already (idempotent via the holds flag). */
	nncam_release_session();
	return 0;
}

bool nn_camera_running(void)
{
	return nncam_run != 0;
}

void nn_camera_stats_get(struct nn_camera_stats *out)
{
	uint32_t now, elapsed;

	if (!out)
		return;
	out->running    = (nncam_run != 0);
	out->res        = nncam_res;
	out->frames     = nnstat.frames;
	out->drops      = nnstat.drops;
	out->infers     = nnstat.infers;
	out->errors     = nnstat.errors;
	out->last_us    = nnstat.last_us;
	out->detections = nnstat.detections;

	now = HAL_GetTick();
	elapsed = now - nnstat.start_tick;
	out->fps_x100 = elapsed ? (uint32_t)((uint64_t)nnstat.infers * 100000u / elapsed) : 0u;
}

int nn_camera_dets_get(struct bf_det *out, int max)
{
	int n;

	if (!nncam_created || !out || max <= 0)
		return 0;                           /* nncam_lock not created before 1st start */
	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	n = nncam_ndet < max ? nncam_ndet : max;
	for (int i = 0; i < n; i++)
		out[i] = nncam_dets[i];
	tx_mutex_put(&nncam_lock);
	return n;
}

void nn_camera_set_norm(int signed_range) { nncam_norm_signed = signed_range ? 1 : 0; }
int  nn_camera_get_norm(void)              { return nncam_norm_signed; }
