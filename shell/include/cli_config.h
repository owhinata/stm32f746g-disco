/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cli_config.h
 * @brief   Compile-time configuration knobs and resource limits for the Shell.
 *
 * Every knob has a default matching the requirements spec (§8) and may be
 * overridden at build time (e.g. -DCLI_CMD_BUFFER_SIZE=512) since each is wrapped
 * in #ifndef.  This task (#2) only *defines* the knobs; the over-limit runtime
 * behaviour noted next to each (BEL, FIFO discard, truncate, ...) is implemented
 * by the later issues referenced in the comments.
 *
 * All shell storage is statically allocated -- no heap is used at run time.
 */
#ifndef CLI_CONFIG_H
#define CLI_CONFIG_H

/* Command (line) input buffer length in bytes.
 * Over limit: ring the bell (BEL) and ignore further input chars (impl. #4). */
#ifndef CLI_CMD_BUFFER_SIZE
#define CLI_CMD_BUFFER_SIZE 256
#endif

/* Maximum argument count (argc), command name included.
 * Over limit: show an error and do not execute the command (impl. #3). */
#ifndef CLI_MAX_ARGC
#define CLI_MAX_ARGC 20
#endif

/* Command history ring size in bytes.
 * Over limit: discard the oldest entry first, FIFO (impl. #10). */
#ifndef CLI_HISTORY_BUFFER_SIZE
#define CLI_HISTORY_BUFFER_SIZE 512
#endif

/* printf/output staging buffer in bytes.
 * Over limit: flush when full (impl. #5, flow control §11). */
#ifndef CLI_PRINTF_BUFFER_SIZE
#define CLI_PRINTF_BUFFER_SIZE 32
#endif

/* Prompt buffer length in bytes.
 * Over limit: truncate (impl. #4/#9). */
#ifndef CLI_PROMPT_BUFFER_SIZE
#define CLI_PROMPT_BUFFER_SIZE 20
#endif

/* Per-instance ThreadX thread stack size in bytes (impl. #4). */
#ifndef CLI_INSTANCE_STACK_SIZE
#define CLI_INSTANCE_STACK_SIZE 2048
#endif

/* Maximum number of concurrent shell instances.
 * Fixed at compile time; exceeding it is a build-time error.  The actual count
 * check happens where instances are defined (#4/#6). */
#ifndef CLI_MAX_INSTANCES
#define CLI_MAX_INSTANCES 4
#endif

/* Maximum static subcommand tree nesting depth.
 * Over limit: show an error and do not execute the command (impl. #3). */
#ifndef CLI_MAX_SUBCMD_DEPTH
#define CLI_MAX_SUBCMD_DEPTH 8
#endif

/* ThreadX priority of each shell instance thread (impl. #4).  Plain integer
 * (no ThreadX symbols here, so cli_config.h stays ThreadX-independent and
 * host-includable).  Default 16: a development shell runs *below* the demo
 * application threads (priority 10 in src/app_threadx.c) so it never starves
 * real work; the <5 ms echo target (req §15) is paced by IRQ -> event flag ->
 * thread wake-up and is verified on target in issue #8. */
#ifndef CLI_INSTANCE_PRIORITY
#define CLI_INSTANCE_PRIORITY 16
#endif

/* Bytes drained from the transport per read() in the instance thread loop
 * (impl. #4).  Purely a batching size; does not bound input length. */
#ifndef CLI_RX_DRAIN_CHUNK
#define CLI_RX_DRAIN_CHUNK 32
#endif

/* TX flow-control timeout in ThreadX ticks (impl. #5, req §11): how long an
 * output blocks waiting for transport TX space before it gives up, drops the
 * rest, bumps the drop stat and fails the call.  0 == wait forever (never drop).
 * Default 1000 ~= 1 s on the usual 1 kHz ThreadX tick.  Plain integer (no
 * ThreadX symbol here); cli_core.c maps 0 -> TX_WAIT_FOREVER. */
#ifndef CLI_TX_TIMEOUT
#define CLI_TX_TIMEOUT 1000
#endif

/* TX/output mutex acquire timeout in ThreadX ticks (impl. #5).  0 == wait
 * forever; cli_core.c maps 0 -> TX_WAIT_FOREVER. */
#ifndef CLI_TX_MUTEX_WAIT
#define CLI_TX_MUTEX_WAIT 0
#endif

/* Colour output (impl. #5): 1 emits VT100 SGR for cli_error/warn/info, 0 emits
 * none (monochrome terminals / logs). */
#ifndef CLI_USE_COLOR
#define CLI_USE_COLOR 1
#endif

/*
 * The number of registered commands is bounded only by the linker section
 * capacity (effectively unlimited; the scan is linear).  Tab-completion does
 * not allocate -- it scans the command tree and prints inline, so there is no
 * "max completion candidates" knob.
 */

/* Sanity checks for the knobs that have meaning at this stage. */
_Static_assert(CLI_CMD_BUFFER_SIZE > 0,     "CLI_CMD_BUFFER_SIZE must be > 0");
_Static_assert(CLI_MAX_ARGC >= 1,           "CLI_MAX_ARGC must be >= 1");
_Static_assert(CLI_HISTORY_BUFFER_SIZE > 0, "CLI_HISTORY_BUFFER_SIZE must be > 0");
_Static_assert(CLI_PRINTF_BUFFER_SIZE > 0,  "CLI_PRINTF_BUFFER_SIZE must be > 0");
_Static_assert(CLI_PROMPT_BUFFER_SIZE > 0,  "CLI_PROMPT_BUFFER_SIZE must be > 0");
_Static_assert(CLI_INSTANCE_STACK_SIZE >= 512, "CLI_INSTANCE_STACK_SIZE too small");
_Static_assert(CLI_MAX_INSTANCES >= 1,      "CLI_MAX_INSTANCES must be >= 1");
_Static_assert(CLI_MAX_SUBCMD_DEPTH >= 1,   "CLI_MAX_SUBCMD_DEPTH must be >= 1");
_Static_assert(CLI_INSTANCE_PRIORITY <= 31, "CLI_INSTANCE_PRIORITY must be 0..31 (ThreadX)");
_Static_assert(CLI_RX_DRAIN_CHUNK >= 1,     "CLI_RX_DRAIN_CHUNK must be >= 1");

#endif /* CLI_CONFIG_H */
