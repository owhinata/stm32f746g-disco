/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host unit test for the Shell output API (issue #5): the minimal formatter
 * (conversions, length modifiers, width/flags and their boundaries), 32 B
 * staging + autoflush across a >32 B write, VT100 colour for error/warn/info,
 * hexdump layout, and TX-failure handling (drop + tx_failed + tx_dropped + the
 * negative return).  Drives cli_printf.c directly with no-op lock stubs and a
 * capturing cli_tx_send_blocking; the ThreadX flow control in cli_core.c is
 * out of scope here (it is compile-smoked for ARM and reviewed).  Built with
 * colour ON (the default) so the SGR escapes are asserted.
 */
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cli_instance.h"
#include "cli_internal.h"
#include "cli_vt100.h"

struct cap { char buf[4096]; size_t n; };

static int dummy_io(struct cli_transport *tr, const uint8_t *d, size_t n)
{ (void)tr; (void)d; (void)n; return 0; }
static int dummy_rd(struct cli_transport *tr, uint8_t *d, size_t n)
{ (void)tr; (void)d; (void)n; return 0; }
static int dummy_ie(struct cli_transport *tr) { (void)tr; return 0; }
static const struct cli_transport_api api = {
	dummy_ie, dummy_ie, dummy_io, dummy_rd, NULL, NULL,
};

static int tx_fail;

int cli_lock(struct cli_instance *sh)    { (void)sh; return 0; }
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

static struct cap           cap_ctx;
static struct cli_transport tr = { &api, NULL, &cap_ctx };
static struct cli_instance  sh;

static void setup(void)
{
	memset(&sh, 0, sizeof sh);
	sh.tr = &tr;
	tr.sh = &sh;
	tr.ctx = &cap_ctx;
	cap_ctx.n = 0;
	cap_ctx.buf[0] = '\0';
	tx_fail = 0;
}

#define EXPECT(want, ...) do {                                                \
	setup();                                                             \
	cli_print(&sh, __VA_ARGS__);                                        \
	if (strcmp(cap_ctx.buf, (want)) != 0) {                            \
		printf("FAIL line %d: got [%s] want [%s]\n",                \
		       __LINE__, cap_ctx.buf, (want));                     \
		assert(0);                                                  \
	}                                                                   \
} while (0)

/* Unattributed alias: deliberately-malformed formats (%q, trailing %, NULL %s)
 * are valid runtime inputs to exercise, but the format(printf) attribute would
 * warn on the literals.  Calls through this pointer are not format-checked. */
static int (*print_raw)(struct cli_instance *, const char *, ...) = cli_print;

static void test_formatter(void)
{
	EXPECT("hello", "hello");
	EXPECT("a%b", "a%%b");
	EXPECT("A", "%c", 'A');
	EXPECT("x=42", "x=%d", 42);
	EXPECT("-7", "%d", -7);
	EXPECT("4000000000", "%u", 4000000000U);
	EXPECT("ab AB", "%x %X", 0xab, 0xab);
	setup();
	print_raw(&sh, "%s", (char *)NULL);      /* NULL %s -> "(null)" */
	assert(strcmp(cap_ctx.buf, "(null)") == 0);
	EXPECT("hi there", "%s %s", "hi", "there");

	/* length modifiers */
	EXPECT("1234567890", "%lu", 1234567890UL);
	EXPECT("9223372036854775807", "%lld", (long long)9223372036854775807LL);
	EXPECT("deadbeef", "%llx", 0xdeadbeefULL);

	/* width / flags */
	EXPECT("   42", "%5d", 42);
	EXPECT("42   |", "%-5d|", 42);
	EXPECT("0000001f", "%08x", 0x1fu);
	EXPECT("  abc", "%5s", "abc");
	EXPECT("abc  |", "%-5s|", "abc");
}

