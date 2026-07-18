/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_gui.c
 * @brief   `gui` shell command: start/stop the GUIX camera UI (issues #55/#61).
 *
 *   gui start   start (or resume) the GUIX camera UI -- takes over the LCD +
 *               FT5336 touch and subscribes the live camera preview.
 *   gui stop    stop the preview, blank the screen, hand the LCD back to
 *               `lcd` (so the `lcd`/`touch`/`camera` test commands can run).
 *   gui info    GUIX state / system-thread priority / display / canvas.
 *
 * Since Epic #99 Phase 1 (#100) the GUI preview is a *subscriber* of the base
 * capture (`camera stream`), not its owner: `gui start` shows the window and
 * subscribes the preview (it follows the base -- frozen while the base is off,
 * live while it streams RGB565); `gui stop` unsubscribes without stopping the base
 * or the AI.  Face-detect boxes are drawn whenever `ai stream` is running (the old
 * `gui overlay` sub-command is gone).  #68 added a settings page reached by tapping
 * the live image (image-quality controls + Back).  The UI is started ON at boot
 * (#60), which also brings the base capture up once so the preview is live out of
 * the box; while it runs the `lcd` drawing commands are refused (the display is
 * owned by GUIX) -- run `gui stop` first.  The camera UI itself lives in
 * ui/guix_camera_ui.c (presentation layer).
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "camera.h"          /* camera_streaming / camera_get_mode (#97 preview state) */
#include "guix_glue.h"
#include "guix_camera_ui.h"

#include <stdbool.h>
#include <string.h>

static int cmd_gui_start(struct cli_instance *sh, int argc, char **argv)
{
	int rc;

	(void)argc;
	(void)argv;
	rc = camera_ui_start();
	if (rc == GUIX_ERR_STATE) {
		cli_error(sh, "gui: display not initialized (is 'lcd' up?)\r\n");
		return 1;
	}
	if (rc != GUIX_OK) {
		cli_error(sh, "gui: failed to start GUIX\r\n");
		return 1;
	}
	/* #97/#101: the preview attach is deferred to the GUIX thread, so report the
	   preview state from the base capture the window will follow -- never claim
	   "live preview" when the base is off (frozen) or JPEG (no raster to render).
	   The preview goes live the moment an RGB565 `camera stream` runs. */
	{
		struct camera_mode m;

		if (!camera_streaming())
			cli_print(sh, "gui: running -- preview idle; no camera capture "
			          "(run `camera stream start`)\r\n");
		else if (camera_get_mode(&m) == 0 && m.is_jpeg)
			cli_print(sh, "gui: running -- preview unavailable; camera stream "
			          "is JPEG (needs RGB565)\r\n");
		else if (camera_get_mode(&m) == 0 && m.format != CAM_FMT_RGB565)
			cli_print(sh, "gui: running -- preview unavailable; camera stream "
			          "is not RGB565\r\n");
		else
			cli_print(sh, "gui: running -- live camera preview "
			          "(gui stop to exit)\r\n");
	}
	return 0;
}

static int cmd_gui_stop(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	(void)camera_ui_stop();
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
	CLI_CMD(start, NULL, "subscribe the GUIX camera preview (takes over LCD + touch)",
	        cmd_gui_start),
	CLI_CMD(stop, NULL, "stop the preview and hand the LCD back to 'lcd'",
	        cmd_gui_stop),
	CLI_CMD(info, NULL, "GUIX state / thread / display / canvas", cmd_gui_info),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(gui, gui_subcmds,
                 "Eclipse ThreadX GUIX camera UI (480x272, DMA2D + FT5336)",
                 NULL, 1, 0);
