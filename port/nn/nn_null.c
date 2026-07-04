/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_null.c
 * @brief   Null nn backend: BlazeFace-shaped stub, no real runtime (issue #81).
 *
 * The default backend (CONFIG_NN_BACKEND=null) so the firmware always builds
 * without the ST Edge AI Core (`stedgeai`) toolchain or any model.  It exposes a
 * synthetic model whose tensor shapes match BlazeFace Front 128x128 int8 (input
 * 1x128x128x3; outputs 1x896x16 box regressors + 1x896x1 scores), with a REAL,
 * writable input buffer placed in the .sdram.ai arena (FMC bank3).  This lets the
 * full plumbing -- `ai info`, `ai bench`, and the camera inference sink (M2) --
 * be developed and exercised end-to-end before the stedgeai backend exists.
 *
 * run() is a no-op returning success (0 inference cycles of real work), so
 * `ai bench` measures only the nn.c dispatch/timing overhead here.
 *
 * NOTE: the input buffer lives in the .sdram (NOLOAD) region, which is only
 * valid after sdram_init() (tx_application_define()).  nn_model_open() is called
 * lazily from the shell, long after boot, so SDRAM is up by then -- the same
 * lifetime rule the real stedgeai activation arena will follow.
 */
#include "nn.h"
#include "nn_backend.h"

#include <stddef.h>
#include <stdint.h>

/* BlazeFace Front 128x128 int8 shapes (mirrored so the plumbing matches). */
#define NULL_IN_H   128
#define NULL_IN_W   128
#define NULL_IN_C   3
#define NULL_IN_BYTES  ((uint32_t)NULL_IN_H * NULL_IN_W * NULL_IN_C)   /* 49152 */
#define NULL_ANCHORS   896
#define NULL_REG_BYTES ((uint32_t)NULL_ANCHORS * 16)                  /* 14336 */
#define NULL_SCR_BYTES ((uint32_t)NULL_ANCHORS * 1)                   /*   896 */

/* Real input buffer in the NN arena (bank3); outputs are small .bss buffers. */
static int8_t null_in_buf[NULL_IN_BYTES]
	__attribute__((aligned(32), section(".sdram.ai")));
static int8_t null_reg_buf[NULL_REG_BYTES] __attribute__((aligned(4)));
static int8_t null_scr_buf[NULL_SCR_BYTES] __attribute__((aligned(4)));

struct null_model {
	struct nn_tensor in[1];
	struct nn_tensor out[2];
};

static struct null_model g_null;

static int null_init(void)
{
	return 0;
}

static int null_open(void **impl_out)
{
	struct null_model *m = &g_null;

	m->in[0] = (struct nn_tensor){
		.data = null_in_buf, .bytes = NULL_IN_BYTES,
		.dims = { 1, NULL_IN_H, NULL_IN_W, NULL_IN_C }, .ndim = 4,
		.dtype = NN_DTYPE_INT8, .scale = 1.0f / 128.0f, .zero_point = 0,
	};
	m->out[0] = (struct nn_tensor){
		.data = null_reg_buf, .bytes = NULL_REG_BYTES,
		.dims = { 1, NULL_ANCHORS, 16, 1 }, .ndim = 3,
		.dtype = NN_DTYPE_INT8, .scale = 1.0f, .zero_point = 0,
	};
	m->out[1] = (struct nn_tensor){
		.data = null_scr_buf, .bytes = NULL_SCR_BYTES,
		.dims = { 1, NULL_ANCHORS, 1, 1 }, .ndim = 3,
		.dtype = NN_DTYPE_INT8, .scale = 1.0f, .zero_point = 0,
	};

	*impl_out = m;
	return 0;
}

static void null_close(void *impl) { (void)impl; }

static const char *null_model_name(void *impl) { (void)impl; return "null-blazeface-128"; }
static int null_in_count(void *impl)  { (void)impl; return 1; }
static int null_out_count(void *impl) { (void)impl; return 2; }

static struct nn_tensor *null_input(void *impl, int idx)
{
	struct null_model *m = impl;
	return (idx == 0) ? &m->in[0] : NULL;
}

static struct nn_tensor *null_output(void *impl, int idx)
{
	struct null_model *m = impl;
	return (idx >= 0 && idx < 2) ? &m->out[idx] : NULL;
}

static uint32_t null_activations_bytes(void *impl) { (void)impl; return NULL_IN_BYTES; }

static int null_run(void *impl)
{
	(void)impl;
	return 0;   /* no real inference */
}

const struct nn_backend_vt nn_backend_vt_selected = {
	.info = &(const struct nn_backend_info){ .name = "null", .version = "stub" },
	.init = null_init,
	.open = null_open,
	.close = null_close,
	.model_name = null_model_name,
	.in_count = null_in_count,
	.out_count = null_out_count,
	.input = null_input,
	.output = null_output,
	.activations_bytes = null_activations_bytes,
	.run = null_run,
};