static void test_formatter_boundaries(void)
{
	EXPECT("-2147483648", "%d", INT_MIN);
	EXPECT("-9223372036854775808", "%lld", LLONG_MIN);

	setup();
	print_raw(&sh, "%q");                     /* unknown spec: verbatim */
	assert(strcmp(cap_ctx.buf, "%q") == 0);

	setup();
	print_raw(&sh, "end%");                   /* trailing '%' */
	assert(strcmp(cap_ctx.buf, "end%") == 0);

	/* %p prints 0x-prefixed hex of the pointer value */
	setup();
	cli_print(&sh, "%p", (void *)0x1234);
	assert(strcmp(cap_ctx.buf, "0x1234") == 0);

	/* %zu reads size_t; %zd its signed counterpart (width-matched per ABI) */
	setup();
	cli_print(&sh, "%zu", (size_t)4096);
	assert(strcmp(cap_ctx.buf, "4096") == 0);

	setup();
	print_raw(&sh, "%zd", (long)-42);   /* signed size (long matches size_t on host) */
	assert(strcmp(cap_ctx.buf, "-42") == 0);

	/* absurd width is capped, not overflowed (no crash, bounded output) */
	setup();
	cli_print(&sh, "%999999999d", 1);
	assert(cap_ctx.n <= 4096);
}

static void test_autoflush(void)
{
	char big[100];
	for (int i = 0; i < 99; i++) big[i] = 'A' + (i % 26);
	big[99] = '\0';

	setup();
	cli_print(&sh, "%s", big);               /* > 32 B staging: flushes ~4x */
	assert(cap_ctx.n == 99);
	assert(strcmp(cap_ctx.buf, big) == 0);   /* every byte arrived, in order */
}

static void test_color(void)
{
	setup();
	cli_error(&sh, "oops");
	assert(strcmp(cap_ctx.buf, "\x1b[31moops\x1b[0m") == 0);

	setup();
	cli_warn(&sh, "careful");
	assert(strcmp(cap_ctx.buf, "\x1b[33mcareful\x1b[0m") == 0);

	setup();
	cli_info(&sh, "ok");
	assert(strcmp(cap_ctx.buf, "\x1b[32mok\x1b[0m") == 0);

	setup();
	cli_print(&sh, "plain");                 /* no colour on cli_print */
	assert(strcmp(cap_ctx.buf, "plain") == 0);
}

static void test_write_and_hexdump(void)
{
	setup();
	assert(cli_write(&sh, "raw\x00\x01", 5) == 0);
	assert(cap_ctx.n == 5 && memcmp(cap_ctx.buf, "raw\x00\x01", 5) == 0);

	setup();
	cli_hexdump(&sh, "Hi\x01!", 4);
	assert(strstr(cap_ctx.buf, "00000000  48 69 01 21 ") != NULL);
	assert(strstr(cap_ctx.buf, "Hi.!\r\n") != NULL);   /* 0x01 -> '.' */
}

static void test_tx_failure(void)
{
	setup();
	tx_fail = 1;
	int r = cli_print(&sh, "dropme");
	assert(r < 0 && sh.tx_failed == 1 && sh.tx_dropped > 0);
	assert(strstr(cap_ctx.buf, "dropme") == NULL);     /* nothing emitted */

	/* sticky: further output stays dropped until tx_failed is cleared */
	r = cli_print(&sh, "again");
	assert(r < 0 && strstr(cap_ctx.buf, "again") == NULL);

	/* recovery: clear the flag, stop failing -> output flows again */
	sh.tx_failed = 0;
	tx_fail = 0;
	r = cli_print(&sh, "back");
	assert(r == 0 && strcmp(cap_ctx.buf, "back") == 0);
}

int main(void)
{
	test_formatter();
	test_formatter_boundaries();
	test_autoflush();
	test_color();
	test_write_and_hexdump();
	test_tx_failure();
	printf("OK: formatter / boundaries / autoflush / colour / hexdump / tx-fail pass\n");
	return 0;
}
