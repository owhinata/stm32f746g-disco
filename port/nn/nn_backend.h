/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_backend.h
 * @brief   Internal vtable a concrete nn backend implements (issue #81).
 *
 * nn.c dispatches the public nn.h API to exactly one backend via this vtable.
 * Each backend (nn_null.c, nn_stedgeai.c, later nn_tflm.c) defines its own
 * private model handle type and exports a single `const struct nn_backend_vt
 * nn_backend_vt_selected`.  The build compiles exactly one backend TU (see
 * CONFIG_NN_BACKEND), so the symbol resolves uniquely at link time.
 *
 * The vtable operates on an opaque backend handle (`void *impl`); nn.c wraps it
 * in the public `struct nn_model` and adds cross-backend concerns (DWT timing).
 * run() is the pure inference call -- it must NOT time itself; nn.c owns timing.
 */
#ifndef NN_BACKEND_H
#define NN_BACKEND_H

#include "nn.h"

#ifdef __cplusplus
extern "C" {
#endif

struct nn_backend_vt {
	const struct nn_backend_info *info;

	/** One-time init; idempotent. 0 on success, <0 on failure. */
	int  (*init)(void);

	/** Open the singleton model; set *impl_out. 0 on success, <0 otherwise. */
	int  (*open)(void **impl_out);
	void (*close)(void *impl);

	const char *(*model_name)(void *impl);
	int  (*in_count)(void *impl);
	int  (*out_count)(void *impl);
	struct nn_tensor *(*input)(void *impl, int idx);
	struct nn_tensor *(*output)(void *impl, int idx);
	uint32_t (*activations_bytes)(void *impl);

	/** Pure inference (no timing). Inputs pre-filled; 0 on success, <0 on error. */
	int  (*run)(void *impl);
};

/** The one backend selected at build time (provided by exactly one backend TU). */
extern const struct nn_backend_vt nn_backend_vt_selected;

#ifdef __cplusplus
}
#endif

#endif /* NN_BACKEND_H */
