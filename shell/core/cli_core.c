/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cli_core.c
 * @brief   Shell ThreadX glue: instance lifecycle, thread loop, ISR notify.
 *
 * This is the only shell core file that calls ThreadX (tx_*) APIs; the line
 * editing (cli_edit.c) and dispatch (cli_session.c) logic it drives stays
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

/* ---- thread -> instance registry (#18) ------------------------------------
 *
 * The backend's printf retarget (_write) asks cli_current_instance() which
 * shell instance owns the running thread, so printf output follows the calling
 * terminal instead of a single global console.  The table is tiny
 * (CLI_THREAD_MAP_MAX entries) and is mutated only inside a short
 * interrupt-disable critical section; the reader scans inside the same
 * critical section, so no volatile / memory barrier is needed today.  (If a
 * future caller ever scans without the lock -- e.g. lock-free fast path -- the
 * publish-.sh-last / retract-.sh-first ordering below must be paired with the
 * appropriate volatile/barriers.)
 */
static struct cli_thread_map {
	TX_THREAD           *thread;
	struct cli_instance *sh;
} cli_thread_reg[CLI_THREAD_MAP_MAX];

/*
 * True when running in exception / ISR context (IPSR != 0).  tx_thread_identify()
 * cannot tell us this: the cortex_m7/gnu port leaves _tx_thread_current_ptr
 * pointing at the *interrupted* thread across an ISR, so without this guard a
 * printf issued from an ISR that preempted a shell thread would be misattributed
 * to that thread's terminal.  Read IPSR with the same MRS the ThreadX port uses
 * so this lean (ThreadX-only, host-excluded) TU needs no CMSIS header.
 */
static inline unsigned int cli_in_isr(void)
{
	unsigned int ipsr;
	__asm__ volatile("MRS %0, IPSR" : "=r"(ipsr));
	return ipsr;
}

int cli_register_thread(TX_THREAD *t, struct cli_instance *sh)
{
	TX_INTERRUPT_SAVE_AREA
	int i, slot = -1;

	if (t == NULL || sh == NULL)
		return -1;

	TX_DISABLE
	for (i = 0; i < CLI_THREAD_MAP_MAX; i++) {
		if (cli_thread_reg[i].sh == NULL) {
			slot = i;
			break;
		}
	}
	if (slot >= 0) {
		cli_thread_reg[slot].thread = t;
		cli_thread_reg[slot].sh     = sh;   /* publish last */
	}
	TX_RESTORE

	return slot >= 0 ? 0 : -1;   /* -1: table full -- caller must not continue */
}

void cli_unregister_thread(TX_THREAD *t)
{
	TX_INTERRUPT_SAVE_AREA
	int i;

	if (t == NULL)
		return;

	TX_DISABLE
	for (i = 0; i < CLI_THREAD_MAP_MAX; i++) {
		if (cli_thread_reg[i].thread == t) {
			cli_thread_reg[i].sh     = NULL;   /* retract first */
			cli_thread_reg[i].thread = NULL;
			break;
		}
	}
	TX_RESTORE
}

struct cli_instance *cli_current_instance(void)
{
	TX_INTERRUPT_SAVE_AREA
	struct cli_instance *result = NULL;
	TX_THREAD *t;
	int i;

	if (cli_in_isr())            /* ISR/exception: no owning terminal */
		return NULL;

	t = tx_thread_identify();    /* NULL before the scheduler starts */
	if (t == NULL)
		return NULL;

	TX_DISABLE
	for (i = 0; i < CLI_THREAD_MAP_MAX; i++) {
		if (cli_thread_reg[i].thread == t && cli_thread_reg[i].sh != NULL) {
			result = cli_thread_reg[i].sh;
			break;
		}
	}
	TX_RESTORE

