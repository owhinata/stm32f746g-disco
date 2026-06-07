/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cli_history.c
 * @brief   Command history hooks -- no-op stubs (issue #9 seam for #10).
 *
 * The line editor (cli_edit.c) already routes the up/down keys (ESC[A / ESC[B,
 * Ctrl+p / Ctrl+n) to cli_history_prev/next(), and the dispatcher records each
 * submitted line through cli_history_add().  Issue #9 ships these as no-ops so
 * the editor is complete on its own; issue #10 implements the fixed history ring
 * (req §8: 512 B, oldest-first FIFO, consecutive-duplicate suppression) by
 * replacing the bodies in THIS file only -- the call sites and the
 * `struct cli_instance` contract do not change.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli_instance.h"
#include "cli_internal.h"

void cli_history_prev(struct cli_instance *sh)
{
	(void)sh;   /* #10: recall the older entry into the line buffer + refresh */
}

void cli_history_next(struct cli_instance *sh)
{
	(void)sh;   /* #10: recall the newer entry (or restore the draft) + refresh */
}

void cli_history_add(struct cli_instance *sh, const char *line)
{
	(void)sh;
	(void)line; /* #10: push the submitted line, FIFO-evicting the oldest */
}
