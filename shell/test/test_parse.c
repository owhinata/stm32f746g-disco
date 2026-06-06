/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host unit test for the command-line parser (issue #3): tokenizer quote/escape
 * rules, static subcommand-tree search, and argc/argv validation including RAW.
 *
 * Compiled together with cli_parse.c and small CLI_MAX_ARGC / CLI_MAX_SUBCMD_DEPTH
 * overrides (see run_host_tests.sh) so the token-limit and nesting-limit paths
 * can be exercised with a compact command tree.  Uses the #2 host_sections.ld to
 * supply the .shell_root_cmds section.
 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "cli_internal.h"

static int h_ok(struct cli_instance *sh, int argc, char **argv)
{
	(void)sh;
	(void)argc;
	(void)argv;
	return 0;
}

/* deep -> a -> b -> c  (a, b are pure parents; c is a leaf) */
CLI_SUBCMD_SET_CREATE(sub_b, CLI_CMD_ARG(c, NULL, "leaf c", h_ok, 1, 0),
	CLI_SUBCMD_SET_END);
CLI_SUBCMD_SET_CREATE(sub_a, CLI_CMD_ARG(b, sub_b, "node b", NULL, 1, 0),
	CLI_SUBCMD_SET_END);
CLI_SUBCMD_SET_CREATE(sub_deep, CLI_CMD_ARG(a, sub_a, "node a", NULL, 1, 0),
	CLI_SUBCMD_SET_END);

/* thread { list, stacks }  (pure parent) */
CLI_SUBCMD_SET_CREATE(sub_thread,
	CLI_CMD_ARG(list,   NULL, "list",   h_ok, 1, 0),
	CLI_CMD_ARG(stacks, NULL, "stacks", h_ok, 1, 0),
	CLI_SUBCMD_SET_END);

/* mem { dump }  (parent that ALSO has its own handler) */
CLI_SUBCMD_SET_CREATE(sub_mem, CLI_CMD_ARG(dump, NULL, "dump", h_ok, 1, 2),
	CLI_SUBCMD_SET_END);

/* say now <raw...>  (RAW leaf under a parent, for handler-relative index test) */
CLI_SUBCMD_SET_CREATE(sub_say, CLI_CMD_ARG(now, NULL, "now raw", h_ok, 1, CLI_ARG_RAW),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(version, NULL,       "version",  h_ok, 1, 0);
CLI_CMD_REGISTER(thread,  sub_thread, "thread",   NULL, 1, 0);
CLI_CMD_REGISTER(mem,     sub_mem,    "mem",      h_ok, 1, 2);
CLI_CMD_REGISTER(echo,    NULL,       "echo raw", h_ok, 2, CLI_ARG_RAW);
CLI_CMD_REGISTER(deep,    sub_deep,   "deep",     NULL, 1, 0);
CLI_CMD_REGISTER(say,     sub_say,    "say",      NULL, 1, 0);

static char buf[CLI_CMD_BUFFER_SIZE];
static char *argv_store[CLI_ARGV_CAP];
static char *targv[CLI_MAX_ARGC + 1];
static struct cli_parse_result res;

static enum cli_parse_status run(const char *s)
{
	strncpy(buf, s, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	return cli_parse(buf, argv_store, CLI_ARGV_CAP, &res);
}

static int run_tok(const char *s)
{
	strncpy(buf, s, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	return cli_tokenize(buf, targv, CLI_MAX_ARGC);
}

static void test_tokenizer(void)
{
	assert(run_tok("a b c") == 3);
	assert(strcmp(targv[0], "a") == 0 && strcmp(targv[1], "b") == 0 &&
	       strcmp(targv[2], "c") == 0);
	assert(targv[3] == NULL);                 /* NULL sentinel */

	assert(run_tok("  a   b  ") == 2);        /* leading/trailing/multiple spaces */
	assert(strcmp(targv[0], "a") == 0 && strcmp(targv[1], "b") == 0);

	assert(run_tok("\"a b\" c") == 2);        /* double quote keeps space */
	assert(strcmp(targv[0], "a b") == 0 && strcmp(targv[1], "c") == 0);

	assert(run_tok("'a b' c") == 2);          /* single quote keeps space */
	assert(strcmp(targv[0], "a b") == 0);

	assert(run_tok("a\\ b") == 1);            /* escaped space -> one token */
	assert(strcmp(targv[0], "a b") == 0);

	assert(run_tok("\"a\\\"b\"") == 1);       /* escape inside double quotes */
	assert(strcmp(targv[0], "a\"b") == 0);

	assert(run_tok("\"\" x") == 2);           /* empty quoted token kept */
	assert(targv[0][0] == '\0' && strcmp(targv[1], "x") == 0);
	assert(run_tok("''") == 1 && targv[0][0] == '\0');

	assert(run_tok("a\\") == 1);              /* trailing backslash -> literal */
	assert(strcmp(targv[0], "a\\") == 0);

	assert(run_tok("\"abc") == -(int)CLI_PARSE_UNTERMINATED_QUOTE);
	assert(run_tok("'abc") == -(int)CLI_PARSE_UNTERMINATED_QUOTE);

	/* 9 tokens with CLI_MAX_ARGC == 8 -> too many */
	assert(run_tok("t1 t2 t3 t4 t5 t6 t7 t8 t9") ==
	       -(int)CLI_PARSE_TOO_MANY_TOKENS);
	assert(run_tok("t1 t2 t3 t4 t5 t6 t7 t8") == 8);   /* exactly the max is OK */
}

static void test_search(void)
{
	assert(run("") == CLI_PARSE_EMPTY);

	assert(run("version") == CLI_PARSE_OK);
	assert(strcmp(res.cmd->name, "version") == 0);
	assert(res.argc == 1 && strcmp(res.argv[0], "version") == 0);
	assert(res.cmd_level == 1 && res.argv[res.argc] == NULL);

	assert(run("nope") == CLI_PARSE_NOT_FOUND);
	assert(res.cmd == NULL && res.argc == 1 && strcmp(res.argv[0], "nope") == 0);

	/* multi-level descent strips the parent path: argv[0] is the leaf */
	assert(run("thread list") == CLI_PARSE_OK);
	assert(strcmp(res.cmd->name, "list") == 0);
	assert(res.cmd_level == 2 && res.argc == 1 && strcmp(res.argv[0], "list") == 0);

	/* pure parent invoked alone / with an unknown subcommand -> NO_HANDLER */
	assert(run("thread") == CLI_PARSE_NO_HANDLER);
	assert(strcmp(res.cmd->name, "thread") == 0 && res.argc == 1);
	assert(run("thread bogus") == CLI_PARSE_NO_HANDLER);
	assert(strcmp(res.cmd->name, "thread") == 0);
	assert(res.argc == 2 && strcmp(res.argv[1], "bogus") == 0);

	/* parent WITH a handler: a non-matching token becomes its argument */
	assert(run("mem foo") == CLI_PARSE_OK);
	assert(strcmp(res.cmd->name, "mem") == 0);
	assert(res.argc == 2 && strcmp(res.argv[0], "mem") == 0 &&
	       strcmp(res.argv[1], "foo") == 0);
	assert(run("mem dump") == CLI_PARSE_OK && strcmp(res.cmd->name, "dump") == 0);

	/* nesting: depth limit is 2 (root + 2 subcommand steps) */
	assert(run("deep a b") == CLI_PARSE_NO_HANDLER);   /* b is a pure parent */
	assert(strcmp(res.cmd->name, "b") == 0 && res.cmd_level == 3);
	assert(run("deep a b c") == CLI_PARSE_NESTING_TOO_DEEP);
	assert(res.cmd == NULL);                           /* error -> zeroed out */
}

static void test_validate(void)
{
	assert(run("mem dump x y") == CLI_PARSE_OK);       /* dump: mand 1, opt 2 */
	assert(res.argc == 3 && strcmp(res.argv[0], "dump") == 0);
	assert(run("mem dump x y z") == CLI_PARSE_WRONG_ARGS);
	assert(strcmp(res.cmd->name, "dump") == 0);        /* out populated on error */
	assert(run("version x") == CLI_PARSE_WRONG_ARGS);

	/* token limit (CLI_MAX_ARGC == 8) fires before arg-count validation */
	assert(run("version 1 2 3 4 5 6 7 8 9") == CLI_PARSE_TOO_MANY_TOKENS);
}

static void test_raw(void)
{
	/* echo: mandatory 2 (echo + 1 token), then the rest verbatim */
	assert(run("echo hello world foo") == CLI_PARSE_OK);
	assert(strcmp(res.cmd->name, "echo") == 0 && res.argc == 3);
	assert(strcmp(res.argv[0], "echo") == 0 && strcmp(res.argv[1], "hello") == 0);
	assert(strcmp(res.argv[2], "world foo") == 0);     /* tail keeps the space */

	/* tail is verbatim: quotes and repeated spaces preserved */
	assert(run("echo k \"a b\"  c") == CLI_PARSE_OK);
	assert(strcmp(res.argv[2], "\"a b\"  c") == 0);

	/* empty tail -> no extra arg, just the mandatory tokens */
	assert(run("echo hello") == CLI_PARSE_OK && res.argc == 2);

	/* too few mandatory tokens -> WRONG_ARGS */
	assert(run("echo") == CLI_PARSE_WRONG_ARGS);

	/* RAW leaf under a parent: handler-relative index, argv[0] is the leaf */
	assert(run("say now hello there") == CLI_PARSE_OK);
	assert(strcmp(res.cmd->name, "now") == 0 && res.cmd_level == 2);
	assert(res.argc == 2 && strcmp(res.argv[0], "now") == 0);
	assert(strcmp(res.argv[1], "hello there") == 0);
}

int main(void)
{
	test_tokenizer();
	test_search();
	test_validate();
	test_raw();
	printf("OK: tokenizer / tree search / validation / RAW all pass\n");
	return 0;
}
