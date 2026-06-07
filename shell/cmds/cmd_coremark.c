/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_coremark.c
 * @brief   `coremark` built-in shell command: run the EEMBC CoreMark benchmark.
 *
 * Replaces the former standalone bare-metal CoreMark image.  The benchmark is
 * built once into the coremark_obj OBJECT library (the lib/coremark sources plus
 * the port in port/coremark, compiled at -O3 -funroll-loops with
 * MEM_METHOD=MEM_STATIC) and linked into the threadx firmware; this handler just
 * calls into it.
 *
 * core_main.c is compiled with -Dmain=coremark_main (see CMakeLists.txt) so the
 * benchmark entry does not collide with the firmware main() -- hence the local
 * declaration below.  It runs synchronously in the calling shell instance thread
 * (~12 s while CoreMark auto-calibrates), so the prompt is blocked until it
 * finishes; the LD1 heartbeat thread (higher priority) keeps blinking.  The data
 * block is static (MEM_STATIC), so it lives in .bss, not on this thread's stack.
 *
 * Output: CoreMark prints its canonical report via ee_printf -> printf, which the
 * UART backend's strong _write routes to the same VCP as cli_print.  Because the
 * benchmark runs in this single thread, its printf and the shell's cli_print
 * never interleave; the timed region itself does no I/O, so TX back-pressure does
 * not perturb the score.  Registered into the `shell`/threadx executable only
 * (never the host test harness), like cmd_system.c / cmd_thread.c / cmd_devmem.c.
 *
 * Not a dangerous command (read-only CPU benchmark), so it is not gated behind
 * CLI_ENABLE_DANGEROUS_CMDS.
 *
 * Clean-room glue; the CoreMark sources themselves are EEMBC's (Apache-2.0).
 */
#include "cli.h"

/* CoreMark entry, renamed from main() by -Dmain=coremark_main on core_main.c. */
int coremark_main(void);

static int cmd_coremark(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;

	cli_info(sh, "Running CoreMark (auto-calibrated, ~12s)...\r\n");
	coremark_main();   /* prints the canonical CoreMark report via printf -> VCP */
	return 0;
}

CLI_CMD_REGISTER(coremark, NULL, "run the EEMBC CoreMark benchmark (~12s)",
                 cmd_coremark, 1, 0);
