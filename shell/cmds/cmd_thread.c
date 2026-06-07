/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_thread.c
 * @brief   `thread` built-in shell command (issue #13): one combined table of
 *          every ThreadX thread -- state / priority / run count + stack usage.
 *
 * Joins help/echo (cmd_builtin.c) and version/uptime/reboot (cmd_system.c) in the
 * `shell` executable only -- never linked into the host test harness.  It reads
 * board state through the standard buffered output API and touches only the shell
 * instance passed to it, so it stays reentrant across instances (req §10).
 *
 * Enumeration walks ThreadX's created-thread list (_tx_thread_created_ptr /
 * _tx_thread_created_count -- a circular doubly-linked list).  This firmware creates
 * every thread once in tx_application_define() and never deletes one, so the list is
 * static; we snapshot head+count under TX_DISABLE/TX_RESTORE, then walk it with
 * interrupts back on (cli_print waits on a mutex and must not run inside a critical
 * section).  Only those two internal globals are declared here; every per-thread
 * field we read lives in the public TX_THREAD typedef (tx_api.h), so the internal
 * tx_thread.h is not pulled in.
 *
 * Stack peak (high-water) usage is computed by scanning the 0xEF fill ThreadX lays
 * down at create time (tx_thread_create.c, TX_STACK_FILL == 0xEFEFEFEF -- present by
 * default unless TX_DISABLE_STACK_FILLING is defined, which this target does not).
 * This is the same best-effort method as ThreadX's own tx_thread_stack_analyze():
 * a used word can legitimately equal 0xEF, so peak may read a few bytes low.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"

#include "tx_api.h"          /* TX_THREAD, tx_thread_state defines, TX_DISABLE/RESTORE */

#include <stdint.h>

/* stack_peak_used() reads the 0xEF fill ThreadX applies by default.  If a build
 * ever turns that off, the stack columns would be meaningless -- fail loudly here
 * rather than silently print wrong numbers.  (Checked after tx_api.h so the port's
 * macro state is final; never fires for the shell target, which does not set it.) */
#ifdef TX_DISABLE_STACK_FILLING
# error "thread command needs the ThreadX stack fill; do not build the shell with TX_DISABLE_STACK_FILLING"
#endif

/* ThreadX created-thread list head + count (internal globals, declared in the
 * private tx_thread.h).  Declare just the two we need rather than including it. */
extern TX_THREAD *_tx_thread_created_ptr;
extern ULONG      _tx_thread_created_count;

/* tx_thread_state (tx_api.h §0..14) -> short label; index is the state value. */
static const char *state_name(UINT s)
{
	static const char *const names[] = {
		"ready",   /*  0 TX_READY          */
		"compl",   /*  1 TX_COMPLETED      */
		"term",    /*  2 TX_TERMINATED     */
		"susp",    /*  3 TX_SUSPENDED      */
		"sleep",   /*  4 TX_SLEEP          */
		"queue",   /*  5 TX_QUEUE_SUSP     */
		"sem",     /*  6 TX_SEMAPHORE_SUSP */
		"event",   /*  7 TX_EVENT_FLAG     */
		"block",   /*  8 TX_BLOCK_MEMORY   */
		"byte",    /*  9 TX_BYTE_MEMORY    */
		"io",      /* 10 TX_IO_DRIVER      */
		"file",    /* 11 TX_FILE           */
		"tcpip",   /* 12 TX_TCP_IP         */
		"mutex",   /* 13 TX_MUTEX_SUSP     */
		"pchg",    /* 14 TX_PRIORITY_CHANGE*/
	};
	return (s < (sizeof names / sizeof names[0])) ? names[s] : "?";
}

/*
 * Peak (high-water) stack usage in bytes.  The stack grows down from
 * tx_thread_stack_end (high) toward tx_thread_stack_start (low), so the untouched
 * tail keeps its 0xEF fill at the low end.  The leading 0xEF run from stack_start
 * is the free headroom; peak = size - free.  (Same scan as _tx_thread_stack_analyze.)
 */
static ULONG stack_peak_used(const TX_THREAD *t)
{
	const UCHAR *base = (const UCHAR *)t->tx_thread_stack_start;
	ULONG size  = t->tx_thread_stack_size;
	ULONG freeb = 0;

	while (freeb < size && base[freeb] == (UCHAR)0xEF)
		freeb++;
	return size - freeb;
}

static int cmd_thread(struct cli_instance *sh, int argc, char **argv)
{
	TX_INTERRUPT_SAVE_AREA

	TX_THREAD *t;
	ULONG count, i;

	(void)argc;
	(void)argv;

	/* Atomically snapshot the created-list head + count.  The nodes are static
	 * (nothing is ever deleted), so we walk the list afterwards with interrupts
	 * on -- cli_print waits on a mutex and must not run in a critical section. */
	TX_DISABLE
	t     = _tx_thread_created_ptr;
	count = _tx_thread_created_count;
	TX_RESTORE

	cli_print(sh, "%-20s %-6s %3s %6s %6s %5s %5s %4s\r\n",
	          "name", "state", "pri", "runs", "size", "peak", "free", "use%");

	if (t == TX_NULL || count == 0) {
		cli_print(sh, "(no threads)\r\n");
		return 0;
	}

	for (i = 0; i < count; i++, t = t->tx_thread_created_next) {
		ULONG size  = t->tx_thread_stack_size;
		ULONG peak  = stack_peak_used(t);
		ULONG freeb = size - peak;
		ULONG pct   = size ? (peak * 100u) / size : 0u;
		const char *name = t->tx_thread_name ? t->tx_thread_name : "(unnamed)";

		cli_print(sh, "%-20s %-6s %3u %6lu %6lu %5lu %5lu %3lu%%\r\n",
		          name, state_name(t->tx_thread_state),
		          t->tx_thread_priority,
		          (unsigned long)t->tx_thread_run_count,
		          (unsigned long)size, (unsigned long)peak,
		          (unsigned long)freeb, (unsigned long)pct);
	}
	return 0;
}

CLI_CMD_REGISTER(thread, NULL, "list threads + stack usage", cmd_thread, 1, 0);
