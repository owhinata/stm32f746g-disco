/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_gui.c
 * @brief   `gui` shell command: start/stop the GUIX demo UI (issue #55).
 *
 *   gui start   start (or resume) the GUIX UI -- takes over the LCD + FT5336
 *               touch.  Tap the on-screen button to switch screens.
 *   gui stop    stop the UI, blank the screen, hand the LCD back to `lcd`.
 *   gui info    GUIX state / system-thread priority / display / canvas.
 *
 * Phase 4 of #48 (#55): GUIX (Eclipse ThreadX GUIX v6.5.1) runs on its own
 * ThreadX thread, drawing through a 565rgb display driver bound to the LTDC
 * tear-free double buffer with DMA2D-accelerated fills, and takes pen input from
 * the FT5336 over I2C3.  Lazy-started so the `lcd`/`touch` test commands work
 * normally until the UI is wanted; while running, the `lcd` drawing commands are
 * refused (the display is owned by GUIX) -- run `gui stop` first.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "guix_glue.h"

static int cmd_gui_start(struct cli_instance *sh, int argc, char **argv)
{
	int rc;

	(void)argc;
	(void)argv;
	rc = guix_start();
	if (rc == GUIX_ERR_STATE) {
		cli_error(sh, "gui: display not initialized (is 'lcd' up?)\r\n");
		return 1;
	}
	if (rc != GUIX_OK) {
		cli_error(sh, "gui: failed to start GUIX\r\n");
		return 1;
	}
	cli_print(sh, "gui: running -- tap the on-screen button (gui stop to exit)\r\n");
	return 0;
}

static int cmd_gui_stop(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	(void)guix_stop();
	cli_print(sh, "gui: stopped (display returned to 'lcd')\r\n");
	return 0;
}

static int cmd_gui_info(struct cli_instance *sh, int argc, char **argv)
{
	struct guix_info gi;

	(void)argc;
	(void)argv;
	guix_get_info(&gi);
	cli_print(sh, "state:   %s\r\n",
	          gi.active ? "running" : (gi.inited ? "stopped" : "not started"));
	cli_print(sh, "touch:   %s\r\n", gi.touch ? "FT5336 (I2C3) up" : "down");
	cli_print(sh, "thread:  GUIX system thread priority %lu\r\n",
	          (unsigned long)gi.thread_prio);
	cli_print(sh, "display: handle %lu, 480x272 RGB565 (LTDC)\r\n",
	          (unsigned long)gi.display_handle);
	cli_print(sh, "canvas:  0x%08lx (LTDC back buffer)\r\n",
	          (unsigned long)gi.canvas_mem);
	return 0;
}

CLI_SUBCMD_SET_CREATE(gui_subcmds,
	CLI_CMD(start, NULL, "start/resume the GUIX UI (takes over LCD + touch)",
	        cmd_gui_start),
	CLI_CMD(stop, NULL, "stop the UI and hand the LCD back to 'lcd'",
	        cmd_gui_stop),
	CLI_CMD(info, NULL, "GUIX state / thread / display / canvas", cmd_gui_info),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(gui, gui_subcmds,
                 "Eclipse ThreadX GUIX demo UI (480x272, DMA2D + FT5336)",
                 NULL, 1, 0);
