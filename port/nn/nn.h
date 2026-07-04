/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn.h
 * @brief   Backend-agnostic on-device neural-network inference API (issue #81).
 *
 * A thin tensor-in / tensor-out abstraction over one selectable inference
 * runtime.  Exactly one backend is compiled in per build (see CONFIG_NN_BACKEND
 * in CMakeLists.txt): `null` (no runtime -- plumbing/scaffold, always builds),
 * `stedgeai` (X-CUBE-AI / ST Edge AI Core), and later `tflm` (LiteRT).  The
 * abstraction is deliberately model-agnostic: model-specific pre/post-processing
 * (e.g. BlazeFace anchor decode + NMS) lives ABOVE this layer in
 * port/nn/models/, so the same nn API serves any model the backend can run.
 *
 * Layering (HAL/CMSIS/ThreadX <- svc <- port <- ui <- shell/src): this is a
 * port/ module.  It depends only on the compiled-in backend and the svc/ layer;
 * it never reaches up into shell/ or ui/.
 *
 * Threading: a single model instance is NOT reentrant.  For P1, nn_run() is
 * driven from one inference worker thread (or, for `ai bench`, the CLI thread
 * while nothing else runs it); callers serialize access.  nn_run() is CPU-bound
 * and never sleeps, so the DWT cycle counter it uses for nn_last_cycles() does
 * not freeze mid-run (the counter only stops in WFI -- see cmd_membench.c).
 */
#ifndef NN_H
#define NN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Element type of a tensor buffer. */
enum nn_dtype {
	NN_DTYPE_NONE = 0,
	NN_DTYPE_INT8,
	NN_DTYPE_UINT8,
	NN_DTYPE_INT16,
	NN_DTYPE_INT32,
	NN_DTYPE_FLOAT32,
};

/** Maximum tensor rank and per-model I/O tensor count this API exposes. */
#define NN_MAX_DIMS 4
#define NN_MAX_IO   8

/**
 * A single input or output tensor.  @p data points at the backend/model-owned
 * buffer the caller fills (inputs) or reads (outputs); the caller does not own
 * or free it.  For quantized (int8/uint8) tensors @p scale and @p zero_point
 * carry the affine quantization params (real = scale * (q - zero_point));
 * @p scale is 0 for non-quantized tensors.  Dims are most- to least-significant
 * (e.g. NHWC: dims[0]=N, dims[1]=H, dims[2]=W, dims[3]=C).
 */
struct nn_tensor {
	void    *data;                 /**< tensor buffer (backend-owned)         */
	uint32_t bytes;                /**< buffer size in bytes                  */
	uint16_t dims[NN_MAX_DIMS];    /**< shape, MSB..LSB; unused dims are 1    */
	uint8_t  ndim;                 /**< number of valid dims                  */
	uint8_t  dtype;               /**< enum nn_dtype                          */
	float    scale;               /**< quant scale (0 => not quantized)       */
	int32_t  zero_point;          /**< quant zero point                       */
};

/** Opaque model handle (defined in nn.c). */
struct nn_model;

/** Identity of the compiled-in backend, for `ai info`. */
struct nn_backend_info {
	const char *name;      /**< "null" | "stedgeai" | "tflm"                  */
	const char *version;   /**< backend/runtime version string, or ""         */
};

/**
 * One-time backend init.  Idempotent and cheap to call repeatedly (the shell
 * calls it lazily on first `ai` use).  Returns 0 on success, <0 on failure.
 */
int nn_init(void);

/** Identity of the compiled-in backend (never NULL). */
const struct nn_backend_info *nn_backend(void);

/**
 * Open the (single, compiled-in) model.  For P1 there is one model per build
 * (BlazeFace-128 with the stedgeai backend; a synthetic BlazeFace-shaped stub
 * with the null backend).  Returns 0 and sets @p *out on success, <0 otherwise.
 * The handle is a singleton; a second open returns the same instance.
 */
int  nn_model_open(struct nn_model **out);
void nn_model_close(struct nn_model *m);

/** Human-readable model name (never NULL). */
const char *nn_model_name(const struct nn_model *m);

int  nn_input_count(const struct nn_model *m);
int  nn_output_count(const struct nn_model *m);

/** Input/output tensor @p idx, or NULL if out of range. */
struct nn_tensor *nn_input(struct nn_model *m, int idx);
struct nn_tensor *nn_output(struct nn_model *m, int idx);

/** Size of the activation arena (bytes), for `ai info`; 0 if unknown. */
uint32_t nn_activations_bytes(const struct nn_model *m);

/**
 * Run one inference.  Inputs must be filled first; outputs are valid on return.
 * CPU-bound and blocking (no sleep).  Returns 0 on success, <0 on error.  The
 * elapsed DWT cycle count is captured for nn_last_cycles().
 */
int nn_run(struct nn_model *m);

/** DWT core cycles spent inside the most recent successful nn_run(); 0 if none. */
uint32_t nn_last_cycles(const struct nn_model *m);

/*
 * Coarse single-session guard.  The singleton model + the backends are NOT
 * reentrant, and multiple shells (UART + telnet) can issue `ai` commands, so
 * only ONE inference activity may use the model at a time: a one-shot `ai bench`,
 * or a live `ai run` / `ai stream`.  try_acquire() claims the session (returns 0)
 * or reports it busy (<0); release() frees it.  Interrupt-guarded and
 * thread-agnostic -- the acquiring and releasing threads may differ (stream start
 * in one shell, stop in another).  No init required.
 */
int  nn_session_try_acquire(void);
void nn_session_release(void);

#ifdef __cplusplus
}
#endif

#endif /* NN_H */
