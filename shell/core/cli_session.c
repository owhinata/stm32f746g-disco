/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cli_session.c
 * @brief   Shell line pipeline: ASCII filter, RX state machine, dispatch.
 *
 * Pure line/dispatch logic over a `struct cli_instance`.  It calls no ThreadX
 * (tx_*) API -- the thread loop, object creation and ISR notify live in
 * cli_core.c -- so this file compiles and unit-tests on the host against a small
 * tx_api.h type shim (shell/test/shim).  All output goes through the buffered
 * output API (cli_write/cli_error, issue #5): echo, prompt and dispatch messages
 * are flow-controlled and (for errors) coloured.  Clean-room design inspired by
 * Zephyr shell; no code reused.
 */
#include <stddef.h>
#include <string.h>

#include "cli_instance.h"
#include "cli_internal.h"

void cli_prompt(struct cli_instance *sh)
{
	cli_write(sh, sh->prompt, strlen(sh->prompt));
}

void cli_dispatch_line(struct cli_instance *sh)
{
	sh->tx_failed = 0;                  /* fresh flow-control state per command */
	cli_write(sh, "\r\n", 2);           /* echo the newline before any output */
	sh->line[sh->len] = '\0';

	enum cli_parse_status st =
		cli_parse(sh->line, sh->argv, CLI_ARGV_CAP, &sh->pr);

	switch (st) {
	case CLI_PARSE_OK: {
		/* Pass the handler-relative argv/argc (argv[0] = leaf name). */
		int ret = sh->pr.cmd->handler(sh, sh->pr.argc, sh->pr.argv);
		/* Force a non-zero result when output was dropped (TX timeout),
		 * even if the handler ignored cli_print's return value (req §11). */
		sh->last_result = (ret == 0 && sh->tx_failed) ? CLI_DISPATCH_ERR : ret;
		break;
	}
	case CLI_PARSE_EMPTY:
		break;                          /* blank line: just reprompt */
	case CLI_PARSE_NOT_FOUND:
		cli_error(sh, "%s: command not found\r\n", sh->pr.argv[0]);
		sh->last_result = CLI_DISPATCH_ERR;
		break;
	case CLI_PARSE_NO_HANDLER:
		cli_error(sh, "%s: missing or unknown subcommand\r\n", sh->pr.argv[0]);
		sh->last_result = CLI_DISPATCH_ERR;
		break;
	case CLI_PARSE_WRONG_ARGS:
		cli_error(sh, "%s: invalid number of arguments\r\n", sh->pr.argv[0]);
		sh->last_result = CLI_DISPATCH_ERR;
		break;
	case CLI_PARSE_TOO_MANY_TOKENS:
		cli_error(sh, "too many arguments\r\n");
		sh->last_result = CLI_DISPATCH_ERR;
		break;
	case CLI_PARSE_NESTING_TOO_DEEP:
		cli_error(sh, "command nesting too deep\r\n");
		sh->last_result = CLI_DISPATCH_ERR;
		break;
	case CLI_PARSE_UNTERMINATED_QUOTE:
		cli_error(sh, "unterminated quote\r\n");
		sh->last_result = CLI_DISPATCH_ERR;
		break;
	default:
		sh->last_result = CLI_DISPATCH_ERR;
		break;
	}

	/* Always return to the prompt; a bad command or a non-zero handler return
	 * never stops the shell, and one instance's error cannot reach another
	 * (fail-safe, req §9). */
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
		sh->tx_failed = 0;
		cli_write(sh, "^C\r\n", 4);
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
			cli_write(sh, "\b \b", 3);
		}
		return;
	}
	if (b >= 0x20 && b <= 0x7E) {           /* printable: append + echo */
		if (sh->len >= CLI_CMD_BUFFER_SIZE - 1) {
			char bel = 0x07;            /* line full (req §8): ring the bell */
			cli_write(sh, &bel, 1);
			return;
		}
		sh->line[sh->len++] = (char)b;
		sh->line[sh->len] = '\0';
		cli_write(sh, &b, 1);
		return;
	}
	/* Any other control byte: ignore. */
}
