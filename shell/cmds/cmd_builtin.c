/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_builtin.c
 * @brief   Minimal built-in shell commands for the #8 bring-up demo: help + echo.
 *
 * These are the only commands the `shell` application registers into
 * .shell_root_cmds; the richer builtins (version/uptime/reboot, thread, devmem)
 * arrive in #12-#14.  Both handlers touch only the shell instance passed to them
 * and write through the buffered output API, so they are reentrant when several
 * instances run the same command concurrently (req §10).
 *
 * This file is linked into the `shell` executable only -- never the host test
 * harness (shell/test) -- so the unit tests keep full control of their own
 * registered command set.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"

/*
 * help: list every registered root command (name + one-line help).  Walking the
 * linker section on target is itself the proof that .shell_root_cmds and its
 * boundary symbols resolved in the linked image (acceptance §18.1).
 */
static int cmd_help(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;

	cli_print(sh, "Commands:\r\n");
	CLI_ROOT_CMD_FOREACH(it)
		cli_print(sh, "  %-10s %s\r\n", it->name, it->help ? it->help : "");
	return 0;
}

/*
 * echo: print the rest of the line verbatim.  Registered CLI_ARG_RAW, so after
 * the command name the untokenized tail (quotes/escapes untouched, leading space
 * trimmed) arrives as a single argv[1]; with no tail, argc is 1.
 */
static int cmd_echo(struct cli_instance *sh, int argc, char **argv)
{
	return cli_print(sh, "%s\r\n", argc > 1 ? argv[1] : "");
}

CLI_CMD_REGISTER(help, NULL, "list commands", cmd_help, 1, 0);
CLI_CMD_REGISTER(echo, NULL, "echo the rest of the line", cmd_echo, 1, CLI_ARG_RAW);
