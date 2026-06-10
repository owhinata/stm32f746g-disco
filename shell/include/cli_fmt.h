/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cli_fmt.h
 * @brief   Clean-room minimal printf formatter; output is a putc callback.
 *
 * Lifted from cli_printf.c (issue #28) so the same formatter backs three sinks:
 * the shell output API (cli_print/cli_hexdump, ctx = struct cli_instance), the
 * RAM log (cli_vsnformat into a stack buffer), and the fault dump (ctx = a
 * polling-UART putc).  It depends only on <stdarg.h>/<stddef.h> -- no
 * cli_instance, no ThreadX, no HAL -- so src/ code (log.c, fault.c) can link it
 * without pulling in the shell.  Clean-room: neither newlib's nor Zephyr's
 * printf is reused.
 *
 * Supported conversions: %% %c %s %d %i %u %x %X %p, length modifiers l / ll / z,
 * field width with '0' / '-' flags.  No precision, no + / space / # flags.
 */
#ifndef CLI_FMT_H
#define CLI_FMT_H

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Character sink: receives one formatted byte at a time with caller context. */
typedef void (*cli_fmt_putc_t)(void *ctx, char c);

/**
 * Format @p fmt / @p ap and stream every output byte to @p put(@p ctx, c).
 * The core formatter; cli_print/cli_hexdump and the fault dump build on it.
 */
void cli_vformat(cli_fmt_putc_t put, void *ctx, const char *fmt, va_list ap);

/**
 * Format into @p buf (capacity @p size, always NUL-terminated when size > 0).
 * Returns the number of chars actually stored, excluding the NUL (the truncated
 * length; never >= size).  Used by the RAM log to render into a stack buffer.
 */
int cli_vsnformat(char *buf, size_t size, const char *fmt, va_list ap);

/**
 * Render @p mag in @p base (10/16) into @p buf (>= 20 chars), most-significant
 * digit first.  @p upper selects A-F vs a-f.  Returns the digit count.  Exposed
 * so hexdump can format offsets/bytes without a full conversion pass.
 */
int cli_fmt_utoa(unsigned long long mag, unsigned base, int upper, char *buf);

/**
 * Emit @p body (@p blen chars) with an optional @p prefix (sign / "0x"), padded
 * to @p width: spaces, or zeros (after the prefix) when @p zero, left-justified
 * when @p left.  The shared padding primitive for the conversions above.
 */
void cli_fmt_padded(cli_fmt_putc_t put, void *ctx, const char *prefix,
                    const char *body, int blen, int width, int zero, int left);

#ifdef __cplusplus
}
#endif

#endif /* CLI_FMT_H */
