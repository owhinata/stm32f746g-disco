/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_tflm.cc
 * @brief   TensorFlow Lite Micro (tflite-micro) nn backend (Epic #80 P3, issue #86).
 *
 * Bridges a TFLM MicroInterpreter to the backend-agnostic nn vtable (nn_backend.h).
 * Compiled only when CONFIG_NN_BACKEND=tflm, into the `tflm` static lib alongside the
 * tflite-micro tree fetched + generated at CMake configure time (cmake/tflite-micro.cmake,
 * issue #86).  This M1 spike wires the tiny
 * committed **hello_world int8** sine model (FullyConnected only, int8 1x1 in/out) so
 * that `ai info` / `ai bench` and the `ai selftest` hook run through the *real* runtime
 * and exercise nn.c's DWT timing + session guard.  The real BlazeFace model + CMSIS-NN
 * come in M2 (this file is model-agnostic except for the compiled-in model + op set).
 *
 * The interpreter is constructed lazily in open() (placement-new into a static buffer,
 * so no global ctor runs before SDRAM/clock are up, and the heap is never touched).
 * run() is CPU-bound and never yields (nn.c times it with the DWT cycle counter).
 * The activation arena lives in .sdram.ai (FMC bank3, already MPU cacheable WBWA /
 * CPU-only, issue #6), matching the stedgeai backend.
 */
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "examples/hello_world/models/hello_world_int8_model_data.h"

#include "nn.h"
#include "nn_backend.h"

#include <new>
#include <cstddef>
#include <cstdint>

namespace {

/* Activation arena in .sdram.ai (bank3).  hello_world int8 needs ~3 KB (upstream
 * uses 3000); 4 KB rounds it up.  32-aligned matches the other .sdram.ai residents
 * and TFLM's kBufferAlignment. */
constexpr int kArenaSize = 4096;
alignas(32) uint8_t g_arena[kArenaSize] __attribute__((section(".sdram.ai")));

using OpResolver = tflite::MicroMutableOpResolver<1>;   /* FullyConnected only */

struct tflm_model {
	struct nn_tensor in[1];
	struct nn_tensor out[1];
	const char *name;
	bool open;
};
tflm_model g_tm;

tflite::MicroInterpreter *g_interp = nullptr;

uint8_t map_dtype(TfLiteType t)
{
	switch (t) {
	case kTfLiteInt8:    return NN_DTYPE_INT8;
	case kTfLiteUInt8:   return NN_DTYPE_UINT8;
	case kTfLiteInt16:   return NN_DTYPE_INT16;
	case kTfLiteInt32:   return NN_DTYPE_INT32;
	case kTfLiteFloat32: return NN_DTYPE_FLOAT32;
	default:             return NN_DTYPE_NONE;
	}
}

void fill_tensor(struct nn_tensor *nt, const TfLiteTensor *tt)
{
	nt->data  = tt->data.data;
	nt->bytes = (uint32_t)tt->bytes;
	int nd = tt->dims ? tt->dims->size : 0;
	nt->ndim  = (uint8_t)(nd <= NN_MAX_DIMS ? nd : NN_MAX_DIMS);
	for (int i = 0; i < NN_MAX_DIMS; i++)
		nt->dims[i] = (i < nd) ? (uint16_t)tt->dims->data[i] : 1;
	nt->dtype      = map_dtype(tt->type);
	nt->scale      = tt->params.scale;
	nt->zero_point = tt->params.zero_point;
}

}  /* namespace */

/* Vtable callbacks: internal linkage + C language linkage so their addresses have
 * C function-pointer type, matching the nn_backend_vt fields exactly. */
extern "C" {

static int tflm_bk_init(void)
{
	return 0;   /* TFLM has no global runtime init */
}

static int tflm_bk_open(void **impl_out)
{
	if (g_tm.open) { *impl_out = &g_tm; return 0; }

	const tflite::Model *model = tflite::GetModel(g_hello_world_int8_model_data);
	if (model->version() != TFLITE_SCHEMA_VERSION)
		return -1;

	/* Function-local statics: constructed on first open() (post-boot, SDRAM up),
	 * serialized by nn.c's PRIMASK open latch (-fno-threadsafe-statics = plain flag).
	 * resolver_ready latches the op registration so a re-open after a later-stage
	 * failure does not AddFullyConnected() twice into the capacity-1 resolver. */
	static OpResolver resolver;
	static bool resolver_ready = false;
	if (!resolver_ready) {
		if (resolver.AddFullyConnected() != kTfLiteOk)
			return -2;
		resolver_ready = true;
	}

	/* Construct the interpreter once (guard on g_interp == nullptr).  A re-open after
	 * a later-stage failure reuses the same object rather than placement-new'ing over
	 * a live one (which would skip its destructor). */
	alignas(tflite::MicroInterpreter) static
		uint8_t interp_buf[sizeof(tflite::MicroInterpreter)];
	if (g_interp == nullptr)
		g_interp = new (interp_buf)
			tflite::MicroInterpreter(model, resolver, g_arena, kArenaSize);

	if (g_interp->AllocateTensors() != kTfLiteOk)
		return -3;

	TfLiteTensor *in  = g_interp->input(0);
	TfLiteTensor *out = g_interp->output(0);
	if (!in || !out || !in->data.data || !out->data.data)
		return -4;

	fill_tensor(&g_tm.in[0], in);
	fill_tensor(&g_tm.out[0], out);
	g_tm.name = "hello_world_int8 (tflm spike)";
	g_tm.open = true;
	*impl_out = &g_tm;
	return 0;
}

static void tflm_bk_close(void *impl)
{
	(void)impl;   /* singleton; keep the interpreter alive */
}

static const char *tflm_bk_name(void *impl) { return ((struct tflm_model *)impl)->name; }
static int tflm_bk_in_count(void *impl)  { (void)impl; return 1; }
static int tflm_bk_out_count(void *impl) { (void)impl; return 1; }

static struct nn_tensor *tflm_bk_input(void *impl, int idx)
{
	struct tflm_model *m = (struct tflm_model *)impl;
	return (idx == 0) ? &m->in[0] : nullptr;
}

static struct nn_tensor *tflm_bk_output(void *impl, int idx)
{
	struct tflm_model *m = (struct tflm_model *)impl;
	return (idx == 0) ? &m->out[0] : nullptr;
}

static uint32_t tflm_bk_acts_bytes(void *impl) { (void)impl; return kArenaSize; }

static int tflm_bk_run(void *impl)
{
	(void)impl;
	return (g_interp && g_interp->Invoke() == kTfLiteOk) ? 0 : -1;
}

static const struct nn_backend_info g_info = { "tflm", "tflite-micro (hello_world spike)" };

/* Positional init (C++17: no designated initializers).  Field order MUST match
 * struct nn_backend_vt in nn_backend.h:
 * info, init, open, close, model_name, in_count, out_count, input, output,
 * activations_bytes, run. */
const struct nn_backend_vt nn_backend_vt_selected = {
	&g_info,
	tflm_bk_init,
	tflm_bk_open,
	tflm_bk_close,
	tflm_bk_name,
	tflm_bk_in_count,
	tflm_bk_out_count,
	tflm_bk_input,
	tflm_bk_output,
	tflm_bk_acts_bytes,
	tflm_bk_run,
};

}  /* extern "C" */