	return result;
}

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
		cli_unregister_thread(&sh->thread);   /* #18: drop the thread->instance map */
		return;
	}
	cli_edit_session_start(sh);   /* probe terminal width + draw the first prompt */

	for (;;) {
		if (tx_event_flags_get(&sh->events, CLI_EVT_RX | CLI_EVT_KILL,
		                       TX_OR_CLEAR, &flags, TX_WAIT_FOREVER)
		    != TX_SUCCESS)
			continue;

		if (flags & CLI_EVT_KILL)
			break;                  /* full stop/uninit lifecycle is future (§14) */

		/* Read ONE byte at a time and feed it immediately.  A command line ends
		 * with '\r', which dispatches the handler synchronously from inside
		 * cli_input_byte(); bulk-reading the ring first would pull any following
		 * type-ahead (e.g. a Ctrl+C) out of the ring into a local buffer, hiding
		 * it from cli_cancel_poll() while the handler runs (issue #16).  Feeding
		 * one byte at a time keeps every not-yet-consumed byte in the ring, so a
		 * running command can still see a 0x03 that arrived right after its line. */
		uint8_t b;
		while (tr->api->read(tr, &b, 1) > 0)
			cli_input_byte(sh, b);
	}

	if (tr->api->uninit)
		tr->api->uninit(tr);
	cli_unregister_thread(&sh->thread);   /* #18: drop the thread->instance map */
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
	sh->cur         = 0;
	sh->line[0]     = '\0';
	sh->rx          = CLI_RX_NORMAL;
	sh->prev_cr     = 0;
	sh->esc_np      = 0;
	sh->esc_bad     = 0;
	sh->esc_p[0]    = 0;
	sh->esc_p[1]    = 0;
	sh->overwrite   = 0;
	sh->hist_used   = 0;        /* command history ring empty (issue #10) */
	sh->hist_nav_on = 0;
	sh->hist_nav    = 0;
	sh->bs_swap     = CLI_BACKSPACE_MODE;
	sh->term_width  = CLI_TERM_WIDTH;
	sh->old_rows    = 0;
	sh->draw_row    = 0;
	sh->probing_cpr = 0;
	sh->tab_list_armed = 0;     /* Tab completion two-stage flag (issue #11) */
	sh->last_result = 0;
	sh->rx_dropped  = 0;
	sh->out_len     = 0;
	sh->tx_failed   = 0;
	sh->tx_dropped  = 0;

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

	/* Register the thread->instance mapping BEFORE creating the auto-started
	 * thread (#18): &sh->thread is a stable member address valid before
	 * tx_thread_create(), so a thread that begins running immediately always
	 * finds itself registered (no register-after-start race).  A full registry
	 * is a start failure -- printf must never silently misroute. */
	if (cli_register_thread(&sh->thread, sh) != 0)
		return -1;

	if (tx_thread_create(&sh->thread, "cli", cli_thread_entry, (ULONG)sh,
	                     sh->stack, sh->stack_size,
	                     CLI_INSTANCE_PRIORITY, CLI_INSTANCE_PRIORITY,
	                     TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS) {
		cli_unregister_thread(&sh->thread);   /* roll back the registration */
		return -1;
	}

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

/* Map a cli_config tick value to a ThreadX wait option (0 == wait forever). */
static ULONG cli_wait(unsigned ticks)
{
	return ticks == 0u ? TX_WAIT_FOREVER : (ULONG)ticks;
}

/*
 * Output lock (issue #5): the per-instance TX mutex guards a whole output call
 * (format + stage + flush) so concurrent writers to one instance never corrupt
 * out_buf/out_len.  TX_INHERIT (set in cli_init) bounds priority inversion while
 * the lock is held across a TX-space wait.
 */
int cli_lock(struct cli_instance *sh)
{
	return tx_mutex_get(&sh->tx_lock, cli_wait(CLI_TX_MUTEX_WAIT)) == TX_SUCCESS
	       ? 0 : -1;
}

void cli_unlock(struct cli_instance *sh)
{
	tx_mutex_put(&sh->tx_lock);
}

/*
 * Push len bytes to the transport, realising req §11's "blocking until sent"
 * semantics.  MUST be called with the output lock held (see cli_lock).  write()
 * is non-blocking and returns the count accepted; when the TX buffer is full we
 * suspend on CLI_EVT_TX (set by the backend when space frees) with a timeout.
 * On timeout the rest is dropped (drop stat + failure).  CLI_EVT_KILL aborts the
 * wait even with an infinite timeout, and is re-posted for the main loop.
 * Returns 0 if all bytes were sent, <0 on drop/error.
 */
int cli_tx_send_blocking(struct cli_instance *sh, const uint8_t *data, size_t len)
{
	struct cli_transport *tr = sh->tr;
	size_t sent = 0;

	while (sent < len) {
		/* Cooperative Ctrl+C (issue #16): once cancel is latched, stop emitting
		 * at once so a handler that keeps printing finishes fast.  Gated by
		 * dispatching so the post-cancel "^C"/prompt cleanup (dispatching == 0)
		 * is never suppressed. */
		if (sh->dispatching && sh->cancel_req)
			return -1;

		int n = tr->api->write(tr, data + sent, len - sent);
		if (n < 0) {
			sh->tx_failed = 1;
			return -1;
		}
		if (n > 0) {
			if ((size_t)n > len - sent)     /* defensive clamp */
				n = (int)(len - sent);
			sent += (size_t)n;
			continue;
		}

		/* TX full: wait for space (or a kill request).  While a command runs we
		 * also wake on RX so a Ctrl+C arriving mid-output aborts the blocked
		 * send.  The main loop's TX_OR_CLEAR may already have consumed the RX
		 * flag for a 0x03 still sitting in the ring, so poll the ring BEFORE
		 * waiting (issue #16 invariant 2). */
		ULONG flags;
		ULONG mask = CLI_EVT_TX | CLI_EVT_KILL |
		             (sh->dispatching ? CLI_EVT_RX : 0u);

		if (sh->dispatching && cli_cancel_poll(sh))
			return -1;

		if (tx_event_flags_get(&sh->events, mask, TX_OR_CLEAR, &flags,
		                       cli_wait(CLI_TX_TIMEOUT)) != TX_SUCCESS) {
			sh->tx_dropped += (uint32_t)(len - sent);   /* timeout */
			return -1;
		}
		if (flags & CLI_EVT_KILL) {
			tx_event_flags_set(&sh->events, CLI_EVT_KILL, TX_OR); /* re-post */
			sh->tx_dropped += (uint32_t)(len - sent);
			return -1;
		}
		if ((flags & CLI_EVT_RX) && cli_cancel_poll(sh))
			return -1;
		/* else CLI_EVT_TX (space freed) or a non-cancel RX byte: retry write */
	}
	return 0;
}

/*
 * Cancellable delay (issue #16): wait up to @p ticks ThreadX ticks, returning
 * early (non-zero) if Ctrl+C is seen or a stop is requested; 0 when the full
 * delay elapsed.  Unlike tx_thread_sleep() the wait is on the instance event
 * flags, so an RX byte (the ISR sets CLI_EVT_RX) wakes it and we drain the ring
 * for a 0x03.  Deadline-based with a wrap-safe elapsed so a non-cancel RX wake
 * neither shortens nor extends the delay.  Building block for watch/sleep (#21).
 */
int cli_sleep(struct cli_instance *sh, unsigned ticks)
{
	if (ticks == 0)
		return 0;                           /* contract: ticks==0 elapses at once */

	ULONG start = tx_time_get();

	for (;;) {
		ULONG elapsed, remaining, flags;
		UINT  st;

		if (cli_cancel_poll(sh))            /* a 0x03 already buffered in the ring */
			return 1;

		elapsed = (ULONG)(tx_time_get() - start);   /* wrap-safe */
		if (elapsed >= (ULONG)ticks)
			return 0;                       /* full delay elapsed */
		remaining = (ULONG)ticks - elapsed;

		st = tx_event_flags_get(&sh->events, CLI_EVT_RX | CLI_EVT_KILL,
		                        TX_OR_CLEAR, &flags, remaining);
		if (st != TX_SUCCESS)
			return 0;                       /* TX_NO_EVENTS (timed out) == elapsed */
		if (flags & CLI_EVT_KILL) {
			tx_event_flags_set(&sh->events, CLI_EVT_KILL, TX_OR); /* re-post */
			return 1;
		}
		if ((flags & CLI_EVT_RX) && cli_cancel_poll(sh))
			return 1;
		/* non-cancel RX: type-ahead drained, loop and re-wait the remainder */
	}
}
