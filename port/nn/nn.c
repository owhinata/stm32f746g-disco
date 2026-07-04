/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn.c
 * @brief   Backend-agnostic nn API dispatcher + inference timing (issue #81).
 *
 * Thin layer over the compiled-in backend (nn_backend_vt_selected).  Adds the
 * two cross-backend concerns the backends must not each reimplement:
 *   - a singleton public `struct nn_model` wrapping the backend's opaque handle,
 *   - DWT CYCCNT timing around nn_run() for nn_last_cycles().
 *
 * DWT: the Cortex-M7 DWT has a software lock that leaves CYCCNT frozen at 0 with
 * no debugger attached until the CoreSight lock key (0xC5ACCE55) is written to
 * the Lock Access Register.  Same technique as cmd_membench.c (issue #57).  The
 * counter only stalls in WFI; nn_run() is CPU-bound and never sleeps, so the
 * elapsed count is the true inference latency (inflated by any preemption, which
 * for a best-effort worker is exactly what we want to measure).
 */
#include "nn.h"
#include "nn_backend.h"

#include "tx_api.h"          /* tx_thread_sleep (open/init serialization) */
#include "stm32f7xx_hal.h"   /* DWT / CoreDebug / __DSB / __ISB / PRIMASK */

/* Public model handle: backend impl + last inference cycle count. */
struct nn_model {
	void    *impl;
	uint32_t last_cycles;
	uint8_t  open;
};

static struct nn_model g_model;     /* single model for P1 */
static uint8_t         g_inited;

/* ---- DWT cycle counter (CoreSight lock key; see cmd_membench.c) ------------ */

#define NN_DWT_LAR_ADDR 0xE0001FB0u   /* DWT Lock Access Register */
#define NN_DWT_LAR_KEY  0xC5ACCE55u   /* CoreSight lock access key */

static int nn_dwt_enable(void)
{
	int attempt;

	if (DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk)
		return -1;                  /* CYCCNT not implemented */

	for (attempt = 0; attempt < 3; attempt++) {
		volatile uint32_t spin = 64u;
		uint32_t a;

		CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
		*((volatile uint32_t *)NN_DWT_LAR_ADDR) = NN_DWT_LAR_KEY; /* unlock (M7) */
		DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

		a = DWT->CYCCNT;
		while (spin--)
			__NOP();
		if (DWT->CYCCNT != a)
			return 0;           /* counting */
	}
	return -1;
}

/* ---- public API ------------------------------------------------------------ */

int nn_init(void)
{
	int rc;

	if (g_inited)
		return 0;

	/* DWT is best-effort: if it fails to run (e.g. NOCYCCNT), timing reads 0
	 * but inference still works, so it is not fatal to init. */
	(void)nn_dwt_enable();

	rc = nn_backend_vt_selected.init();
	if (rc != 0)
		return rc;

	g_inited = 1;
	return 0;
}

const struct nn_backend_info *nn_backend(void)
{
	return nn_backend_vt_selected.info;
}

static volatile int g_opening;      /* one thread owns the singleton open/init */

int nn_model_open(struct nn_model **out)
{
	int rc;

	if (!out)
		return -1;

	/* Serialize the one-time singleton open/init across concurrent shells: claim
	 * an init latch under a brief PRIMASK critical section so exactly one thread
	 * runs backend init/open; any other waits for g_model.open.  No pre-created
	 * mutex (avoids an init race) and the already-open path stays lock-free. */
	for (;;) {
		uint32_t primask = __get_PRIMASK();
		int owner = 0;

		__disable_irq();
		if (g_model.open) {
			__set_PRIMASK(primask);
			*out = &g_model;
			return 0;
		}
		if (!g_opening) { g_opening = 1; owner = 1; }
		__set_PRIMASK(primask);

		if (owner)
			break;
		tx_thread_sleep(1);         /* another thread is opening; wait + retry */
	}

	/* This thread owns the init. */
	rc = nn_init();
	if (rc == 0)
		rc = nn_backend_vt_selected.open(&g_model.impl);
	if (rc == 0) {
		g_model.last_cycles = 0;
		g_model.open = 1;           /* publish (waiters spin on this) */
	}
	g_opening = 0;

	if (rc != 0)
		return rc;
	*out = &g_model;
	return 0;
}

void nn_model_close(struct nn_model *m)
{
	if (!m || !m->open)
		return;
	nn_backend_vt_selected.close(m->impl);
	m->impl = NULL;
	m->open = 0;
	m->last_cycles = 0;
}

const char *nn_model_name(const struct nn_model *m)
{
	if (!m || !m->open)
		return "(none)";
	return nn_backend_vt_selected.model_name(m->impl);
}

int nn_input_count(const struct nn_model *m)
{
	return (m && m->open) ? nn_backend_vt_selected.in_count(m->impl) : 0;
}

int nn_output_count(const struct nn_model *m)
{
	return (m && m->open) ? nn_backend_vt_selected.out_count(m->impl) : 0;
}

struct nn_tensor *nn_input(struct nn_model *m, int idx)
{
	return (m && m->open) ? nn_backend_vt_selected.input(m->impl, idx) : NULL;
}

struct nn_tensor *nn_output(struct nn_model *m, int idx)
{
	return (m && m->open) ? nn_backend_vt_selected.output(m->impl, idx) : NULL;
}

uint32_t nn_activations_bytes(const struct nn_model *m)
{
	return (m && m->open) ? nn_backend_vt_selected.activations_bytes(m->impl) : 0u;
}

int nn_run(struct nn_model *m)
{
	uint32_t c0, c1;
	int rc;

	if (!m || !m->open)
		return -1;

	__DSB();
	__ISB();
	c0 = DWT->CYCCNT;

	rc = nn_backend_vt_selected.run(m->impl);

	__DSB();
	__ISB();
	c1 = DWT->CYCCNT;

	if (rc == 0)
		m->last_cycles = c1 - c0;   /* wraps naturally at 2^32 */
	return rc;
}

uint32_t nn_last_cycles(const struct nn_model *m)
{
	return (m && m->open) ? m->last_cycles : 0u;
}

/* ---- single-session guard -------------------------------------------------- */

/* A plain flag test-set under a brief PRIMASK critical section: interrupt-safe,
 * thread-agnostic (acquire and release may run on different threads), and needs
 * no one-time init (avoids an init race between concurrent shells). */
static volatile int nn_session_busy;

int nn_session_try_acquire(void)
{
	uint32_t primask = __get_PRIMASK();
	int ok;

	__disable_irq();
	if (!nn_session_busy) { nn_session_busy = 1; ok = 1; } else { ok = 0; }
	__set_PRIMASK(primask);
	return ok ? 0 : -1;
}

void nn_session_release(void)
{
	uint32_t primask = __get_PRIMASK();

	__disable_irq();
	nn_session_busy = 0;
	__set_PRIMASK(primask);
}
