/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_tflm.cc
 * @brief   TensorFlow Lite Micro (tflite-micro) nn backend (Epic #80 P3, issue #88).
 *
 * Bridges a TFLM MicroInterpreter to the backend-agnostic nn vtable (nn_backend.h).
 * Compiled only when CONFIG_NN_BACKEND=tflm, into the `tflm` static lib alongside
 * the tflite-micro tree fetched + generated at CMake configure time
 * (cmake/tflite-micro.cmake).  M1 (#86) wired a tiny hello_world spike; M2 (#88)
 * runs the real **BlazeFace-front 128 int8** face-detection model, so `ai info` /
 * `ai bench` / `ai stream` / `gui overlay` all execute through the real runtime.
 *
 * The model is int8-compute with float32 I/O: a QUANTIZE op at the input and four
 * DEQUANTIZE ops at the outputs, so this backend exposes one float32 input
 * (1x128x128x3) and four float32 outputs (box/score at two anchor scales).  The
 * op set is exactly QUANTIZE, CONV_2D, DEPTHWISE_CONV_2D, ADD, PAD, MAX_POOL_2D,
 * RESHAPE, DEQUANTIZE (no LOGISTIC/SOFTMAX -- activations are fused), registered
 * one-to-one in the capacity-8 op resolver.  The tensor descriptors are built
 * generically from the interpreter (dtype/shape/quant), so blazeface.c locates
 * the four outputs by SHAPE and this file stays model-agnostic beyond the op set.
 *
 * The interpreter is constructed lazily in open() (placement-new into a static
 * buffer, so no global ctor runs before SDRAM/clock are up, and the heap is never
 * touched).  run() is CPU-bound and never yields (nn.c times it with the DWT cycle
 * counter).  The activation arena lives in .sdram.ai (FMC bank3, MPU cacheable
 * WBWA / CPU-only, issue #6), matching the stedgeai backend.
 */
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "blazeface_model_data.h"        /* g_blazeface_model_data[/_size] (generated) */

#include "nn.h"
#include "nn_backend.h"

#include <new>
#include <cstddef>
#include <cstdint>

/* Canary (codex review): when built with CMSIS-NN, the SIMD int8 kernels are only
 * selected if the compiler advertises the DSP extension.  Cortex-M7 is armv7e-m so
 * GCC defines __ARM_FEATURE_DSP; if a flag change ever drops it, fail the build loud
 * rather than silently falling back to CMSIS-NN's plain-C path. */
#if defined(NN_TFLM_CMSIS_NN) && !defined(__ARM_FEATURE_DSP)
#error "CMSIS-NN build without __ARM_FEATURE_DSP -- DSP int8 path not selected (check -mcpu)"
#endif

namespace {

/* Activation arena in .sdram.ai (bank3).  BlazeFace's activations measured
 * ~328 KB with ST Edge AI; TFLM's greedy planner is the same order.  512 KB is a
 * documented reservation with headroom -- the true figure is reported by
 * arena_used_bytes() (see tflm_bk_acts_bytes).  bank3 is 2 MB and also holds the
 * 2x192 KB camera staging (nn_camera.c): 384 KB + 512 KB = 896 KB < 2 MB (linker
 * ASSERT enforces the fit).  32-aligned matches the other .sdram.ai residents and
 * TFLM's kBufferAlignment. */
constexpr int kArenaSize = 512 * 1024;
alignas(32) uint8_t g_arena[kArenaSize] __attribute__((section(".sdram.ai")));

/* BlazeFace op set (8 builtins, one resolver slot each). */
using OpResolver = tflite::MicroMutableOpResolver<8>;

struct tflm_model {
	struct nn_tensor in[1];             /* BlazeFace: single 1x128x128x3 f32 input */
	struct nn_tensor out[NN_MAX_IO];    /* up to 8 outputs (BlazeFace uses 4)      */
	int n_in;
	int n_out;
	uint32_t used;                      /* arena_used_bytes() after AllocateTensors */
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

	const tflite::Model *model = tflite::GetModel(g_blazeface_model_data);
	if (model->version() != TFLITE_SCHEMA_VERSION)
		return -1;

	/* Function-local statics: constructed on first open() (post-boot, SDRAM up),
	 * serialized by nn.c's PRIMASK open latch (-fno-threadsafe-statics = plain flag).
	 * resolver_ready latches the op registration so a re-open after a later-stage
	 * failure does not register the ops twice into the capacity-8 resolver. */
	static OpResolver resolver;
	static bool resolver_ready = false;
	if (!resolver_ready) {
		if (resolver.AddQuantize()        != kTfLiteOk ||
		    resolver.AddConv2D()          != kTfLiteOk ||
		    resolver.AddDepthwiseConv2D() != kTfLiteOk ||
		    resolver.AddAdd()             != kTfLiteOk ||
		    resolver.AddPad()             != kTfLiteOk ||
		    resolver.AddMaxPool2D()       != kTfLiteOk ||
		    resolver.AddReshape()         != kTfLiteOk ||
		    resolver.AddDequantize()      != kTfLiteOk)
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
		return -3;   /* arena too small -> bump kArenaSize (see cmd_ai `ai info`) */

	/* Validate the I/O shape before publishing descriptors: exactly one input,
	 * 1..NN_MAX_IO outputs, every tensor present with a non-NULL buffer and a rank
	 * within NN_MAX_DIMS (codex plan review -- fail loud instead of OOB later). */
	size_t n_in  = g_interp->inputs_size();
	size_t n_out = g_interp->outputs_size();
	if (n_in != 1)
		return -5;
	if (n_out < 1 || n_out > (size_t)NN_MAX_IO)
		return -6;

	for (size_t i = 0; i < n_in; i++) {
		TfLiteTensor *t = g_interp->input(i);
		if (!t || !t->data.data || !t->dims || t->dims->size > NN_MAX_DIMS)
			return -4;
	}
	for (size_t i = 0; i < n_out; i++) {
		TfLiteTensor *t = g_interp->output(i);
		if (!t || !t->data.data || !t->dims || t->dims->size > NN_MAX_DIMS)
			return -4;
	}

	fill_tensor(&g_tm.in[0], g_interp->input(0));
	for (size_t i = 0; i < n_out; i++)
		fill_tensor(&g_tm.out[i], g_interp->output(i));

	g_tm.n_in  = (int)n_in;
	g_tm.n_out = (int)n_out;
	g_tm.used  = (uint32_t)g_interp->arena_used_bytes();
	g_tm.name  = "blazeface_front_128 (tflm)";
	g_tm.open  = true;
	*impl_out  = &g_tm;
	return 0;
}

static void tflm_bk_close(void *impl)
{
	(void)impl;   /* singleton; keep the interpreter alive */
}

static const char *tflm_bk_name(void *impl) { return ((struct tflm_model *)impl)->name; }
static int tflm_bk_in_count(void *impl)  { return ((struct tflm_model *)impl)->n_in; }
static int tflm_bk_out_count(void *impl) { return ((struct tflm_model *)impl)->n_out; }

static struct nn_tensor *tflm_bk_input(void *impl, int idx)
{
	struct tflm_model *m = (struct tflm_model *)impl;
	return (idx >= 0 && idx < m->n_in) ? &m->in[idx] : nullptr;
}

static struct nn_tensor *tflm_bk_output(void *impl, int idx)
{
	struct tflm_model *m = (struct tflm_model *)impl;
	return (idx >= 0 && idx < m->n_out) ? &m->out[idx] : nullptr;
}

/* Report the arena TFLM actually planned (not the 512 KB reservation), so `ai info`
 * compares apples-to-apples with the stedgeai backend's ACTIVATIONS_SIZE. */
static uint32_t tflm_bk_acts_bytes(void *impl) { return ((struct tflm_model *)impl)->used; }

static int tflm_bk_run(void *impl)
{
	(void)impl;
	return (g_interp && g_interp->Invoke() == kTfLiteOk) ? 0 : -1;
}

static const struct nn_backend_info g_info = { "tflm", "tflite-micro (BlazeFace)" };

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
