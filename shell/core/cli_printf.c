/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cli_printf.c
 * @brief   Shell output API: minimal formatter, staging buffer, colour, hexdump.
 *
 * The public cli_print/error/warn/info/write/hexdump are implemented here.  Each
 * call is bracketed by cli_lock/cli_unlock (per-instance TX mutex) so formatting,
 * 32 B staging and flush are atomic, then reaches the transport only through
 * cli_tx_send_blocking() -- the ThreadX-specific lock + flow control live in
 * cli_core.c.  This file calls no tx_* function, so it builds and unit-tests on
 * the host against the tx_api.h shim.
 *
 * The formatter is a small original implementation that streams characters into
 * the staging buffer (no large intermediate buffer; honours the §8 "32 B printf
 * buffer, flush when full" model).  Supported conversions: %% %c %s %d %i %u %x
 * %X %p, length modifiers l / ll / z, field width with '0' / '-' flags.  No
 * precision, no + / space / # flags.  It is clean-room -- neither newlib's nor
 * Zephyr's printf code is reused.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cli_instance.h"
#include "cli_internal.h"
#include "cli_vt100.h"

/* ---- staging ----------------------------------------------------------- */

void cli_out_flush(struct cli_instance *sh)
{
	if (sh->out_len == 0)
		return;
	/* Once a send has failed this command, discard the rest (req §11). */
	if (!sh->tx_failed) {
		if (cli_tx_send_blocking(sh, (const uint8_t *)sh->out_buf,
		                         sh->out_len) < 0)
			sh->tx_failed = 1;
	}
	sh->out_len = 0;
}

void cli_out_putc(struct cli_instance *sh, char c)
{
	if (sh->tx_failed)
		return;                         /* drop further output this command */
	sh->out_buf[sh->out_len++] = c;
	if (sh->out_len >= CLI_PRINTF_BUFFER_SIZE)
		cli_out_flush(sh);
}

static void out_str(struct cli_instance *sh, const char *s)
{
	while (*s)
		cli_out_putc(sh, *s++);
}

/* ---- minimal formatter ------------------------------------------------- */

enum len_mod { LM_INT, LM_LONG, LM_LLONG, LM_SIZE };

/* Emit a body (digits/text) of @p blen chars with optional sign / "0x" prefix,
 * padded to @p width using spaces, or zeros when @p zero (zeros go after the
 * sign/prefix), left-justified when @p left. */
static void emit_padded(struct cli_instance *sh, const char *prefix,
                        const char *body, int blen, int width,
                        int zero, int left)
{
	int plen = prefix ? (int)strlen(prefix) : 0;
	int total = plen + blen;
	int pad = width > total ? width - total : 0;

	if (left) {
		if (prefix) out_str(sh, prefix);
		for (int i = 0; i < blen; i++) cli_out_putc(sh, body[i]);
		while (pad-- > 0) cli_out_putc(sh, ' ');
	} else if (zero) {
		if (prefix) out_str(sh, prefix);
		while (pad-- > 0) cli_out_putc(sh, '0');
		for (int i = 0; i < blen; i++) cli_out_putc(sh, body[i]);
	} else {
		while (pad-- > 0) cli_out_putc(sh, ' ');
		if (prefix) out_str(sh, prefix);
		for (int i = 0; i < blen; i++) cli_out_putc(sh, body[i]);
	}
}

/* Render @p mag in @p base (10/16) into @p buf (reversed-safe), return length.
 * @p upper selects A-F vs a-f.  buf must hold >= 20 chars. */
static int utoa_rev(unsigned long long mag, unsigned base, int upper, char *buf)
{
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char tmp[20];
	int n = 0;
	do {
		tmp[n++] = digits[mag % base];
		mag /= base;
	} while (mag != 0);
	for (int i = 0; i < n; i++)
		buf[i] = tmp[n - 1 - i];     /* un-reverse */
	return n;
}

/* Read the integer arg inline.  va_list is an array type on some ABIs (so it
 * decays to a pointer as a function parameter); reading directly off `ap` here
 * keeps the single sequential va_arg cursor correct -- passing &ap to a helper
 * would mistype it.  Magnitude is unsigned so LLONG_MIN negates safely. */
#define GET_UNSIGNED(lm, ap)                                                  \
	((lm) == LM_LONG  ? (unsigned long long)va_arg((ap), unsigned long)  : \
	 (lm) == LM_LLONG ? va_arg((ap), unsigned long long)                 : \
	 (lm) == LM_SIZE  ? (unsigned long long)va_arg((ap), size_t)         : \
	                    (unsigned long long)va_arg((ap), unsigned int))

/* For %zd the argument is the *signed type corresponding to size_t*, which has
 * no portable name -- read it as whichever standard signed type matches size_t's
 * width (int on targets where size_t is unsigned int, e.g. Cortex-M; long on
 * LP64 hosts).  Reading a fixed `long` would be the wrong va_arg type on M7. */
#define GET_SIGNED(lm, ap)                                                    \
	((lm) == LM_LONG  ? (long long)va_arg((ap), long)      :              \
	 (lm) == LM_LLONG ? va_arg((ap), long long)            :              \
	 (lm) == LM_SIZE                                                       \
		? (sizeof(size_t) == sizeof(int)  ? (long long)va_arg((ap), int)  : \
		   sizeof(size_t) == sizeof(long) ? (long long)va_arg((ap), long) : \
		                                    va_arg((ap), long long))      : \
	                    (long long)va_arg((ap), int))

static void cli_vprintf(struct cli_instance *sh, const char *fmt, va_list ap)
{
	for (const char *p = fmt; *p; p++) {
		if (*p != '%') {
			cli_out_putc(sh, *p);
			continue;
		}
		p++;                            /* consume '%' */

		int left = 0, zero = 0;
		for (;; p++) {                  /* flags */
			if (*p == '-')      left = 1;
			else if (*p == '0') zero = 1;
			else break;
		}

		int width = 0;                  /* field width (capped at 4096) */
		while (*p >= '0' && *p <= '9') {
			width = width * 10 + (*p - '0');
			if (width > 4096)
				width = 4096;
			p++;
		}

		enum len_mod lm = LM_INT;       /* length modifier */
		if (*p == 'l') {
			p++;
			if (*p == 'l') { lm = LM_LLONG; p++; }
			else             lm = LM_LONG;
		} else if (*p == 'z') {
			lm = LM_SIZE;
			p++;
		}

		char body[20];
		int  blen;
		switch (*p) {
		case '%':
			cli_out_putc(sh, '%');
			break;
		case 'c': {
			char ch = (char)va_arg(ap, int);
			emit_padded(sh, NULL, &ch, 1, width, 0, left);
			break;
		}
		case 's': {
			const char *s = va_arg(ap, const char *);
			if (s == NULL) s = "(null)";
			emit_padded(sh, NULL, s, (int)strlen(s), width, 0, left);
			break;
		}
		case 'd':
		case 'i': {
			long long v = GET_SIGNED(lm, ap);
			unsigned long long mag = (v < 0)
				? (unsigned long long)(-(v + 1)) + 1ULL   /* safe for LLONG_MIN */
				: (unsigned long long)v;
			blen = utoa_rev(mag, 10, 0, body);
			emit_padded(sh, v < 0 ? "-" : NULL, body, blen, width, zero, left);
			break;
		}
		case 'u':
			blen = utoa_rev(GET_UNSIGNED(lm, ap), 10, 0, body);
			emit_padded(sh, NULL, body, blen, width, zero, left);
			break;
		case 'x':
			blen = utoa_rev(GET_UNSIGNED(lm, ap), 16, 0, body);
			emit_padded(sh, NULL, body, blen, width, zero, left);
			break;
		case 'X':
			blen = utoa_rev(GET_UNSIGNED(lm, ap), 16, 1, body);
			emit_padded(sh, NULL, body, blen, width, zero, left);
			break;
		case 'p': {
			uintptr_t v = (uintptr_t)va_arg(ap, void *);
			blen = utoa_rev((unsigned long long)v, 16, 0, body);
			emit_padded(sh, "0x", body, blen, width, zero, left);
			break;
		}
		case '\0':                      /* trailing '%': emit literally, stop */
			cli_out_putc(sh, '%');
			return;
		default:                        /* unknown spec: emit verbatim */
			cli_out_putc(sh, '%');
			cli_out_putc(sh, *p);
			break;
		}
	}
}

/* ---- public API -------------------------------------------------------- */

/* Bracket a formatted call with lock + autoflush; @p color is "" for cli_print
 * or a VT100 SGR for the level helpers (and "" when CLI_USE_COLOR=0). */
static int vemit(struct cli_instance *sh, const char *color,
                 const char *fmt, va_list ap)
{
	if (cli_lock(sh) != 0) {
		sh->tx_failed = 1;      /* so the command result is forced non-zero */
		return -1;
	}
	if (color[0]) out_str(sh, color);
	cli_vprintf(sh, fmt, ap);
	if (color[0]) out_str(sh, CLI_VT100_RESET);
	cli_out_flush(sh);
	int r = sh->tx_failed ? -1 : 0;
	cli_unlock(sh);
	return r;
}

int cli_print(struct cli_instance *sh, const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	int r = vemit(sh, "", fmt, ap);
	va_end(ap);
	return r;
}

int cli_error(struct cli_instance *sh, const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	int r = vemit(sh, CLI_VT100_RED, fmt, ap);
	va_end(ap);
	return r;
}

int cli_warn(struct cli_instance *sh, const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	int r = vemit(sh, CLI_VT100_YELLOW, fmt, ap);
	va_end(ap);
	return r;
}

int cli_info(struct cli_instance *sh, const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	int r = vemit(sh, CLI_VT100_GREEN, fmt, ap);
	va_end(ap);
	return r;
}

int cli_write(struct cli_instance *sh, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	if (cli_lock(sh) != 0) {
		sh->tx_failed = 1;
		return -1;
	}
	for (size_t i = 0; i < len; i++)
		cli_out_putc(sh, (char)p[i]);
	cli_out_flush(sh);
	int r = sh->tx_failed ? -1 : 0;
	cli_unlock(sh);
	return r;
}

int cli_hexdump_base(struct cli_instance *sh, const void *data, size_t len,
                     unsigned long long base)
{
	const uint8_t *p = (const uint8_t *)data;
	char body[20];

	if (cli_lock(sh) != 0) {
		sh->tx_failed = 1;
		return -1;
	}

	for (size_t off = 0; off < len; off += 16) {
		int n = utoa_rev(base + (unsigned long long)off, 16, 0, body);
		emit_padded(sh, NULL, body, n, 8, 1, 0);   /* %08x offset */
		out_str(sh, "  ");

		for (int j = 0; j < 16; j++) {
			if (off + (size_t)j < len) {
				int h = utoa_rev(p[off + j], 16, 0, body);
				emit_padded(sh, NULL, body, h, 2, 1, 0);   /* %02x */
				cli_out_putc(sh, ' ');
			} else {
				out_str(sh, "   ");
			}
		}
		cli_out_putc(sh, ' ');

		for (int j = 0; j < 16 && off + (size_t)j < len; j++) {
			uint8_t b = p[off + j];
			cli_out_putc(sh, (b >= 0x20 && b <= 0x7E) ? (char)b : '.');
		}
		out_str(sh, "\r\n");
	}

	cli_out_flush(sh);
	int r = sh->tx_failed ? -1 : 0;
	cli_unlock(sh);
	return r;
}

int cli_hexdump(struct cli_instance *sh, const void *data, size_t len)
{
	return cli_hexdump_base(sh, data, len, 0);
}
