/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    tflm_spike.cc
 * @brief   TFLM feasibility self-test (Epic #80 P3 M1, issue #86).
 *
 * Runs one hello_world int8 inference THROUGH the backend-agnostic nn API (so it
 * exercises the real MicroInterpreter via nn_tflm.cc plus nn.c's DWT timing and
 * session guard), and proves the C++ runtime is healthy on the F746:
 *   - a file-scope C++ object's ctor writes a sentinel -> proves __libc_init_array
 *     ran the static ctors (flag-only, no HAL/clock/SDRAM dependency);
 *   - the sine approximation matches a precomputed golden within tolerance;
 *   - no heap was touched (g_tflm_cxx_new_calls stays 0).
 * The `ai selftest` shell hook (cmd_ai.c, CONFIG_NN_BACKEND_TFLM only) drives it.
 * All math is single-precision float (no double / no libm sinf) to stay off the
 * soft-float path on this fpv5-sp-d16 core.  Temporary: removed when M2 lands.
 */
#include "nn.h"

#include <cstdint>

extern "C" volatile uint32_t g_tflm_cxx_new_calls;   /* from cxx_runtime.cc */

namespace {
/* Static-ctor probe: writes a sentinel at C++ init time.  FLAG ONLY -- runs before
 * main()/HAL/clock/SDRAM, so it must not touch any of them. */
volatile uint32_t g_ctor_sentinel = 0;
struct CtorProbe { CtorProbe() { g_ctor_sentinel = 0xC0DECAFEu; } };
CtorProbe g_ctor_probe;
}  /* namespace */

extern "C" uint32_t tflm_spike_ctor_sentinel(void) { return g_ctor_sentinel; }
extern "C" uint32_t tflm_spike_new_calls(void)     { return g_tflm_cxx_new_calls; }

/**
 * Run one inference.  @p x_milli is the input angle in 1/1000 units (e.g. 1570 =
 * 1.57 rad).  On success returns 0 and writes @p *y_milli = sin-approx * 1000.
 * Negative on error: -1 open, -2 tensors, -3 dtype/scale, -4 session busy,
 * -5 run, -6 null @p y_milli.
 */
extern "C" int tflm_spike_selftest(int32_t x_milli, int32_t *y_milli)
{
	struct nn_model *m = nullptr;
	if (!y_milli)
		return -6;
	if (nn_model_open(&m) != 0)
		return -1;

	/* Claim the single inference session BEFORE touching the shared model input, so
	 * a concurrent `ai bench` / `ai selftest` (another shell) cannot corrupt each
	 * other's input tensor.  Every exit path below releases it. */
	if (nn_session_try_acquire() != 0)
		return -4;

	struct nn_tensor *in  = nn_input(m, 0);
	struct nn_tensor *out = nn_output(m, 0);
	if (!in || !out || !in->data || !out->data) {
		nn_session_release();
		return -2;
	}
	if (in->dtype != NN_DTYPE_INT8 || out->dtype != NN_DTYPE_INT8 ||
	    !(in->scale > 0.0f)) {
		nn_session_release();
		return -3;
	}

	/* Quantize x (real) -> int8: q = round(x/scale) + zero_point, clamped. */
	float xf = (float)x_milli / 1000.0f;
	float qf = xf / in->scale + (float)in->zero_point;
	int qi = (int)(qf + (qf >= 0.0f ? 0.5f : -0.5f));
	if (qi < -128) qi = -128;
	else if (qi > 127) qi = 127;
	((int8_t *)in->data)[0] = (int8_t)qi;

	int rc = nn_run(m);
	if (rc != 0) {
		nn_session_release();
		return -5;
	}

	/* Dequantize output int8 -> real, back to milli units. */
	int8_t oq = ((int8_t *)out->data)[0];
	float yf = ((float)oq - (float)out->zero_point) * out->scale;
	*y_milli = (int32_t)(yf * 1000.0f + (yf >= 0.0f ? 0.5f : -0.5f));
	nn_session_release();
	return 0;
}
