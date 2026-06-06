/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cli_internal.h
 * @brief   Shell core-internal API (not part of the public cli.h surface).
 *
 * Currently holds the command-line parser: tokenizer, static subcommand-tree
 * search and argc/argv validation (issue #3).  The parser is a pure function of
 * the input line plus the registered command tree -- it does not touch a
 * `struct cli_instance` and never invokes a handler.  It resolves the command
 * and the handler-relative argc/argv and reports a status; the core (#4) does
 * the actual dispatch and message printing.
 */
#ifndef CLI_INTERNAL_H
#define CLI_INTERNAL_H

#include "cli.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minimum capacity of the argv array the core (#4) must provide to cli_parse():
 * up to CLI_MAX_ARGC normal tokens, plus one RAW tail argument, plus the NULL
 * sentinel written at argv[argc].
 */
#define CLI_ARGV_CAP (CLI_MAX_ARGC + 2)

/** Outcome of cli_parse(). */
enum cli_parse_status {
	CLI_PARSE_OK = 0,            /**< cmd has a handler and args validate: dispatch it */
	CLI_PARSE_EMPTY,             /**< blank line (no tokens) */
	CLI_PARSE_NOT_FOUND,         /**< argv[0] matched no root command */
	CLI_PARSE_NO_HANDLER,        /**< resolved a parent that has no handler */
	CLI_PARSE_WRONG_ARGS,        /**< argc outside [mandatory, mandatory+optional] */
	CLI_PARSE_TOO_MANY_TOKENS,   /**< more than CLI_MAX_ARGC tokens */
	CLI_PARSE_NESTING_TOO_DEEP,  /**< subcommand descent exceeded CLI_MAX_SUBCMD_DEPTH */
	CLI_PARSE_UNTERMINATED_QUOTE,/**< a quote was opened but never closed */
};

/** Result of cli_parse(); see the per-status contract below. */
struct cli_parse_result {
	const struct cli_cmd *cmd;   /**< resolved command (leaf or parent), or NULL */
	char                **argv;  /**< handler argv (into caller array; argv[0]=leaf name, argv[argc]=NULL) */
	int                   argc;  /**< handler argc */
	int                   cmd_level; /**< number of command-path tokens (root included) */
};

/**
 * Tokenize @p line in place into @p argv (RAW handling is NOT applied; this is
 * the bare splitter, exposed mainly for unit tests).  Quote (`"`/`'`) and
 * backslash escapes are processed; empty quoted tokens ("" / '') are kept as
 * empty-string arguments.  At most @p max_argc tokens are stored and, on
 * success, argv[argc] is set to NULL -- so @p argv must hold max_argc+1 slots.
 *
 * @return the token count (>=0), or a negative value whose magnitude is the
 *         offending enum cli_parse_status (UNTERMINATED_QUOTE / TOO_MANY_TOKENS).
 */
int cli_tokenize(char *line, char **argv, int max_argc);

/**
 * Parse @p line: tokenize, walk the static command tree, handle RAW, and
 * validate the argument count.  @p argv is caller-provided scratch of capacity
 * @p argv_cap (must be >= CLI_ARGV_CAP).  @p out is zero-initialised first; on
 * return it is populated per the status (see cli_parse.c).
 */
enum cli_parse_status cli_parse(char *line, char **argv, int argv_cap,
                                struct cli_parse_result *out);

#ifdef __cplusplus
}
#endif

#endif /* CLI_INTERNAL_H */
