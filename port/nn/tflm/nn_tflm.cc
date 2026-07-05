/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_tflm.cc
 * @brief   TensorFlow Lite Micro (tflite-micro) nn backend (Epic #80 P3/P2).
 *
 * Bridges a TFLM MicroInterpreter to the backend-agnostic nn vtable (nn_backend.h).
 * Compiled only when CONFIG_NN_BACKEND=tflm, into the `tflm` static lib alongside
 * the tflite-micro tree fetched + generated at CMake configure time
 * (cmake/tflite-micro.cmake).  Runs the built-in **BlazeFace-front 128 int8**
 * face-detection model (#88), and -- because TFLM interprets any .tflite flatbuffer
 * in RAM -- also a model loaded at runtime from the microSD card (#89 P2).
 *
 * Runtime model swap (#89): TFLM's op set is fixed at compile time by the resolver,
 * so this backend registers a broad common-vision op SUPERSET (23 ops) once, and an
 * SD-loaded model that stays within it runs with no rebuild.  Two 512 KB model slots
 * in .sdram.ai are double-buffered: load_region() hands out the INACTIVE slot so
 * reading a new .tflite never corrupts the flatbuffer the live interpreter is still
 * referencing, and reload() is transactional -- on any failure it rebuilds the
 * previous (known-good) model and reports it.
 *
 * The interpreter is constructed lazily in open() (placement-new into a static
 * buffer, so no global ctor runs before SDRAM/clock are up, and the heap is never
 * touched).  ~MicroInterpreter() is NOT a no-op (it calls FreeSubgraphs()), so a
 * rebuild explicitly destroys the old one first (destroy_interp()).  run() is
 * CPU-bound and never yields (nn.c times it with the DWT cycle counter).  The
 * activation arena lives in .sdram.ai (FMC bank3, MPU cacheable WBWA / CPU-only,
 * issue #6), matching the stedgeai backend.
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
 * ~470 KB; 512 KB is a documented reservation with headroom -- the true figure is
 * reported by arena_used_bytes() (see tflm_bk_acts_bytes).  bank3 is 2 MB and also
 * holds the 2x192 KB camera staging (nn_camera.c) and the 2x512 KB SD model slots
 * below: 384 + 512 + 1024 = 1920 KB < 2 MB (linker ASSERT enforces the fit). */
constexpr int kArenaSize = 512 * 1024;
alignas(32) uint8_t g_arena[kArenaSize] __attribute__((section(".sdram.ai")));

/* Two SD model slots (#89 P2), double-buffered so loading a new .tflite never
 * overwrites the flatbuffer the live interpreter is still reading.  16-aligned for
 * tflite::GetModel().  g_sd_slot names the slot holding the ACTIVE model, or -1
 * when the built-in model is active. */
#ifndef NN_TFLM_SD_MODEL_MAX
#define NN_TFLM_SD_MODEL_MAX (512u * 1024u)
#endif
alignas(16) uint8_t g_sd_model_buf[2][NN_TFLM_SD_MODEL_MAX]
	__attribute__((section(".sdram.ai")));
int g_sd_slot = -1;

/* Broad common-vision op superset (23), registered once.  BlazeFace uses only 8
 * of these; the rest let an SD-loaded int8 classifier/detector run without a
 * rebuild.  Unregistered kernels are compiled but dropped by --gc-sections, so the
 * Flash cost is only the kept kernels. */
using OpResolver = tflite::MicroMutableOpResolver<24>;

struct tflm_model {
	struct nn_tensor in[1];             /* one input assumed (BlazeFace / classifiers) */
	struct nn_tensor out[NN_MAX_IO];    /* up to 8 outputs (BlazeFace uses 4)          */
	int n_in;
	int n_out;
	uint32_t used;                      /* arena_used_bytes() after AllocateTensors     */
	const char *name;                   /* -> g_model_name                              */
	bool open;
};
tflm_model g_tm;
char g_model_name[64];

/* The active flatbuffer + its length (for `ai model` display).  Defaults to the
 * built-in BlazeFace model; redirected to an SD slot by reload(). */
const void *g_active_model = g_blazeface_model_data;
uint32_t    g_active_len;                    /* lazily set to the built-in size */

tflite::MicroInterpreter *g_interp = nullptr;

const char *const kBuiltinName = "blazeface_front_128 (tflm)";

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

/* Copy a model display name into g_model_name (backend-owned, so `ai info` never
 * holds a dangling pointer into argv/path).  slot<0 => the built-in name; else
 * "sd:<name>". */
void set_model_name(const char *name, int slot)
{
	const int cap = (int)sizeof(g_model_name) - 1;
	int n = 0;

	if (slot < 0) {
		for (const char *s = kBuiltinName; *s && n < cap; s++)
			g_model_name[n++] = *s;
	} else {
		const char *pfx = "sd:";
		for (const char *s = pfx; *s && n < cap; s++)
			g_model_name[n++] = *s;
		if (name)
			for (const char *s = name; *s && n < cap; s++)
				g_model_name[n++] = *s;
	}
	g_model_name[n] = '\0';
	g_tm.name = g_model_name;
}

/* Register the 23-op superset into a single resolver, exactly once. */
OpResolver *get_resolver()
{
	static OpResolver resolver;
	static bool ready = false;
	if (ready)
		return &resolver;
	if (resolver.AddConv2D()               == kTfLiteOk &&
	    resolver.AddDepthwiseConv2D()       == kTfLiteOk &&
	    resolver.AddFullyConnected()        == kTfLiteOk &&
	    resolver.AddAveragePool2D()         == kTfLiteOk &&
	    resolver.AddMaxPool2D()             == kTfLiteOk &&
	    resolver.AddSoftmax()               == kTfLiteOk &&
	    resolver.AddLogistic()              == kTfLiteOk &&
	    resolver.AddRelu()                  == kTfLiteOk &&
	    resolver.AddRelu6()                 == kTfLiteOk &&
	    resolver.AddReshape()               == kTfLiteOk &&
	    resolver.AddQuantize()              == kTfLiteOk &&
	    resolver.AddDequantize()            == kTfLiteOk &&
	    resolver.AddPad()                   == kTfLiteOk &&
	    resolver.AddAdd()                   == kTfLiteOk &&
	    resolver.AddMul()                   == kTfLiteOk &&
	    resolver.AddSub()                   == kTfLiteOk &&
	    resolver.AddMean()                  == kTfLiteOk &&
	    resolver.AddConcatenation()         == kTfLiteOk &&
	    resolver.AddResizeBilinear()        == kTfLiteOk &&
	    resolver.AddResizeNearestNeighbor() == kTfLiteOk &&
	    resolver.AddStridedSlice()          == kTfLiteOk &&
	    resolver.AddLeakyRelu()             == kTfLiteOk &&
	    resolver.AddTranspose()             == kTfLiteOk) {
		ready = true;
		return &resolver;
	}
	return nullptr;
}

alignas(tflite::MicroInterpreter) uint8_t g_interp_buf[sizeof(tflite::MicroInterpreter)];

/* ~MicroInterpreter() is NOT trivial (it calls graph_.FreeSubgraphs()), so a
 * rebuild must explicitly destroy the previous interpreter before placement-new. */
void destroy_interp()
{
	if (g_interp) {
		g_interp->~MicroInterpreter();
		g_interp = nullptr;
	}
}

/* Build the interpreter for @p model_data (@p len bytes) and publish the I/O
 * descriptors into g_tm on success.  Leaves g_interp destroyed (nullptr) and does
 * NOT touch g_tm on failure.  Returns 0, or <0: -1 version, -2 resolver, -3 arena,
 * -4 tensor, -5 shape, -6 empty, -7 malformed flatbuffer. */
int build_interp(const void *model_data, uint32_t len)
{
	if (len == 0)
		return -6;
	/* Verify the flatbuffer against its declared length before trusting it, so a
	 * truncated / corrupt SD .tflite fails cleanly here (and reload() then restores
	 * the previous model) instead of faulting deep inside GetModel/AllocateTensors. */
	flatbuffers::Verifier verifier(static_cast<const uint8_t *>(model_data),
	                               (size_t)len);
	if (!tflite::VerifyModelBuffer(verifier))
		return -7;

	const tflite::Model *model = tflite::GetModel(model_data);
	if (model->version() != TFLITE_SCHEMA_VERSION)
		return -1;
	OpResolver *resolver = get_resolver();
	if (!resolver)
		return -2;

	destroy_interp();
	g_interp = new (g_interp_buf)
		tflite::MicroInterpreter(model, *resolver, g_arena, kArenaSize);

	if (g_interp->AllocateTensors() != kTfLiteOk) {
		destroy_interp();
		return -3;
	}

	/* Exactly one input, 1..NN_MAX_IO outputs, every tensor present with a non-NULL
	 * buffer and a rank within NN_MAX_DIMS (fail loud instead of OOB later). */
	size_t n_in  = g_interp->inputs_size();
	size_t n_out = g_interp->outputs_size();
	if (n_in != 1 || n_out < 1 || n_out > (size_t)NN_MAX_IO) {
		destroy_interp();
		return -5;
	}
	for (size_t i = 0; i < n_in; i++) {
		TfLiteTensor *t = g_interp->input(i);
		if (!t || !t->data.data || !t->dims || t->dims->size > NN_MAX_DIMS) {
			destroy_interp();
			return -4;
		}
	}
	for (size_t i = 0; i < n_out; i++) {
		TfLiteTensor *t = g_interp->output(i);
		if (!t || !t->data.data || !t->dims || t->dims->size > NN_MAX_DIMS) {
			destroy_interp();
			return -4;
		}
	}

	fill_tensor(&g_tm.in[0], g_interp->input(0));
	for (size_t i = 0; i < n_out; i++)
		fill_tensor(&g_tm.out[i], g_interp->output(i));
	g_tm.n_in  = (int)n_in;
	g_tm.n_out = (int)n_out;
	g_tm.used  = (uint32_t)g_interp->arena_used_bytes();
	return 0;
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

	if (g_active_len == 0)                   /* lazily default to the built-in model */
		g_active_len = g_blazeface_model_data_size;

	int rc = build_interp(g_active_model, g_active_len);
	if (rc != 0)
		return rc;

	set_model_name(nullptr, g_sd_slot);      /* -1 => built-in name */
	g_tm.open = true;
	*impl_out = &g_tm;
	return 0;
}

static void tflm_bk_close(void *impl)
{
	(void)impl;   /* singleton; keep the interpreter alive */
}

/* Return the INACTIVE slot so filling it cannot corrupt the live flatbuffer. */
static int tflm_bk_load_region(void **buf, uint32_t *cap)
{
	int inactive = (g_sd_slot == 0) ? 1 : 0;
	*buf = g_sd_model_buf[inactive];
	*cap = NN_TFLM_SD_MODEL_MAX;
	return 0;
}

/* Transactional model swap.  data==NULL reverts to the built-in model; otherwise
 * data must point at one of the two SD slots (the one load_region() handed out).
 * On failure the previous known-good model is rebuilt and reported.  *impl_out is
 * ALWAYS the resulting active handle (&g_tm), or NULL if even the restore failed. */
static int tflm_bk_reload(const void *data, uint32_t len, const char *name,
                          void **impl_out)
{
	const void *old_model = g_active_model;
	uint32_t    old_len   = g_active_len;
	int         old_slot  = g_sd_slot;

	const void *new_model;
	int         new_slot;

	if (data == NULL) {
		new_model = g_blazeface_model_data;
		new_slot  = -1;
	} else if (data == g_sd_model_buf[0]) {
		new_model = data; new_slot = 0;
	} else if (data == g_sd_model_buf[1]) {
		new_model = data; new_slot = 1;
	} else {
		*impl_out = &g_tm;               /* not a known slot; nothing changed */
		return -10;
	}

	uint32_t new_len = (data == NULL) ? g_blazeface_model_data_size : len;
	int rc = build_interp(new_model, new_len);
	if (rc == 0) {
		/* Atomic publish: only after build + I/O fill succeeded. */
		g_active_model = new_model;
		g_active_len   = new_len;
		g_sd_slot      = new_slot;
		set_model_name(name, new_slot);
		g_tm.open = true;
		*impl_out = &g_tm;
		return 0;
	}

	/* New model failed -> restore the previous (known-good) model.  Its flatbuffer
	 * is intact (double-buffered slots / Flash built-in), so this should succeed.
	 * g_model_name still holds the old name (only success paths overwrite it), so
	 * no re-derive -- just re-point g_tm.name after build_interp refilled g_tm. */
	if (build_interp(old_model, old_len) == 0) {
		g_active_model = old_model;
		g_active_len   = old_len;
		g_sd_slot      = old_slot;
		g_tm.name      = g_model_name;
		g_tm.open = true;
		*impl_out = &g_tm;
		return rc;                       /* report the original failure */
	}

	/* Catastrophic: even the previous model could not be rebuilt (leave closed so
	 * a later nn_model_open() retries a fresh build). */
	g_tm.open = false;
	*impl_out = nullptr;
	return rc;
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
 * activations_bytes, run, load_region, reload. */
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
	tflm_bk_load_region,
	tflm_bk_reload,
};

}  /* extern "C" */
