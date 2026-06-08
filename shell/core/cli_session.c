/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cli_session.c
 * @brief   Shell line pipeline: prompt + line dispatch.
 *
 * Pure dispatch logic over a `struct cli_instance`: it submits the accumulated
 * line to the parser, prints the outcome and returns to the prompt.  The byte
 * input / line-editing state machine that feeds the buffer lives in cli_edit.c
 * (issue #9); both files call no ThreadX (tx_*) API -- the thread loop, object
 * creation and ISR notify live in cli_core.c -- so they compile and unit-test on
 * the host against a small tx_api.h type shim (shell/test/shim).  All output
 * goes through the buffered output API (cli_write/cli_error, issue #5): echo,
 * prompt and dispatch messages are flow-controlled and (for errors) coloured.
 * Clean-room design inspired by Zephyr shell; no code reused.
 */
#include <stddef.h>
#include <string.h>

#include "cli_instance.h"
#include "cli_internal.h"

void cli_prompt(struct cli_instance *sh)
{
	cli_write(sh, sh->prompt, strlen(sh->prompt));
}

int cli_cancel_poll(struct cli_instance *sh)
{
	struct cli_transport *tr = sh->tr;
	uint8_t buf[CLI_RX_DRAIN_CHUNK];
	int n;

	/* Drain everything currently buffered; a 0x03 anywhere latches the cancel.
	 * Non-0x03 bytes are discarded -- type-ahead typed during a running command
	 * is dropped by design (issue #16). */
	while ((n = tr->api->read(tr, buf, sizeof buf)) > 0)
		for (int i = 0; i < n; i++)
			if (buf[i] == 0x03)
				sh->cancel_req = 1;

	return sh->cancel_req;
}

bool cli_cancel_requested(struct cli_instance *sh)
{
	return cli_cancel_poll(sh) != 0;
}

void cli_dispatch_line(struct cli_instance *sh)
{
	sh->tx_failed = 0;                  /* fresh flow-control state per command */
	cli_write(sh, "\r\n", 2);           /* echo the newline before any output */
	sh->line[sh->len] = '\0';

	/* Record the submitted line for history (seam for #10; no-op in #9).  Done
	 * BEFORE cli_parse, which tokenizes sh->line in place. */
	if (sh->len > 0)
		cli_history_add(sh, sh->line);

	enum cli_parse_status st =
		cli_parse(sh->line, sh->argv, CLI_ARGV_CAP, &sh->pr);

	switch (st) {
	case CLI_PARSE_OK: {
		/* Pass the handler-relative argv/argc (argv[0] = leaf name).  Mark the
		 * window in which a Ctrl+C cancels the command (issue #16): cancel_req is
		 * cleared first, dispatching gates the cooperative-cancel paths. */
		sh->cancel_req  = 0;
		sh->dispatching = 1;
		int ret = sh->pr.cmd->handler(sh, sh->pr.argc, sh->pr.argv);
		sh->dispatching = 0;
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

	/* Cooperative Ctrl+C (issue #16): if the command observed a cancel, echo the
	 * editor-style "^C" feedback.  Clear cancel_req + tx_failed + the staging
	 * buffer FIRST (mirrors cli_edit.c's 0x03 path) so the feedback is not eaten
	 * by the cancel fast-fail or the tx_failed output-drop; drop any residual RX
	 * with a raw drain (NOT cli_cancel_poll, which would re-latch on the 0x03) so
	 * queued type-ahead does not auto-run. */
	if (sh->cancel_req) {
		uint8_t b;
		sh->cancel_req = 0;
		sh->tx_failed  = 0;
		sh->out_len    = 0;
		while (sh->tr->api->read(sh->tr, &b, 1) > 0)
			;                          /* discard residual RX */
		cli_write(sh, "^C\r\n", 4);
		sh->last_result = CLI_DISPATCH_CANCELLED;
	}

	/* Always return to the prompt; a bad command or a non-zero handler return
	 * never stops the shell, and one instance's error cannot reach another
	 * (fail-safe, req §9).  Reset the editor's cursor + render state so the
	 * fresh prompt is the new render baseline (issue #9); overwrite mode is
	 * intentionally kept across commands (session-wide, like the terminal). */
	sh->len = 0;
	sh->cur = 0;
	sh->line[0] = '\0';
	sh->rx = CLI_RX_NORMAL;
	/* Leave history navigation unconditionally: cli_history_add only runs for a
	 * non-empty line, so a recalled line cleared to empty (or a blank submit)
	 * would otherwise leave hist_nav_on set and the next ↑ would resume from a
	 * stale offset instead of the newest entry (issue #10). */
	sh->hist_nav_on = 0;
	sh->hist_nav = 0;
	{
		unsigned w = sh->term_width ? sh->term_width : (unsigned)CLI_TERM_WIDTH;
		sh->old_rows = (uint8_t)((unsigned)strlen(sh->prompt) / w + 1);
		sh->draw_row = 0;
	}
	cli_prompt(sh);
}
