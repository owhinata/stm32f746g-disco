/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cli_core.c
 * @brief   Shell ThreadX glue: instance lifecycle, thread loop, ISR notify.
 *
 * This is the only shell core file that calls ThreadX (tx_*) APIs; the line
 * editing / dispatch logic it drives lives in cli_session.c and stays
 * ThreadX-free for host unit testing.  Per instance it owns one tx_thread, one
 * tx_event_flags group (RX / TX / KILL) and one tx_mutex.  The thread blocks on
 * the event flags, drains the transport on an RX signal and feeds each byte to
 * the state machine.  No mutable global state, so several instances run
 * concurrently and independently (requirements §10).
 *
 * Clean-room design inspired by Zephyr shell's thread model; no code reused.
 */
#include <stddef.h>

#include "cli_instance.h"
#include "cli_internal.h"

/*
 * Instance thread.  Enable the backend, show the prompt, then loop: wait for an
 * RX (or KILL) signal, drain every available byte and run the state machine.
 * The mutex is created for #5's locked output path and is intentionally not
 * taken here -- in #4 all output comes from this one thread.
 */
static void cli_thread_entry(ULONG arg)
{
	struct cli_instance  *sh = (struct cli_instance *)arg;
	struct cli_transport *tr = sh->tr;
	ULONG flags;

	/* If the backend cannot start RX, do not spin pretending to be live: let
	 * the thread fall through and exit, leaving the rest of the system running
	 * (req §9).  Other instances are unaffected. */
	if (tr->api->enable(tr) != 0) {
		if (tr->api->uninit)
			tr->api->uninit(tr);
		return;
	}
	cli_prompt(sh);

	for (;;) {
		if (tx_event_flags_get(&sh->events, CLI_EVT_RX | CLI_EVT_KILL,
		                       TX_OR_CLEAR, &flags, TX_WAIT_FOREVER)
		    != TX_SUCCESS)
			continue;

		if (flags & CLI_EVT_KILL)
			break;                  /* full stop/uninit lifecycle is future (§14) */

		uint8_t buf[CLI_RX_DRAIN_CHUNK];
		int n;
		while ((n = tr->api->read(tr, buf, sizeof buf)) > 0) {
			for (int i = 0; i < n; i++)
				cli_input_byte(sh, buf[i]);
		}
	}

	if (tr->api->uninit)
		tr->api->uninit(tr);
}

int cli_init(struct cli_instance *sh)
{
	struct cli_transport *tr = sh->tr;

	/* One-shot: cli_init runs once per instance (a failed init is terminal for
	 * that instance and must not be retried -- see the create-order note below). */
	if (sh->state != CLI_UNINIT)
		return -1;

	/* Mandatory transport ops must be present. */
	if (tr == NULL || tr->api == NULL ||
	    tr->api->init == NULL || tr->api->enable == NULL ||
	    tr->api->write == NULL || tr->api->read == NULL) {
		sh->state = CLI_UNINIT;
		return -1;
	}

	tr->sh          = sh;
	sh->len         = 0;
	sh->line[0]     = '\0';
	sh->rx          = CLI_RX_NORMAL;
	sh->prev_cr     = 0;
	sh->last_result = 0;
	sh->rx_dropped  = 0;

	/*
	 * Bring up the backend BEFORE creating any ThreadX object.  The common
	 * failure (backend init) then needs no ThreadX teardown -- which matters
	 * because cli_init runs inside tx_application_define(), where the public
	 * tx_*_delete services return TX_CALLER_ERROR (system state != 0).  On the
	 * rarer event-flags/mutex create failures we therefore do NOT delete; the
	 * half-created object is harmless because the instance is never started.
	 */
	if (tr->api->init(tr) != 0) {
		sh->state = CLI_UNINIT;
		return -1;
	}

	if (tx_event_flags_create(&sh->events, "cli_evt") != TX_SUCCESS) {
		if (tr->api->uninit)
			tr->api->uninit(tr);
		sh->state = CLI_UNINIT;
		return -1;
	}

	/* TX_INHERIT: priority inheritance so a low-priority shell holding the TX
	 * lock (added in #5) cannot be preempted indefinitely by a mid thread. */
	if (tx_mutex_create(&sh->tx_lock, "cli_tx", TX_INHERIT) != TX_SUCCESS) {
		if (tr->api->uninit)
			tr->api->uninit(tr);
		sh->state = CLI_UNINIT;
		return -1;
	}

	sh->state = CLI_INITED;
	return 0;
}

int cli_start(struct cli_instance *sh)
{
	if (sh->state != CLI_INITED)
		return -1;

	if (tx_thread_create(&sh->thread, "cli", cli_thread_entry, (ULONG)sh,
	                     sh->stack, sh->stack_size,
	                     CLI_INSTANCE_PRIORITY, CLI_INSTANCE_PRIORITY,
	                     TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
		return -1;

	sh->state = CLI_STARTED;
	return 0;
}

void cli_transport_notify_rx(struct cli_instance *sh)
{
	/* ISR-safe: only sets an event flag (no lock, no suspend). */
	tx_event_flags_set(&sh->events, CLI_EVT_RX, TX_OR);
}

void cli_transport_notify_tx(struct cli_instance *sh)
{
	tx_event_flags_set(&sh->events, CLI_EVT_TX, TX_OR);
}
