/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cli_backend_uart.c
 * @brief   USART1 (VCP) interrupt-driven transport implementation (issue #7).
 *
 * Implements struct cli_transport_api over the STM32 HAL UART in IT mode plus the
 * USART1 ISR, the HAL Rx/Tx/Error callbacks and the printf (_write) retarget that
 * shares the single TX ring.  See cli_backend_uart.h for the threading model and
 * the PRIMASK rationale; cli_uart_ring.h for the (host-tested) ring helpers.
 *
 * Single console: this backend targets the VCP on USART1, so the file-global
 * @ref g_uart_console resolves the HAL callbacks (which carry only a
 * UART_HandleTypeDef*) back to the owning context, and _write routes through it.
 * A future multi-UART backend would switch on huart->Instance instead.
 */
#include "cli_backend_uart.h"
#include "cli_internal.h" /* cli_out_begin/cli_out_end: lock printf with cli_print (#25) */
#include "bsp.h"   /* extern huart1: the pre-enable polling fallback for _write */
#include "tx_api.h" /* execution-profile ISR hooks (auto-pulls tx_execution_profile.h) */

/* The one active UART console (set in init).  The HAL Rx/Tx/Error callbacks and
 * _write reach the context through this; NULL until the first cli_init(). */
static struct cli_uart *g_uart_console;

/* USART1 IRQ priority.  Above SysTick (14) for low echo latency / ORE headroom;
 * any value is ThreadX-safe under this port's PRIMASK critical sections. */
#define CLI_UART_IRQ_PRIORITY 5u

/* Bounded spin for the printf path when the TX ring is momentarily full: the TX
 * ISR (priority 5) drains in the background between iterations, so this resolves
 * quickly; the cap only stops a wedged TX from hanging the caller (then drops). */
#define CLI_UART_WRITE_SPIN_MAX 1000000u

/* PRIMASK critical section.  Nests safely inside ThreadX's own PRIMASK section. */
#define CLI_UART_CRIT_ENTER()  do { uint32_t _pm = __get_PRIMASK(); __disable_irq()
#define CLI_UART_CRIT_EXIT()   __set_PRIMASK(_pm); } while (0)

/*
 * Start a HAL_UART_Transmit_IT of the next contiguous run if none is in flight.
 * MUST be called with the critical section held.  Only commits tx_in_flight /
 * tx_chunk on HAL_OK; on HAL_BUSY/HAL_ERROR the bytes stay queued and the next
 * write()/TxComplete retries (defends against a transient gState clash with the
 * polling _write fallback).  Used by write(), _write and the TxComplete callback
 * so the HAL return is handled in exactly one place.
 */
static void tx_start_locked(struct cli_uart *u)
{
	if (u->tx_in_flight)
		return;

	const uint8_t *p;
	size_t run = cli_uart_ring_contig(&u->tx_ring, &p);
	if (run == 0u)
		return;                 /* nothing to send */

	/* run <= CLI_UART_TX_BUFFER_SIZE-1, fits HAL's uint16_t Size. */
	if (HAL_UART_Transmit_IT(u->huart, p, (uint16_t)run) == HAL_OK) {
		u->tx_in_flight = 1;
		u->tx_chunk     = (uint16_t)run;
	}
}

/* ---- transport vtable -------------------------------------------------- */

static int uart_init(struct cli_transport *tr)
{
	struct cli_uart *u = (struct cli_uart *)tr->ctx;

	if (u == NULL || u->huart == NULL)
		return -1;

	cli_uart_ring_init(&u->rx_ring, u->rx_buf, sizeof u->rx_buf);
	cli_uart_ring_init(&u->tx_ring, u->tx_buf, sizeof u->tx_buf);
	u->rx_byte       = 0;
	u->tx_in_flight  = 0;
	u->tx_chunk      = 0;
	u->enabled       = 0;
	u->rx_overrun    = 0;
	u->rx_rearm_fail = 0;
	u->sh            = tr->sh;      /* cli_init() set tr->sh before calling init */

	/* Become the console now so _write routes here; enabled stays 0 until
	 * enable() arms RX, so _write keeps using the polling fallback meanwhile. */
	g_uart_console = u;
	return 0;
}

static int uart_enable(struct cli_transport *tr)
{
	struct cli_uart *u = (struct cli_uart *)tr->ctx;

	HAL_NVIC_SetPriority(USART1_IRQn, CLI_UART_IRQ_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(USART1_IRQn);

	/* Arm the first 1-byte receive; bail (and let the core drop this thread,
	 * cli_core.c) if HAL is busy so we never pretend to be live (req §9). */
	if (HAL_UART_Receive_IT(u->huart, &u->rx_byte, 1) != HAL_OK) {
		HAL_NVIC_DisableIRQ(USART1_IRQn);
		return -1;
	}

	u->enabled = 1;
	return 0;
}

static int uart_write(struct cli_transport *tr, const uint8_t *data, size_t len)
{
	struct cli_uart *u = (struct cli_uart *)tr->ctx;
	size_t acc;

	/* Non-blocking (req §11): enqueue what fits, kick TX, return the count.  A
	 * short/zero return makes the core block on CLI_EVT_TX; TxComplete frees
	 * space and fires cli_transport_notify_tx() to wake it. */
	CLI_UART_CRIT_ENTER();
	acc = cli_uart_ring_put_buf(&u->tx_ring, data, len);
	tx_start_locked(u);
	CLI_UART_CRIT_EXIT();

	return (int)acc;
}

static int uart_read(struct cli_transport *tr, uint8_t *data, size_t cap)
{
	struct cli_uart *u = (struct cli_uart *)tr->ctx;

	/* SPSC: the USART1 ISR is the only producer, this (the shell thread) the
	 * only consumer, so draining the ring needs no lock. */
	return (int)cli_uart_ring_get_buf(&u->rx_ring, data, cap);
}

static void uart_uninit(struct cli_transport *tr)
{
	struct cli_uart *u = (struct cli_uart *)tr->ctx;

	/* Blocking abort stops TX+RX and returns with gState/RxState == READY, so
	 * clearing `enabled` afterwards leaves no "disabled but IT TX in flight"
	 * window for the polling _write fallback to clash on huart1.gState.
	 * NOTE (future KILL/uninit lifecycle, §14): `enabled` is still 1 across the
	 * abort, so a concurrent _write on another thread could enqueue to the ring
	 * mid-teardown.  Harmless in #7 (uninit never runs at steady state); when the
	 * lifecycle is real, clear `enabled` (and quiesce writers) before aborting. */
	HAL_UART_Abort(u->huart);
	u->enabled      = 0;
	u->tx_in_flight = 0;
	u->tx_chunk     = 0;
	HAL_NVIC_DisableIRQ(USART1_IRQn);
}

const struct cli_transport_api cli_uart_api = {
	uart_init, uart_enable, uart_write, uart_read, uart_uninit, NULL,
};

/* ---- HAL callbacks + ISR (resolved to the console via g_uart_console) --- */

void USART1_IRQHandler(void)
{
#if defined(TX_EXECUTION_PROFILE_ENABLE)
	/* Charge this ISR to the execution profile (issue #19).  No profile_active
	 * gate is needed: USART1's IRQ is only enabled from uart_enable(), which runs
	 * in the shell thread -- i.e. after the scheduler and _tx_execution_initialize().
	 * PRIMASK-protect the enter/exit so the kit's nest counter + 64-bit totals stay
	 * atomic if SysTick (priority 14) is preempted... actually USART1 (priority 5)
	 * is the preemptor, so this guards the reverse case symmetrically. */
	CLI_UART_CRIT_ENTER(); _tx_execution_isr_enter(); CLI_UART_CRIT_EXIT();
#endif

	struct cli_uart *u = g_uart_console;

	if (u != NULL && u->huart != NULL)
		HAL_UART_IRQHandler(u->huart);   /* dispatches to the callbacks below */

#if defined(TX_EXECUTION_PROFILE_ENABLE)
	CLI_UART_CRIT_ENTER(); _tx_execution_isr_exit(); CLI_UART_CRIT_EXIT();
#endif
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
	struct cli_uart *u = g_uart_console;

	if (u == NULL || huart != u->huart)
		return;

	/* Store the byte; on ring overflow drop it and count (req §9/§15, as dummy). */
	if (!cli_uart_ring_put(&u->rx_ring, u->rx_byte)) {
		if (u->sh != NULL)
			u->sh->rx_dropped++;
	}

	/* Re-arm the next 1-byte receive; surface a silent RX stall if HAL refuses. */
	if (HAL_UART_Receive_IT(huart, &u->rx_byte, 1) != HAL_OK)
		u->rx_rearm_fail++;

	/* ISR-safe: only sets an event flag (req §10). */
	if (u->sh != NULL)
		cli_transport_notify_rx(u->sh);
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
	struct cli_uart *u = g_uart_console;

	if (u == NULL || huart != u->huart)
		return;

	CLI_UART_CRIT_ENTER();
	cli_uart_ring_advance_tail(&u->tx_ring, u->tx_chunk);
	u->tx_chunk     = 0;
	u->tx_in_flight = 0;
	tx_start_locked(u);             /* start the next chunk if any (one code path) */
	CLI_UART_CRIT_EXIT();

	/* Space just freed: wake the core if it was blocked on TX (req §11). */
	if (u->sh != NULL)
		cli_transport_notify_tx(u->sh);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
	struct cli_uart *u = g_uart_console;

	if (u == NULL || huart != u->huart)
		return;

	if (HAL_UART_GetError(huart) & HAL_UART_ERROR_ORE)
		u->rx_overrun++;            /* HW overrun: bytes lost before the ring */

	/* ORE/RTO end the IT receive (RxState -> READY); re-arm.  FE/NE/PE leave RX
	 * running (RxState stays BUSY), so the READY check avoids a double-arm. */
	if (huart->RxState == HAL_UART_STATE_READY) {
		if (HAL_UART_Receive_IT(huart, &u->rx_byte, 1) != HAL_OK)
			u->rx_rearm_fail++;
	}
}

/* ---- printf / _write coexistence (single TX owner) --------------------- */

/*
 * Push one byte into the TX ring, spinning (bounded) until it fits while the TX
 * ISR drains in the background, and keep TX running.  Returns 1 on success, 0 if
 * TX stayed wedged past the spin cap.  *stall carries the idle-iteration count
 * and resets on progress.  Used by _write for byte-granular LF->CRLF translation.
 */
static int tx_putc_spin(struct cli_uart *u, uint8_t b, uint32_t *stall)
{
	for (;;) {
		int ok;
		CLI_UART_CRIT_ENTER();
		ok = cli_uart_ring_put(&u->tx_ring, b);
		tx_start_locked(u);
		CLI_UART_CRIT_EXIT();

		if (ok) {
			*stall = 0;
			return 1;
		}
		if (++*stall > CLI_UART_WRITE_SPIN_MAX)
			return 0;               /* TX wedged: give up (best-effort) */
	}
}

/*
 * Resolve a shell instance to its UART TX backend, but only when this backend
 * owns it (api identity) and the console is live (enabled).  Used by _write to
 * route printf to the *calling* instance's UART; a non-UART transport (e.g. the
 * dummy backend), an un-enabled instance or a NULL instance returns NULL so the
 * caller falls back to the global console (#18).
 */
static struct cli_uart *uart_ctx_from_instance(struct cli_instance *sh)
{
	struct cli_uart *u;

	if (sh == NULL || sh->tr == NULL || sh->tr->api != &cli_uart_api)
		return NULL;
	u = (struct cli_uart *)sh->tr->ctx;
	return (u != NULL && u->enabled) ? u : NULL;
}

/*
 * Strong _write that overrides the weak polling one in bsp.c when this backend is
 * linked.  Once a console is enabled, route printf through the same TX ring as
 * the shell so each USART has exactly one owner; before that (early boot logs, or
 * no console bound yet) fall back to blocking polling, which is safe because no
 * IT TX has been armed.  printf is best-effort: a wedged TX drops the remainder.
 *
 * Per-thread routing (#18): the target is the UART of the shell instance that
 * owns the running thread (cli_current_instance), so printf -- including the
 * CoreMark report (ee_printf == printf), which runs in the calling shell thread
 * -- follows the calling terminal.  From an ISR, before the scheduler starts
 * (boot banner) or from a non-shell thread, cli_current_instance() returns NULL
 * and we fall back to g_uart_console, i.e. exactly the previous behaviour (the
 * single VCP is unchanged byte-for-byte).
 *
 * Bare LF is translated to CR+LF so a raw terminal (without picocom's --imap
 * lfcrlf) shows printf output -- notably the CoreMark report, whose lines end in
 * '\n' -- without staircasing; an existing CR before the LF is left as-is (no
 * double CR).  Only C-library printf flows through _write: the shell's own
 * cli_print emits CRLF through the transport write() path and is unaffected.
 * Line-atomicity (issue #25): when the running thread is a registered shell/job
 * thread on THIS UART (uart_ctx_from_instance() != NULL), the byte drain is
 * bracketed by cli_out_begin/cli_out_end -- the same output lock cli_print and
 * the line editor take (the FOREGROUND's lock for a bg-job worker) -- so a bg
 * printf (e.g. CoreMark in the background) is serialised with cli_print and the
 * editor and inherits the bg line-break.  From an ISR, before the scheduler, or
 * from a non-shell / non-UART thread the instance resolves to NULL, no lock is
 * taken and we fall back to g_uart_console exactly as before (byte-for-byte).
 * The TX ring itself is always intact regardless (PRIMASK-guarded dual producer).
 */
int _write(int file, char *ptr, int len)
{
	struct cli_instance *sh = cli_current_instance();
	struct cli_uart     *u  = uart_ctx_from_instance(sh);
	int locked = 0;
	(void)file;

	if (len <= 0)
		return len;

	/* During a raw binary transfer (issue #50) this UART is owned by the YMODEM
	 * byte stream; drop printf output -- it would otherwise reach g_uart_console
	 * unlocked (from a non-shell thread / ISR) and corrupt the transfer.  The
	 * transfer's own bytes go via the transport write() path, not _write. */
	if (cli_xfer_active)
		return len;

	if (u == NULL)
		u = g_uart_console;   /* ISR / pre-kernel / non-shell thread / non-UART */
	else
		locked = (cli_out_begin(sh) == 0);   /* serialise with cli_print + editor (#25) */

	if (u != NULL && u->enabled) {
		const uint8_t *d = (const uint8_t *)ptr;
		uint32_t stall = 0;
		uint8_t  prev = 0;
		int i;

		for (i = 0; i < len; i++) {
			uint8_t b = (uint8_t)d[i];

			if (b == (uint8_t)'\n' && prev != (uint8_t)'\r') {
				if (!tx_putc_spin(u, (uint8_t)'\r', &stall))
					break;          /* wedged: drop the rest */
			}
			if (!tx_putc_spin(u, b, &stall))
				break;                  /* wedged: drop the rest */
			prev = b;
		}
		if (locked)
			cli_out_end(sh);
		return len;
	}

	/* Pre-enable path: poll the bound handle, or the VCP huart1 if no console
	 * has been bound yet (the earliest boot printf, before cli_init()).  (Only
	 * reached with locked == 0: a resolved enabled instance takes the branch
	 * above.) */
	{
		UART_HandleTypeDef *h = (u != NULL && u->huart != NULL) ? u->huart
		                                                        : &huart1;
		HAL_UART_Transmit(h, (uint8_t *)ptr, (uint16_t)len, HAL_MAX_DELAY);
	}
	if (locked)
		cli_out_end(sh);
	return len;
}
