/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host unit test for the Shell core (issue #4): the ASCII filter, the RX state
 * machine (printable/echo, Backspace, BEL on full, Ctrl+C, ESC/CSI swallow,
 * CR / LF / CR-LF coalescing) and dispatch (cli_parse status -> message, handler
 * invocation with handler-relative argv, fail-safe, instance isolation).
 *
 * Drives the ThreadX-free cli_session.c directly (no thread, no backend): a
 * capture transport records everything written, and bytes are injected with
 * cli_input_byte().  The tx_* glue in cli_core.c is out of scope here and is
 * exercised on target.  Compiled with small CLI_* limits (see run_host_tests.sh)
 * so the buffer-full and too-many-tokens paths fit a compact input.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cli_instance.h"
#include "cli_internal.h"

/* ---- capture transport ------------------------------------------------- */

struct cap {
	char   buf[2048];
	size_t n;
};

static int cap_init(struct cli_transport *tr)   { (void)tr; return 0; }
static int cap_enable(struct cli_transport *tr) { (void)tr; return 0; }
static int cap_read(struct cli_transport *tr, uint8_t *d, size_t cap)
{
	(void)tr; (void)d; (void)cap; return 0;   /* test injects via cli_input_byte */
}
static int cap_write(struct cli_transport *tr, const uint8_t *d, size_t len)
{
	struct cap *c = (struct cap *)tr->ctx;
	for (size_t i = 0; i < len && c->n < sizeof c->buf - 1; i++)
		c->buf[c->n++] = (char)d[i];
	c->buf[c->n] = '\0';
	return (int)len;
}
static const struct cli_transport_api cap_api = {
	cap_init, cap_enable, cap_write, cap_read, NULL, NULL,
};

/* #5 output-path hooks (ThreadX glue is firmware-only): lock is a no-op on the
 * host, and the "send" stub captures bytes -- or, when tx_fail is armed, drops
 * them and fails so the dispatch can exercise the §11 non-zero promotion. */
static int tx_fail;

int cli_lock(struct cli_instance *sh)   { (void)sh; return 0; }
void cli_unlock(struct cli_instance *sh) { (void)sh; }

int cli_tx_send_blocking(struct cli_instance *sh, const uint8_t *data, size_t len)
{
	struct cap *c = (struct cap *)sh->tr->ctx;
	if (tx_fail) {
		sh->tx_dropped += (uint32_t)len;
		return -1;
	}
	for (size_t i = 0; i < len && c->n < sizeof c->buf - 1; i++)
		c->buf[c->n++] = (char)data[i];
	c->buf[c->n] = '\0';
	return 0;
}

/* ---- test commands ----------------------------------------------------- */

static int  ran_argc;
static char ran_a0[32];
static char ran_a1[32];

static int h_echo(struct cli_instance *sh, int argc, char **argv)
{
	ran_argc = argc;
	ran_a0[0] = ran_a1[0] = '\0';
	if (argc > 0) { strncpy(ran_a0, argv[0], sizeof ran_a0 - 1); ran_a0[sizeof ran_a0 - 1] = '\0'; }
	if (argc > 1) { strncpy(ran_a1, argv[1], sizeof ran_a1 - 1); ran_a1[sizeof ran_a1 - 1] = '\0'; }
	cli_write(sh, "echo-ran\r\n", 10);
	return 0;
}
static int h_fail(struct cli_instance *sh, int argc, char **argv)
{
	(void)sh; (void)argc; (void)argv;
	return 7;
}

CLI_SUBCMD_SET_CREATE(sub_thr, CLI_CMD_ARG(list, NULL, "list", h_echo, 1, 0),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(version, NULL,    "show version", h_echo, 1, 0);
CLI_CMD_REGISTER(fails,   NULL,    "always fails", h_fail, 1, 0);
CLI_CMD_REGISTER(threads, sub_thr, "threads",      NULL,   1, 0);
CLI_CMD_REGISTER(need2,   NULL,    "needs 2 args", h_echo, 2, 0);

/* ---- harness ----------------------------------------------------------- */

static struct cap          cap_ctx;
static struct cli_transport tr = { &cap_api, NULL, &cap_ctx };
static struct cli_instance  sh;

static void reset_sh(void)
{
	memset(&sh, 0, sizeof sh);
	sh.tr = &tr;
	tr.sh = &sh;
	tr.ctx = &cap_ctx;
	strcpy(sh.prompt, "> ");
	cap_ctx.n = 0;
	cap_ctx.buf[0] = '\0';
	ran_argc = -1;
	ran_a0[0] = ran_a1[0] = '\0';
	tx_fail = 0;
}

static void feed(const char *s)
{
	for (const char *p = s; *p; p++)
		cli_input_byte(&sh, (uint8_t)*p);
}

static void feed_byte(uint8_t b) { cli_input_byte(&sh, b); }

static int count_occurrences(const char *hay, const char *needle)
{
	int n = 0;
	size_t nl = strlen(needle);
	for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += nl)
		n++;
	return n;
}
static int cap_has(const char *needle) { return strstr(cap_ctx.buf, needle) != NULL; }

/* ---- tests ------------------------------------------------------------- */

static void test_input_editing(void)
{
	reset_sh();
	feed("abc");
	assert(sh.len == 3 && strcmp(sh.line, "abc") == 0);
	assert(cap_has("abc"));                       /* chars are echoed */

	/* ASCII filter: high-bit bytes are dropped, line unchanged. */
	feed_byte(0xC3);
	feed_byte(0x80);
	assert(sh.len == 3 && strcmp(sh.line, "abc") == 0);

	/* Backspace erases one char and emits the destructive sequence. */
	reset_sh();
	feed("ab");
	feed_byte(0x08);
	assert(sh.len == 1 && strcmp(sh.line, "a") == 0);
	assert(cap_has("\b \b"));
	feed_byte(0x7F);                              /* DEL also erases */
	assert(sh.len == 0);

	/* ESC / CSI (arrow-key) sequence is swallowed, never entering the line. */
	reset_sh();
	feed("a");
	feed_byte(0x1B); feed_byte('['); feed_byte('C');   /* right arrow */
	feed("b");
	assert(sh.len == 2 && strcmp(sh.line, "ab") == 0);
}

static void test_bel_on_full(void)
{
	/* Compiled with CLI_CMD_BUFFER_SIZE=16 -> at most 15 chars. */
	reset_sh();
	for (int i = 0; i < 40; i++)
		feed_byte('x');
	assert(sh.len == CLI_CMD_BUFFER_SIZE - 1);
	assert(cap_has("\x07"));                       /* bell rung */
}

static void test_ctrl_c(void)
{
	reset_sh();
	feed("partial");
	feed_byte(0x03);                              /* Ctrl+C */
	assert(sh.len == 0);
	assert(cap_has("^C"));
	assert(strstr(cap_ctx.buf, "> ") != NULL);    /* fresh prompt */

	/* Ctrl+C recovers even from a half-read escape sequence. */
	reset_sh();
	feed("ab");
	feed_byte(0x1B); feed_byte('[');              /* enter CSI state */
	feed_byte(0x03);                              /* Ctrl+C cancels + resets state */
	assert(sh.rx == CLI_RX_NORMAL && sh.len == 0);
	feed("cd");                                   /* subsequent input is normal */
	assert(sh.len == 2 && strcmp(sh.line, "cd") == 0);
}

static void test_dispatch_ok(void)
{
	reset_sh();
	feed("version\r");
	assert(cap_has("echo-ran"));
	assert(ran_argc == 1 && strcmp(ran_a0, "version") == 0);
	assert(sh.len == 0);                          /* line reset after dispatch */
	assert(sh.last_result == 0);
	/* prompt reappears at the very end */
	assert(strcmp(cap_ctx.buf + cap_ctx.n - 2, "> ") == 0);

	/* Subcommand: handler gets the leaf-relative argv (argv[0] == "list"). */
	reset_sh();
	feed("threads list\r");
	assert(cap_has("echo-ran"));
	assert(ran_argc == 1 && strcmp(ran_a0, "list") == 0);
}

static void test_newline_coalescing(void)
{
	reset_sh();
	feed("version\r\n");                          /* CR-LF: one dispatch */
	assert(count_occurrences(cap_ctx.buf, "echo-ran") == 1);

	reset_sh();
	feed("version\n");                            /* lone LF: one dispatch */
	assert(count_occurrences(cap_ctx.buf, "echo-ran") == 1);

	reset_sh();
	feed("version\r");                            /* lone CR: one dispatch */
	assert(count_occurrences(cap_ctx.buf, "echo-ran") == 1);
}

static void test_dispatch_errors(void)
{
	reset_sh();
	feed("nope\r");
	assert(cap_has("nope: command not found"));
	assert(sh.last_result == CLI_DISPATCH_ERR);

	reset_sh();
	feed("threads\r");                            /* pure parent, no handler */
	assert(cap_has("threads: missing or unknown subcommand"));
	assert(sh.last_result == CLI_DISPATCH_ERR);

	reset_sh();
	feed("need2\r");                              /* mandatory 2, got 1 */
	assert(cap_has("need2: invalid number of arguments"));

	reset_sh();
	feed("version a b c d\r");                    /* > CLI_MAX_ARGC(4) tokens */
	assert(cap_has("too many arguments"));

	reset_sh();
	feed("version \"ab\r");                       /* unterminated quote */
	assert(cap_has("unterminated quote"));

	reset_sh();
	feed("\r");                                   /* blank line: no error */
	assert(!cap_has("not found") && !cap_has("invalid"));
}

static void test_fail_safe(void)
{
	reset_sh();
	feed("fails\r");
	assert(sh.last_result == 7);                  /* non-zero handler return kept */
	/* shell keeps going: a following command still dispatches */
	feed("version\r");
	assert(cap_has("echo-ran"));
}

/* §11: a TX failure during a command forces a non-zero result even though the
 * handler itself returned 0 (it did not check cli_print's return). */
static void test_tx_failure_promotes_result(void)
{
	reset_sh();
	tx_fail = 1;
	feed("version\r");
	tx_fail = 0;
	assert(sh.last_result == CLI_DISPATCH_ERR);
	assert(sh.tx_dropped > 0);
}

/* Two independent instances must not share state or cross output streams. */
static void test_instance_isolation(void)
{
	struct cap c1 = {0}, c2 = {0};
	struct cli_transport t1 = { &cap_api, NULL, &c1 };
	struct cli_transport t2 = { &cap_api, NULL, &c2 };
	struct cli_instance  s1, s2;

	memset(&s1, 0, sizeof s1); s1.tr = &t1; t1.sh = &s1; strcpy(s1.prompt, "1> ");
	memset(&s2, 0, sizeof s2); s2.tr = &t2; t2.sh = &s2; strcpy(s2.prompt, "2> ");

	for (const char *p = "version\r"; *p; p++) cli_input_byte(&s1, (uint8_t)*p);
	for (const char *p = "no\r";      *p; p++) cli_input_byte(&s2, (uint8_t)*p);

	assert(strstr(c1.buf, "echo-ran") != NULL);            /* s1 ran the command */
	assert(strstr(c1.buf, "command not found") == NULL);
	assert(strstr(c2.buf, "no: command not found") != NULL); /* s2 saw only its own */
	assert(strstr(c2.buf, "echo-ran") == NULL);            /* no cross-talk */
	assert(s1.last_result == 0 && s2.last_result == CLI_DISPATCH_ERR);
}

int main(void)
{
	test_input_editing();
	test_bel_on_full();
	test_ctrl_c();
	test_dispatch_ok();
	test_newline_coalescing();
	test_dispatch_errors();
	test_fail_safe();
	test_tx_failure_promotes_result();
	test_instance_isolation();
	printf("OK: ascii filter / state machine / dispatch / fail-safe / isolation pass\n");
	return 0;
}
