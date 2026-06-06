/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host integration test for the Shell core (issue #6): input -> execute ->
 * output verified END-TO-END THROUGH THE DUMMY BACKEND.  Unlike test_core.c
 * (which injects at the session level and stubs the transport), this drives the
 * real transport contract: cli_dummy_inject() fills the backend RX FIFO,
 * cli_test_pump() drains it through read() into the line state machine, and
 * every byte of output reaches the dummy capture log through tr->api->write()
 * via the faithful host cli_tx_send_blocking() (host_glue.c).
 *
 * Coverage at this milestone (implemented surface #2-#5):
 *   - basic dispatch + subcommands (handler ran, output captured, prompt back)
 *   - §13 ASCII filter, CR/LF coalesce, ESC/CSI swallow -- via the backend
 *   - §11 flow control: normal backpressure completes, timeout drops, immediate
 *     write() failure, and dispatch result promotion on TX failure
 *   - §9/§18 abnormal: unknown cmd, arg errors, line overflow (BEL), Ctrl+C,
 *     RX-ring overflow drop + stat
 *   - §10 multi-instance: two dummy instances interleaved, no output crosstalk
 *
 * Built with small CLI_* limits (see run_host_tests.sh) and colour OFF so the
 * overflow / too-many-tokens paths fit compact input and output compares plainly.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cli_instance.h"
#include "cli_internal.h"
#include "cli_backend_dummy.h"
#include "host_glue.h"

/* ---- test commands ----------------------------------------------------- */

static int ran;

static int h_ok(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc; (void)argv;
	ran++;
	cli_print(sh, "OK\r\n");
	return 0;
}
static int h_args(struct cli_instance *sh, int argc, char **argv)
{
	cli_print(sh, "argc=%d a1=%s\r\n", argc, argc > 1 ? argv[1] : "-");
	return 0;
}

CLI_SUBCMD_SET_CREATE(sub_thing, CLI_CMD_ARG(list, NULL, "list", h_ok, 1, 0),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(hello, NULL,      "say hi",       h_ok,   1, 0);
CLI_CMD_REGISTER(echo2, NULL,      "echo arg",     h_args, 1, 1);
CLI_CMD_REGISTER(thing, sub_thing, "parent only",  NULL,   1, 0);
CLI_CMD_REGISTER(need2, NULL,      "needs 2 args", h_args, 2, 0);

/* ---- harness ----------------------------------------------------------- */

CLI_BACKEND_DUMMY_DEFINE(tr0);
CLI_BACKEND_DUMMY_DEFINE(tr1);
static struct cli_instance sh0;
static struct cli_instance sh1;

static void reset(struct cli_instance *s, struct cli_transport *t)
{
	memset(s, 0, sizeof *s);
	s->tr = t;
	t->sh = s;
	strcpy(s->prompt, "> ");
	cli_dummy_clear_output(t);
	cli_dummy_clear_rx(t);
	cli_dummy_reset_stats(t);
	cli_dummy_set_tx_fail(t, 0);
	cli_dummy_set_tx_cap(t, 0);          /* unlimited */
	cli_test_set_tx_wait_hook(NULL, NULL);
	ran = 0;
}

/* Inject a line as if received by the backend, then drain it through read(). */
static void run_line(struct cli_instance *s, const char *line)
{
	cli_dummy_inject(s->tr, line, strlen(line));
	cli_test_pump(s);
}

static void inject_bytes(struct cli_instance *s, const uint8_t *b, size_t n)
{
	cli_dummy_inject(s->tr, b, n);
	cli_test_pump(s);
}

static const char *out_of(struct cli_transport *t) { return cli_dummy_output_str(t); }
static int has(struct cli_transport *t, const char *needle)
{
	return strstr(out_of(t), needle) != NULL;
}
static int count(struct cli_transport *t, const char *needle)
{
	int n = 0;
	size_t nl = strlen(needle);
	for (const char *p = out_of(t); (p = strstr(p, needle)) != NULL; p += nl)
		n++;
	return n;
}

/* ---- basic dispatch through the backend -------------------------------- */

static void test_basic_dispatch(void)
{
	reset(&sh0, &tr0);
	run_line(&sh0, "hello\r");
	assert(ran == 1);
	assert(has(&tr0, "OK"));
	assert(has(&tr0, "hello"));            /* input was echoed back */
	assert(sh0.last_result == 0);
	assert(sh0.len == 0);                  /* line reset after dispatch */

	size_t n;
	const char *o = cli_dummy_output(&tr0, &n);
	assert(n >= 2 && strcmp(o + n - 2, "> ") == 0);   /* prompt reappears */

	/* Subcommand: leaf handler runs. */
	reset(&sh0, &tr0);
	run_line(&sh0, "thing list\r");
	assert(ran == 1 && has(&tr0, "OK"));

	/* Optional arg reaches the handler. */
	reset(&sh0, &tr0);
	run_line(&sh0, "echo2 foo\r");
	assert(has(&tr0, "argc=2 a1=foo"));
}

/* ---- §13 ASCII filter / coalesce / swallow, all via the backend -------- */

static void test_filter_and_editing_via_backend(void)
{
	/* §13: high-bit bytes never reach the line. */
	reset(&sh0, &tr0);
	const uint8_t hi[] = { 'a', 'b', 0xC3, 0x80, 'c' };   /* no newline */
	inject_bytes(&sh0, hi, sizeof hi);
	assert(sh0.len == 3 && strcmp(sh0.line, "abc") == 0);

	/* CR-LF coalesces into exactly one dispatch. */
	reset(&sh0, &tr0);
	run_line(&sh0, "hello\r\n");
	assert(count(&tr0, "OK") == 1);

	/* ESC/CSI (right-arrow) is swallowed, never entering the line. */
	reset(&sh0, &tr0);
	const uint8_t esc[] = { 'a', 0x1B, '[', 'C', 'b' };   /* no newline */
	inject_bytes(&sh0, esc, sizeof esc);
	assert(sh0.len == 2 && strcmp(sh0.line, "ab") == 0);
}

/* ---- §11 flow control -------------------------------------------------- */

/* TX-wait hook that frees a little capacity each time TX reports full -- the
 * host analogue of a backend firing cli_transport_notify_tx() as it drains. */
static void free_some_hook(struct cli_instance *s, void *arg)
{
	(void)arg;
	cli_dummy_free_tx(s->tr, 8);
}

static void test_backpressure_completes(void)
{
	reset(&sh0, &tr0);
	cli_dummy_set_tx_cap(&tr0, 4);                 /* full after 4 bytes */
	cli_test_set_tx_wait_hook(free_some_hook, NULL);

	int r = cli_print(&sh0, "0123456789");         /* 10 B > capacity */
	assert(r == 0);                                /* completed, not dropped */
	assert(sh0.tx_failed == 0 && sh0.tx_dropped == 0);

	size_t n;
	const char *o = cli_dummy_output(&tr0, &n);
	assert(n == 10 && memcmp(o, "0123456789", 10) == 0);   /* in order */
}

static void test_tx_timeout_drops(void)
{
	reset(&sh0, &tr0);
	cli_dummy_set_tx_cap(&tr0, 4);                 /* 4 B then full */
	cli_test_set_tx_wait_hook(NULL, NULL);         /* space never freed */

	int r = cli_print(&sh0, "0123456789");         /* 10 B */
	assert(r < 0);
	assert(sh0.tx_failed == 1);
	assert(sh0.tx_dropped == 6);                   /* 10 - 4 accepted */

	size_t n;
	cli_dummy_output(&tr0, &n);
	assert(n == 4);                                /* only the accepted prefix */
}

static void test_tx_immediate_fail(void)
{
	reset(&sh0, &tr0);
	cli_dummy_set_tx_fail(&tr0, 1);

	int r = cli_print(&sh0, "hello");
	assert(r < 0 && sh0.tx_failed == 1);
	assert(sh0.tx_dropped == 0);                   /* write()<0 path: not a drop */

	size_t n;
	cli_dummy_output(&tr0, &n);
	assert(n == 0);                                /* nothing accepted */
}

/* §11: a TX failure during a command forces a non-zero command result even
 * though the handler returned 0 (it ignored cli_print's return). */
static void test_tx_failure_promotes_result(void)
{
	reset(&sh0, &tr0);
	cli_dummy_set_tx_fail(&tr0, 1);
	run_line(&sh0, "hello\r");
	assert(sh0.last_result == CLI_DISPATCH_ERR);
}

/* ---- §9 / §18 abnormal cases ------------------------------------------- */

static void test_unknown_command(void)
{
	reset(&sh0, &tr0);
	run_line(&sh0, "nope\r");
	assert(has(&tr0, "nope: command not found"));
	assert(sh0.last_result == CLI_DISPATCH_ERR);

	/* fail-safe: prompt resumes and the next command still runs. */
	size_t n;
	const char *o = cli_dummy_output(&tr0, &n);
	assert(n >= 2 && strcmp(o + n - 2, "> ") == 0);
	run_line(&sh0, "hello\r");
	assert(has(&tr0, "OK"));
}

static void test_arg_errors(void)
{
	reset(&sh0, &tr0);
	run_line(&sh0, "need2\r");                     /* mandatory 2, got 1 */
	assert(has(&tr0, "need2: invalid number of arguments"));
	assert(sh0.last_result == CLI_DISPATCH_ERR);

	reset(&sh0, &tr0);
	run_line(&sh0, "hello a b c d\r");             /* > CLI_MAX_ARGC tokens */
	assert(has(&tr0, "too many arguments"));

	reset(&sh0, &tr0);
	run_line(&sh0, "thing\r");                     /* parent, no handler */
	assert(has(&tr0, "thing: missing or unknown subcommand"));

	reset(&sh0, &tr0);
	run_line(&sh0, "hello \"ab\r");                /* unterminated quote */
	assert(has(&tr0, "unterminated quote"));
}

static void test_ctrl_c(void)
{
	reset(&sh0, &tr0);
	const uint8_t seq[] = { 'p', 'a', 'r', 't', 0x03 };   /* "part" + Ctrl+C */
	inject_bytes(&sh0, seq, sizeof seq);
	assert(sh0.len == 0);
	assert(has(&tr0, "^C"));
	size_t n;
	const char *o = cli_dummy_output(&tr0, &n);
	assert(n >= 2 && strcmp(o + n - 2, "> ") == 0);   /* fresh prompt */
}

static void test_line_overflow_bel(void)
{
	/* CLI_CMD_BUFFER_SIZE=16 -> at most 15 chars; further chars ring the bell. */
	reset(&sh0, &tr0);
	uint8_t blast[40];
	memset(blast, 'x', sizeof blast);
	inject_bytes(&sh0, blast, sizeof blast);
	assert(sh0.len == CLI_CMD_BUFFER_SIZE - 1);
	assert(has(&tr0, "\x07"));                      /* BEL */
}

static void test_rx_overflow_drops(void)
{
	/* A burst larger than the RX FIFO overflows: excess dropped + counted,
	 * both in the backend and in the instance stat (req §9/§18 10e). */
	reset(&sh0, &tr0);
	uint8_t burst[CLI_DUMMY_RX_BUFFER_SIZE + 64];
	memset(burst, 'y', sizeof burst);
	cli_dummy_inject(&tr0, burst, sizeof burst);    /* no pump: let it overflow */
	assert(tr0_ctx.rx_dropped > 0);
	assert(sh0.rx_dropped == tr0_ctx.rx_dropped);
}

/* ---- §10 multi-instance isolation -------------------------------------- */

static void test_multi_instance_isolation(void)
{
	reset(&sh0, &tr0);
	reset(&sh1, &tr1);

	/* Interleave the two sessions byte-group by byte-group; outputs must not
	 * cross and per-instance state stays independent (req §10). */
	run_line(&sh0, "hel");
	run_line(&sh1, "no");
	run_line(&sh0, "lo\r");                         /* sh0: "hello" -> OK */
	run_line(&sh1, "pe\r");                         /* sh1: "nope"  -> error */

	assert(has(&tr0, "OK"));
	assert(!has(&tr0, "command not found"));
	assert(has(&tr1, "nope: command not found"));
	assert(!has(&tr1, "OK"));                        /* no crosstalk */
	assert(sh0.last_result == 0);
	assert(sh1.last_result == CLI_DISPATCH_ERR);
}

int main(void)
{
	test_basic_dispatch();
	test_filter_and_editing_via_backend();
	test_backpressure_completes();
	test_tx_timeout_drops();
	test_tx_immediate_fail();
	test_tx_failure_promotes_result();
	test_unknown_command();
	test_arg_errors();
	test_ctrl_c();
	test_line_overflow_bel();
	test_rx_overflow_drops();
	test_multi_instance_isolation();
	printf("OK: dummy backend end-to-end / flow control / abnormal / isolation pass\n");
	return 0;
}
