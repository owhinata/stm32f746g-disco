/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cli_session.c
 * @brief   Shell line pipeline: ASCII filter, RX state machine, dispatch.
 *
 * Pure line/dispatch logic over a `struct cli_instance`.  It deliberately calls
 * no ThreadX (tx_*) API -- the thread loop, object creation and ISR notify live
 * in cli_core.c -- so this file compiles and unit-tests on the host against a
 * small tx_api.h type shim (shell/test/shim).  Output here is a minimal blocking
 * raw write to the transport; the buffered/locked/coloured output API is added
 * by issue #5.  Clean-room design inspired by Zephyr shell; no code reused.
 */
#include <stddef.h>
#include <string.h>

#include "cli_instance.h"
#include "cli_internal.h"

int cli_raw_write_unlocked(struct cli_instance *sh, const void *data, size_t len)
{
	if (sh->tr == NULL || sh->tr->api == NULL || sh->tr->api->write == NULL)
		return -1;
	return sh->tr->api->write(sh->tr, (const uint8_t *)data, len);
}

static void raw_str(struct cli_instance *sh, const char *s)
{
	cli_raw_write_unlocked(sh, s, strlen(s));
}

void cli_prompt(struct cli_instance *sh)
{
	raw_str(sh, sh->prompt);
}

void cli_dispatch_line(struct cli_instance *sh)
{
	raw_str(sh, "\r\n");                 /* echo the newline before any output */
	sh->line[sh->len] = '\0';

	enum cli_parse_status st =
		cli_parse(sh->line, sh->argv, CLI_ARGV_CAP, &sh->pr);

	switch (st) {
	case CLI_PARSE_OK:
		/* Pass the handler-relative argv/argc (argv[0] = leaf name), never
		 * the root-anchored sh->argv. */
		sh->last_result = sh->pr.cmd->handler(sh, sh->pr.argc, sh->pr.argv);
		break;
	case CLI_PARSE_EMPTY:
		break;                          /* blank line: just reprompt */
	case CLI_PARSE_NOT_FOUND:
		raw_str(sh, sh->pr.argv[0]);
		raw_str(sh, ": command not found\r\n");
		sh->last_result = CLI_DISPATCH_ERR;
		break;
	case CLI_PARSE_NO_HANDLER:
		raw_str(sh, sh->pr.argv[0]);
		raw_str(sh, ": missing or unknown subcommand\r\n");
		sh->last_result = CLI_DISPATCH_ERR;
		break;
	case CLI_PARSE_WRONG_ARGS:
		raw_str(sh, sh->pr.argv[0]);
		raw_str(sh, ": invalid number of arguments\r\n");
		sh->last_result = CLI_DISPATCH_ERR;
		break;
	case CLI_PARSE_TOO_MANY_TOKENS:
		raw_str(sh, "too many arguments\r\n");
		sh->last_result = CLI_DISPATCH_ERR;
		break;
	case CLI_PARSE_NESTING_TOO_DEEP:
		raw_str(sh, "command nesting too deep\r\n");
		sh->last_result = CLI_DISPATCH_ERR;
		break;
	case CLI_PARSE_UNTERMINATED_QUOTE:
		raw_str(sh, "unterminated quote\r\n");
		sh->last_result = CLI_DISPATCH_ERR;
		break;
	default:
		sh->last_result = CLI_DISPATCH_ERR;
		break;
	}

	/* Reset the line and return to the prompt no matter what happened above --
	 * a bad command or a non-zero handler return never stops the shell, and one
	 * instance's error cannot reach another (fail-safe, req §9). */
	sh->len = 0;
	sh->line[0] = '\0';
	sh->rx = CLI_RX_NORMAL;
	cli_prompt(sh);
}

void cli_input_byte(struct cli_instance *sh, uint8_t b)
{
	/* ASCII filter (req §13): non-ASCII bytes never reach the line buffer. */
	if (b & 0x80u)
		return;

	/* Ctrl+C cancels the input line from ANY state, including a half-read
	 * escape sequence -- so a stuck terminal always recovers (req §9). */
	if (b == 0x03) {
		sh->rx = CLI_RX_NORMAL;
		sh->prev_cr = 0;
		raw_str(sh, "^C\r\n");
		sh->len = 0;
		sh->line[0] = '\0';
		cli_prompt(sh);
		return;
	}

	/* Minimal escape swallower: keep arrow keys etc. out of the line.  Full
	 * VT100 parsing replaces these states in issue #9. */
	if (sh->rx == CLI_RX_ESC) {
		sh->rx = (b == '[') ? CLI_RX_CSI : CLI_RX_NORMAL;
		return;
	}
	if (sh->rx == CLI_RX_CSI) {
		if (b >= 0x40 && b <= 0x7E)     /* CSI final byte */
			sh->rx = CLI_RX_NORMAL;
		return;
	}

	/* CLI_RX_NORMAL.  Handle line endings first so prev_cr coalesces CR-LF into
	 * a single dispatch (dispatch resets the line but leaves prev_cr to us). */
	if (b == '\r') {
		cli_dispatch_line(sh);
		sh->prev_cr = 1;
		return;
	}
	if (b == '\n') {
		if (sh->prev_cr) {              /* the LF half of a CR-LF: swallow */
			sh->prev_cr = 0;
			return;
		}
		cli_dispatch_line(sh);
		return;
	}
	sh->prev_cr = 0;

	if (b == 0x1B) {                        /* ESC: begin swallow */
		sh->rx = CLI_RX_ESC;
		return;
	}
	if (b == 0x08 || b == 0x7F) {           /* Backspace / DEL: erase one char */
		if (sh->len > 0) {
			sh->len--;
			sh->line[sh->len] = '\0';
			raw_str(sh, "\b \b");
		}
		return;
	}
	if (b >= 0x20 && b <= 0x7E) {           /* printable: append + echo */
		if (sh->len >= CLI_CMD_BUFFER_SIZE - 1) {
			uint8_t bel = 0x07;         /* line full (req §8): ring the bell */
			cli_raw_write_unlocked(sh, &bel, 1);
			return;
		}
		sh->line[sh->len++] = (char)b;
		sh->line[sh->len] = '\0';
		cli_raw_write_unlocked(sh, &b, 1);
		return;
	}
	/* Any other control byte: ignore. */
}
