/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host-side ThreadX-free glue shared by the Shell host tests.  See host_glue.h.
 *
 * cli_tx_send_blocking() mirrors cli_core.c:cli_tx_send_blocking() so the host
 * tests exercise the real §11 flow-control semantics through tr->api->write()
 * rather than a capture stub:
 *   - partial accept (0<n<len)   -> keep sending the remainder until complete
 *   - full (n==0)                -> invoke the TX-wait hook (notify_tx analogue);
 *                                   repeated no-progress waits == timeout: drop
 *                                   the remainder, bump tx_dropped, return <0
 *   - error (n<0)                -> set tx_failed, return <0
 *   - n>remaining                -> defensive clamp
 * The first three behaviours are pinned by test_integration.c so that if this
 * host model drifts from cli_core.c the tests fail; the clamp mirrors cli_core.c
 * defensively and is not exercised (a well-behaved backend never over-accepts).
 */
#include <stddef.h>
#include <stdint.h>

#include "cli_instance.h"
#include "cli_internal.h"
#include "host_glue.h"

/* How many consecutive no-progress TX waits count as a timeout.  With a hook
 * that frees space, progress resets the counter so a send never times out; with
 * no hook the send drops after this many waits (deterministic, no spin). */
#ifndef CLI_TEST_TX_MAX_STALLS
#define CLI_TEST_TX_MAX_STALLS 2
#endif

/* ---- output lock: no-op on the single-threaded host --------------------- */

int  cli_lock(struct cli_instance *sh)   { (void)sh; return 0; }
void cli_unlock(struct cli_instance *sh) { (void)sh; }

/* ---- backend notify: no-op (RX is pumped synchronously) ----------------- *
 * The real notify_tx contract ("backend calls this when TX space frees") is NOT
 * verified on the host -- it is exercised on target by the UART backend (#7). */
void cli_transport_notify_rx(struct cli_instance *sh) { (void)sh; }
void cli_transport_notify_tx(struct cli_instance *sh) { (void)sh; }

/* ---- TX-wait hook ------------------------------------------------------- */

static cli_test_tx_wait_fn g_tx_wait_fn;
static void               *g_tx_wait_arg;

void cli_test_set_tx_wait_hook(cli_test_tx_wait_fn fn, void *arg)
{
	g_tx_wait_fn  = fn;
	g_tx_wait_arg = arg;
}

/* ---- flow-controlled send (mirrors cli_core.c) -------------------------- */

int cli_tx_send_blocking(struct cli_instance *sh, const uint8_t *data, size_t len)
{
	struct cli_transport *tr = sh->tr;
	size_t sent = 0;
	int    stalls = 0;

	while (sent < len) {
		int n = tr->api->write(tr, data + sent, len - sent);
		if (n < 0) {
			sh->tx_failed = 1;
			return -1;
		}
		if (n > 0) {
			if ((size_t)n > len - sent)         /* defensive clamp */
				n = (int)(len - sent);
			sent += (size_t)n;
			stalls = 0;
			continue;
		}

		/* TX full: wait for space (real core suspends on CLI_EVT_TX). */
		if (g_tx_wait_fn)
			g_tx_wait_fn(sh, g_tx_wait_arg);
		if (++stalls >= CLI_TEST_TX_MAX_STALLS) {
			sh->tx_dropped += (uint32_t)(len - sent);   /* timeout drop */
			return -1;
		}
	}
	return 0;
}

/* ---- RX pump (mirrors cli_core.c's thread loop) ------------------------- */

void cli_test_pump(struct cli_instance *sh)
{
	struct cli_transport *tr = sh->tr;
	uint8_t buf[CLI_RX_DRAIN_CHUNK];
	int n;

	while ((n = tr->api->read(tr, buf, sizeof buf)) > 0)
		for (int i = 0; i < n; i++)
			cli_input_byte(sh, buf[i]);
}
