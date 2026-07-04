/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_stedgeai.c
 * @brief   X-CUBE-AI (ST Edge AI Core / STAI) nn backend (issue #81 / Epic #80).
 *
 * Bridges the ST Edge AI Core generated network (port/nn/generated/, produced by
 * `stedgeai generate`) to the backend-agnostic nn vtable (nn_backend.h).  Uses
 * the modern STAI API (stai_network_*; the generated code and the ABI2.1 GCC/CM7
 * runtime static library `NetworkRuntime*_CM7_GCC.a`).  Compiled only when
 * CONFIG_NN_BACKEND=stedgeai (CMakeLists.txt), which also links the runtime lib
 * and adds the generated + ST AI include paths.
 *
 * Buffers:
 *   - context  : STAI_NETWORK_CONTEXT_SIZE opaque bytes (.bss, 8-aligned)
 *   - weights  : g_network_weights_array (Flash .rodata) -- auto-bound by init
 *   - activations: STAI_NETWORK_ACTIVATIONS_SIZE bytes placed in the .sdram.ai
 *     arena (FMC bank3); the input/output tensors live INSIDE it (PREALLOCATED),
 *     so set_activations() computes their addresses (returned by get_inputs/outputs).
 *
 * Model-agnostic: tensor descriptors (shape / dtype / quant) are built at open()
 * from stai_network_get_info(), so the same TU serves MNIST (1-in/1-out) and
 * BlazeFace (1-in/2-out) after a regenerate -- no per-model edits here.
 *
 * The generated code + ST-SLA runtime lib are NOT committed (public repo); see
 * .gitignore and docs/{ja,en}/ai/inference.md.
 */
#include "nn.h"
#include "nn_backend.h"

#include "network.h"        /* generated: STAI_NETWORK_* macros, includes stai.h */

#include <stddef.h>
#include <stdint.h>

/* This backend assumes exactly one input and one activation arena, and handles
 * up to 4 outputs (covers MNIST 1-out and BlazeFace-front 4-out: 2 score + 2 box
 * tensors, 2 anchor scales).  Guard so a future model with a different shape
 * fails loudly at compile time (codex suggestion). */
#if STAI_NETWORK_IN_NUM != 1
#error "nn_stedgeai: exactly one input assumed -- extend the backend"
#endif
#if STAI_NETWORK_ACTIVATIONS_NUM != 1
#error "nn_stedgeai: exactly one activation buffer assumed -- extend the backend"
#endif
#if STAI_NETWORK_OUT_NUM > 4
#error "nn_stedgeai: extend the output-tensor ladder (>4 outputs)"
#endif

/* Quantized (int8/uint8) tensors have SCALE/ZERO_POINT macros; float32 I/O
 * tensors (e.g. BlazeFace) do not.  Default the missing ones to 0 (nn_tensor
 * treats scale==0 as "not quantized") so the macro-based build works for both. */
#ifndef STAI_NETWORK_IN_1_SCALE
#define STAI_NETWORK_IN_1_SCALE 0.0f
#endif
#ifndef STAI_NETWORK_IN_1_ZERO_POINT
#define STAI_NETWORK_IN_1_ZERO_POINT 0
#endif
#ifndef STAI_NETWORK_OUT_1_SCALE
#define STAI_NETWORK_OUT_1_SCALE 0.0f
#endif
#ifndef STAI_NETWORK_OUT_1_ZERO_POINT
#define STAI_NETWORK_OUT_1_ZERO_POINT 0
#endif
#ifndef STAI_NETWORK_OUT_2_SCALE
#define STAI_NETWORK_OUT_2_SCALE 0.0f
#endif
#ifndef STAI_NETWORK_OUT_2_ZERO_POINT
#define STAI_NETWORK_OUT_2_ZERO_POINT 0
#endif
#ifndef STAI_NETWORK_OUT_3_SCALE
#define STAI_NETWORK_OUT_3_SCALE 0.0f
#endif
#ifndef STAI_NETWORK_OUT_3_ZERO_POINT
#define STAI_NETWORK_OUT_3_ZERO_POINT 0
#endif
#ifndef STAI_NETWORK_OUT_4_SCALE
#define STAI_NETWORK_OUT_4_SCALE 0.0f
#endif
#ifndef STAI_NETWORK_OUT_4_ZERO_POINT
#define STAI_NETWORK_OUT_4_ZERO_POINT 0
#endif

/* Opaque network context (8-aligned per STAI_NETWORK_CONTEXT_ALIGNMENT). */
static uint8_t stai_ctx[STAI_NETWORK_CONTEXT_SIZE] __attribute__((aligned(8)));

/* Activation arena in .sdram.ai (bank3).  32-aligned satisfies the runtime's
 * ACTIVATION alignment and keeps it on a cache line for a future cacheable MPU
 * remap.  Inputs/outputs are carved from here by set_activations(). */
static uint8_t stai_acts[STAI_NETWORK_ACTIVATIONS_SIZE]
	__attribute__((aligned(32), section(".sdram.ai")));

struct stai_model {
	struct nn_tensor in[STAI_NETWORK_IN_NUM];
	struct nn_tensor out[STAI_NETWORK_OUT_NUM];
	int         n_in;
	int         n_out;
	const char *name;
};
static struct stai_model g_stm;

static uint8_t stai_map_dtype(stai_format f)
{
	switch (f) {
	case STAI_FORMAT_S8:  return NN_DTYPE_INT8;
	case STAI_FORMAT_U8:  return NN_DTYPE_UINT8;
	case STAI_FORMAT_S16: return NN_DTYPE_INT16;
	case STAI_FORMAT_S32: return NN_DTYPE_INT32;
	default:              return NN_DTYPE_FLOAT32;
	}
}

/* Build a tensor descriptor from the generated per-index STAI_NETWORK_* macros.
 * stai_network_get_info() is NOT emitted by the lite runtime (it does not link),
 * so the compile-time macros are the metadata source; get_inputs/get_outputs
 * still supply the data pointers (inside the activation buffer). */
static void stai_fill(struct nn_tensor *t, void *data, uint32_t bytes,
                      stai_format fmt, float scale, int32_t zp,
                      int ndim, const int32_t *shape)
{
	t->data  = data;
	t->bytes = bytes;
	t->ndim  = (uint8_t)(ndim <= NN_MAX_DIMS ? ndim : NN_MAX_DIMS);
	for (int i = 0; i < NN_MAX_DIMS; i++)
		t->dims[i] = (i < ndim) ? (uint16_t)shape[i] : 1;
	t->dtype      = stai_map_dtype(fmt);
	t->scale      = scale;
	t->zero_point = zp;
}

#define STAI_SHAPE_NDIM(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))

static int stai_bk_init(void)
{
	return (stai_runtime_init() == STAI_SUCCESS) ? 0 : -1;
}

static int stai_bk_open(void **impl_out)
{
	stai_network *net = (stai_network *)stai_ctx;
	stai_ptr      acts[STAI_NETWORK_ACTIVATIONS_NUM];
	stai_ptr      ins[STAI_NETWORK_IN_NUM];
	stai_ptr      outs[STAI_NETWORK_OUT_NUM];
	stai_size     n;

	if (stai_network_init(net) != STAI_SUCCESS)
		return -1;
	acts[0] = (stai_ptr)stai_acts;
	if (stai_network_set_activations(net, acts, STAI_NETWORK_ACTIVATIONS_NUM) != STAI_SUCCESS)
		return -2;

	/* After set_activations() the input/output addresses live inside the arena;
	 * validate the counts/pointers rather than trusting them blindly. */
	n = 0;
	if (stai_network_get_inputs(net, ins, &n) != STAI_SUCCESS ||
	    n != STAI_NETWORK_IN_NUM || ins[0] == NULL)
		return -3;
	n = 0;
	if (stai_network_get_outputs(net, outs, &n) != STAI_SUCCESS ||
	    n != STAI_NETWORK_OUT_NUM || outs[0] == NULL)
		return -4;

	g_stm.n_in  = STAI_NETWORK_IN_NUM;
	g_stm.n_out = STAI_NETWORK_OUT_NUM;
	g_stm.name  = STAI_NETWORK_ORIGIN_MODEL_NAME;

	{ static const int32_t sh[] = STAI_NETWORK_IN_1_SHAPE;
	  stai_fill(&g_stm.in[0], ins[0], STAI_NETWORK_IN_1_SIZE_BYTES,
	            STAI_NETWORK_IN_1_FORMAT, STAI_NETWORK_IN_1_SCALE,
	            STAI_NETWORK_IN_1_ZERO_POINT, STAI_SHAPE_NDIM(sh), sh); }

	{ static const int32_t sh[] = STAI_NETWORK_OUT_1_SHAPE;
	  stai_fill(&g_stm.out[0], outs[0], STAI_NETWORK_OUT_1_SIZE_BYTES,
	            STAI_NETWORK_OUT_1_FORMAT, STAI_NETWORK_OUT_1_SCALE,
	            STAI_NETWORK_OUT_1_ZERO_POINT, STAI_SHAPE_NDIM(sh), sh); }
#if STAI_NETWORK_OUT_NUM >= 2
	{ static const int32_t sh[] = STAI_NETWORK_OUT_2_SHAPE;
	  stai_fill(&g_stm.out[1], outs[1], STAI_NETWORK_OUT_2_SIZE_BYTES,
	            STAI_NETWORK_OUT_2_FORMAT, STAI_NETWORK_OUT_2_SCALE,
	            STAI_NETWORK_OUT_2_ZERO_POINT, STAI_SHAPE_NDIM(sh), sh); }
#endif
#if STAI_NETWORK_OUT_NUM >= 3
	{ static const int32_t sh[] = STAI_NETWORK_OUT_3_SHAPE;
	  stai_fill(&g_stm.out[2], outs[2], STAI_NETWORK_OUT_3_SIZE_BYTES,
	            STAI_NETWORK_OUT_3_FORMAT, STAI_NETWORK_OUT_3_SCALE,
	            STAI_NETWORK_OUT_3_ZERO_POINT, STAI_SHAPE_NDIM(sh), sh); }
#endif
#if STAI_NETWORK_OUT_NUM >= 4
	{ static const int32_t sh[] = STAI_NETWORK_OUT_4_SHAPE;
	  stai_fill(&g_stm.out[3], outs[3], STAI_NETWORK_OUT_4_SIZE_BYTES,
	            STAI_NETWORK_OUT_4_FORMAT, STAI_NETWORK_OUT_4_SCALE,
	            STAI_NETWORK_OUT_4_ZERO_POINT, STAI_SHAPE_NDIM(sh), sh); }
#endif

	*impl_out = &g_stm;
	return 0;
}

static void stai_bk_close(void *impl)
{
	(void)impl;
	(void)stai_network_deinit((stai_network *)stai_ctx);
}

static const char *stai_bk_name(void *impl) { return ((struct stai_model *)impl)->name; }
static int stai_bk_in_count(void *impl)  { return ((struct stai_model *)impl)->n_in; }
static int stai_bk_out_count(void *impl) { return ((struct stai_model *)impl)->n_out; }

static struct nn_tensor *stai_bk_input(void *impl, int idx)
{
	struct stai_model *m = impl;
	return (idx >= 0 && idx < m->n_in) ? &m->in[idx] : NULL;
}

static struct nn_tensor *stai_bk_output(void *impl, int idx)
{
	struct stai_model *m = impl;
	return (idx >= 0 && idx < m->n_out) ? &m->out[idx] : NULL;
}

static uint32_t stai_bk_acts_bytes(void *impl)
{
	(void)impl;
	return STAI_NETWORK_ACTIVATIONS_SIZE;
}

static int stai_bk_run(void *impl)
{
	(void)impl;
	return (stai_network_run((stai_network *)stai_ctx, STAI_MODE_SYNC) == STAI_SUCCESS) ? 0 : -1;
}

const struct nn_backend_vt nn_backend_vt_selected = {
	.info = &(const struct nn_backend_info){ .name = "stedgeai", .version = "STAI 4.0" },
	.init = stai_bk_init,
	.open = stai_bk_open,
	.close = stai_bk_close,
	.model_name = stai_bk_name,
	.in_count = stai_bk_in_count,
	.out_count = stai_bk_out_count,
	.input = stai_bk_input,
	.output = stai_bk_output,
	.activations_bytes = stai_bk_acts_bytes,
	.run = stai_bk_run,
};
