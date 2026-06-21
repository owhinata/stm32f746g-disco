/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_touch.c
 * @brief   `touch` shell command: FT5336 capacitive touch status + read (#54).
 *
 *   touch probe   read the FT5336 chip ID (0x51 = present)
 *   touch info    bus / address / state / point count / mode
 *   touch read    poll the active touch points until Ctrl+C
 *
 * Phase of #48 after the LTDC display (#52): used to verify the touch wiring
 * and the X/Y mapping before EXTI-driven dispatch and GUIX input (#55/#56).
 * `touch read` polls every 100 ms and prints each active point's panel-pixel
 * coordinates (x 0..479, y 0..271; TS_SWAP_XY applied), event flag and ID tag.
 *
 * While the GUIX UI runs it owns the FT5336 (its own I2C3 poller), so `touch
 * probe`/`read` refuse until `gui stop` -- a concurrent shell poll would clash
 * on the bus and surface the FT5336's all-ones "not touched" sentinel (#73).
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "guix_glue.h"
#include "touch.h"

/* While the GUIX camera UI runs it owns FT5336 input -- its touch thread
   (port/guix/guix_touch.c) polls the same I2C3 at ~60 Hz.  A concurrent shell
   poll would be a second unsynchronized I2C3 master (touch_lock only serializes
   single transactions, not the TD_STATUS + per-point read sequence), so refuse
   the bus-touching touch commands while gui owns the input -- symmetric to the
   lcd draw guard (ltdc_gui_owns) and the camera CAM_ERR_BUSY gates (#73).  (The
   FT5336 not-touched sentinel itself is dropped in touch_read(); this guard is
   about bus ownership.)  `touch info` does no bus I/O and stays allowed. */
static int touch_gui_owned(struct cli_instance *sh)
{
	if (guix_is_up()) {
		cli_error(sh, "touch: input owned by gui; run 'gui stop' first\r\n");
		return 1;
	}
	return 0;
}

static int cmd_touch_probe(struct cli_instance *sh, int argc, char **argv)
{
	uint8_t id = 0;
	int rc;

	(void)argc;
	(void)argv;

	if (touch_gui_owned(sh))
		return 1;

	/* Bring the bus up lazily if init was skipped/failed at boot. */
	if (!touch_is_up() && touch_init() != 0) {
		cli_error(sh, "touch: I2C3 not initialized\r\n");
		return 1;
	}

	rc = touch_probe(&id);
	if (rc == TOUCH_OK) {
		cli_print(sh, "FT5336 detected: chip ID 0x%02x\r\n", id);
		return 0;
	}
	if (rc == TOUCH_ERR_ID)
		cli_error(sh, "touch: wrong chip ID 0x%02x (FT5336 absent?)\r\n",
		          id);
	else
		cli_error(sh, "touch: I2C read failed\r\n");
	return 1;
}

static int cmd_touch_info(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;

	cli_print(sh, "controller: FT5336 capacitive touch\r\n");
	cli_print(sh, "bus:        I2C3 (PH7=SCL/PH8=SDA, AF4)\r\n");
	cli_print(sh, "address:    0x70\r\n");
	cli_print(sh, "state:      %s\r\n", touch_is_up() ? "up" : "down");
	cli_print(sh, "max points: %u\r\n", (unsigned)TOUCH_MAX_POINTS);
	cli_print(sh, "mode:       interrupt (EXTI13 wake + I2C-IT)\r\n");
	return 0;
}

static int cmd_touch_read(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;

	if (touch_gui_owned(sh))
		return 1;

	if (!touch_is_up()) {
		cli_error(sh, "touch: I2C3 not initialized\r\n");
		return 1;
	}

	cli_print(sh, "polling FT5336 (Ctrl+C to stop) ...\r\n");
	while (!cli_cancel_requested(sh)) {
		struct touch_state st;

		if (touch_read(&st) != 0) {
			cli_error(sh, "touch: I2C read failed\r\n");
			return 1;
		}
		for (uint8_t i = 0; i < st.count; i++)
			cli_print(sh, "P%u: x=%u y=%u event=%u\r\n",
			          (unsigned)st.p[i].id,
			          (unsigned)st.p[i].x,
			          (unsigned)st.p[i].y,
			          (unsigned)st.p[i].event);
		/* cli_sleep ticks; 1 tick = 1 ms here -> 100 ms poll period. */
		if (cli_sleep(sh, 100) != 0)
			break;
	}
	return 0;
}

CLI_SUBCMD_SET_CREATE(touch_subcmds,
	CLI_CMD(probe, NULL, "read the FT5336 chip ID (0x51 = present)",
	        cmd_touch_probe),
	CLI_CMD(info, NULL, "bus / address / state / mode", cmd_touch_info),
	CLI_CMD(read, NULL, "poll active touch points until Ctrl+C",
	        cmd_touch_read),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(touch, touch_subcmds,
                 "FT5336 capacitive touch (I2C3)", NULL, 1, 0);
