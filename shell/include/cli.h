/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cli.h
 * @brief   Shell (CLI) public API -- command registration foundation.
 *
 * Commands are registered declaratively from any translation unit with
 * CLI_CMD_REGISTER(); each root command lands as a `struct cli_cmd` in the
 * dedicated linker section `.shell_root_cmds`, between the boundary symbols
 * __cli_root_cmds_start / __cli_root_cmds_end.  Subcommands form a static tree
 * of `struct cli_cmd[]` arrays terminated by a CLI_SUBCMD_SET_END sentinel.
 * Root and subcommand entries share the same descriptor type and are read-only
 * data shared by every shell instance.
 *
 * This header is a clean-room design; it borrows the *concept* of an iterable
 * command section from Zephyr's shell but reuses none of its code.
 *
 * Scope of this file currently covers command registration only.  Output API
 * (cli_print/...), instance definition and the full `struct cli_instance` are
 * added by later issues; here cli_instance is only forward-declared so handler
 * pointers type-check.
 */
#ifndef CLI_H
#define CLI_H

#include <stddef.h>   /* size_t */
#include <stdint.h>   /* uint8_t */

#include "cli_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque shell instance; full definition comes with the core (issue #4). */
struct cli_instance;

/**
 * Command handler.  Receives the owning shell instance and the parsed argument
 * vector (argv[0] is the command name).  Returns 0 on success; non-zero marks
 * an error.  argc/argv validation (mandatory/optional) runs before the handler;
 * if it fails the handler is not called.
 */
typedef int (*cli_cmd_handler_t)(struct cli_instance *sh, int argc, char **argv);

/**
 * One command descriptor -- used for both root commands (placed in
 * .shell_root_cmds) and subcommand-set entries.
 */
struct cli_cmd {
	const char           *name;     /**< command name; NULL marks a sentinel */
	const char           *help;     /**< one-line help text */
	const struct cli_cmd *subcmds;  /**< sentinel-terminated subcommand array, or NULL */
	cli_cmd_handler_t     handler;  /**< handler, or NULL for a pure parent command */
	uint8_t               mandatory;/**< required argc (command name included) */
	uint8_t               optional; /**< number of optional arguments allowed */
};

/*
 * Section attribute for a root command entry.  `used` + the linker's KEEP keep
 * the entry through -Wl,--gc-sections; `aligned(__alignof__(struct cli_cmd))`
 * pins the input-section alignment to the type's natural alignment so the
 * linker packs entries back-to-back with no padding, which is what lets the
 * section be walked as a plain array via pointer arithmetic.
 */
#define CLI_SECTION_ATTR(nm) \
	__attribute__((used, section(".shell_root_cmds." nm), \
	               aligned(__alignof__(struct cli_cmd))))

/**
 * Register a root command into the .shell_root_cmds section.
 *
 * @param name       command name as a bare C identifier (stringified; also used
 *                   to build a unique symbol/section name)
 * @param subcmds    pointer to a subcommand set (CLI_SUBCMD_SET_CREATE) or NULL
 * @param help       one-line help string
 * @param handler    cli_cmd_handler_t, or NULL for a pure parent command
 * @param mandatory  required argc including the command name
 * @param optional   number of optional arguments allowed
 *
 * Note: `name` must be a valid C identifier, so names with dashes (e.g.
 * "foo-bar") cannot be registered this way today.  Every standard-tier command
 * (version/uptime/reboot/thread/devmem/help/clear/history/backends) is a valid
 * identifier.  Detection of duplicate root names across translation units is
 * left to the parser (issue #3).
 */
/* Parameters are underscore-prefixed so a `name`/`help`/... parameter does not
 * collide with the identically named designated-initializer field below (the
 * preprocessor would otherwise rewrite `.name` into `.<arg>`). */
#define CLI_CMD_REGISTER(_name, _subcmds, _help, _handler, _mandatory, _optional) \
	static const struct cli_cmd __cli_cmd_##_name CLI_SECTION_ATTR(#_name) = { \
		.name      = #_name, \
		.help      = (_help), \
		.subcmds   = (_subcmds), \
		.handler   = (_handler), \
		.mandatory = (_mandatory), \
		.optional  = (_optional), \
	}

/**
 * Create a static subcommand set: a `struct cli_cmd[]` initialised with the
 * given CLI_CMD / CLI_CMD_ARG entries and terminated by CLI_SUBCMD_SET_END.
 */
#define CLI_SUBCMD_SET_CREATE(_set_name, ...) \
	static const struct cli_cmd _set_name[] = { __VA_ARGS__ }

/** A subcommand-set entry with explicit mandatory/optional argument counts. */
#define CLI_CMD_ARG(_name, _subcmds, _help, _handler, _mandatory, _optional) \
	{ .name = #_name, .help = (_help), .subcmds = (_subcmds), .handler = (_handler), \
	  .mandatory = (_mandatory), .optional = (_optional) }

/** A subcommand-set entry with the default argument counts (mandatory=1, optional=0). */
#define CLI_CMD(_name, _subcmds, _help, _handler) \
	CLI_CMD_ARG(_name, _subcmds, _help, _handler, 1, 0)

/** Sentinel that terminates a subcommand set. */
#define CLI_SUBCMD_SET_END { .name = NULL }

/* Boundary symbols provided by the linker script (PROVIDE_HIDDEN). */
extern const struct cli_cmd __cli_root_cmds_start[];
extern const struct cli_cmd __cli_root_cmds_end[];

/** Iterate every registered root command in .shell_root_cmds order. */
#define CLI_ROOT_CMD_FOREACH(it) \
	for (const struct cli_cmd *it = __cli_root_cmds_start; \
	     it < __cli_root_cmds_end; ++it)

/** Number of registered root commands. */
static inline size_t cli_root_cmd_count(void)
{
	return (size_t)(__cli_root_cmds_end - __cli_root_cmds_start);
}

#ifdef __cplusplus
}
#endif

#endif /* CLI_H */
