/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_camera.c
 * @brief   `camera` shell command: B-CAMS-OMV / OV5640 control (#39/#41).
 *
 *   camera probe          power-cycle the module and read the OV5640 chip ID
 *   camera info           driver / sensor state
 *   camera capture [test] snapshot one QVGA RGB565 frame (test = colorbar)
 *   camera off            cut module power
 *
 * `capture` prints per-channel min/max/mean statistics plus a first-pixels
 * hexdump so a frame can be sanity-checked without any display: the colorbar
 * pattern gives known flat bands (DCMI polarity/timing check independent of
 * optics), and covering the lens must move the live mean.  `camera save`
 * (file export) is issue #42.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "camera.h"

#include <string.h>

static const char *cam_strerror(int rc)
{
	switch (rc) {
	case CAM_ERR_PARAM:     return "bad argument";
	case CAM_ERR_HAL:       return "I2C/sensor I/O failed";
	case CAM_ERR_TIMEOUT:   return "operation timed out";
	case CAM_ERR_STATE:     return "driver not initialized / SDRAM down";
	case CAM_ERR_NO_SENSOR: return "OV5640 not detected (module connected?)";
	case CAM_ERR_NO_FRAME:  return "no captured frame";
	default:                return "unknown error";
	}
}

static int cmd_camera_probe(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t id = 0;
	int rc;

	(void)argc;
	(void)argv;

	/* The probe power-cycles and soft-resets the sensor; the ID read alone
	   sits ~600 ms behind those settle times. */
	cli_print(sh, "camera: probing OV5640 (takes ~1s) ...\r\n");
	rc = camera_probe(&id);
	if (rc != 0) {
		cli_error(sh, "camera: %s\r\n", cam_strerror(rc));
		return 1;
	}
	cli_print(sh, "OV5640 detected: chip ID 0x%04lx\r\n", (unsigned long)id);
	return 0;
}

static int cmd_camera_info(struct cli_instance *sh, int argc, char **argv)
{
	struct camera_info ci;
	int rc;

	(void)argc;
	(void)argv;

	rc = camera_get_info(&ci);
	if (rc != 0) {
		cli_error(sh, "camera: %s\r\n", cam_strerror(rc));
		return 1;
	}

	cli_print(sh, "module:     B-CAMS-OMV (OV5640) on P1/DCMI, I2C1 @0x78\r\n");
	cli_print(sh, "power:      %s\r\n", ci.powered ? "on" : "off");
	cli_print(sh, "chip ID:    ");
	if (ci.chip_id != 0)
		cli_print(sh, "0x%04lx\r\n", (unsigned long)ci.chip_id);
	else
		cli_print(sh, "- (not probed)\r\n");
	cli_print(sh, "configured: %s\r\n", ci.configured ? "yes" : "no");
	cli_print(sh, "frame:      %s\r\n", ci.frame_valid ? "valid" : "none");
	return 0;
}

/*
 * Per-channel statistics over the captured frame, computed row by row through
 * camera_frame_read (the driver mutex serializes against a concurrent
 * capture).  Sums fit comfortably: 76800 px * max 63 < 2^23.
 */
static int cmd_camera_capture(struct cli_instance *sh, int argc, char **argv)
{
	uint16_t row[CAMERA_FRAME_WIDTH];
	uint32_t rmin = 31, rmax = 0, rsum = 0;
	uint32_t gmin = 63, gmax = 0, gsum = 0;
	uint32_t bmin = 31, bmax = 0, bsum = 0;
	uint32_t npx = CAMERA_FRAME_WIDTH * CAMERA_FRAME_HEIGHT;
	int colorbar = 0;
	int rc;

	if (argc > 1) {
		if (strcmp(argv[1], "test") != 0) {
			cli_error(sh, "camera: unknown option '%s' (try: test)\r\n",
			          argv[1]);
			return 1;
		}
		colorbar = 1;
	}

	cli_print(sh, "camera: capturing %s frame ...\r\n",
	          colorbar ? "colorbar test" : "live");
	rc = camera_capture(colorbar);
	if (rc != 0) {
		cli_error(sh, "camera: %s\r\n", cam_strerror(rc));
		return 1;
	}

	for (uint32_t y = 0; y < CAMERA_FRAME_HEIGHT; y++) {
		rc = camera_frame_read(y * sizeof row, row, sizeof row);
		if (rc != 0) {
			cli_error(sh, "camera: %s\r\n", cam_strerror(rc));
			return 1;
		}
		for (uint32_t x = 0; x < CAMERA_FRAME_WIDTH; x++) {
			uint32_t p = row[x];
			uint32_t r = (p >> 11) & 0x1Fu;
			uint32_t gg = (p >> 5) & 0x3Fu;
			uint32_t b = p & 0x1Fu;

			if (r < rmin) rmin = r;
			if (r > rmax) rmax = r;
			rsum += r;
			if (gg < gmin) gmin = gg;
			if (gg > gmax) gmax = gg;
			gsum += gg;
			if (b < bmin) bmin = b;
			if (b > bmax) bmax = b;
			bsum += b;
		}
		if (cli_cancel_requested(sh))
			return 1;
	}

	cli_print(sh, "frame: %ux%u RGB565 (%lu bytes)\r\n",
	          (unsigned)CAMERA_FRAME_WIDTH, (unsigned)CAMERA_FRAME_HEIGHT,
	          (unsigned long)CAMERA_FRAME_BYTES);
	cli_print(sh, "R5: min %2lu  max %2lu  mean %lu.%lu\r\n",
	          (unsigned long)rmin, (unsigned long)rmax,
	          (unsigned long)(rsum / npx),
	          (unsigned long)((rsum % npx) * 10u / npx));
	cli_print(sh, "G6: min %2lu  max %2lu  mean %lu.%lu\r\n",
	          (unsigned long)gmin, (unsigned long)gmax,
	          (unsigned long)(gsum / npx),
	          (unsigned long)((gsum % npx) * 10u / npx));
	cli_print(sh, "B5: min %2lu  max %2lu  mean %lu.%lu\r\n",
	          (unsigned long)bmin, (unsigned long)bmax,
	          (unsigned long)(bsum / npx),
	          (unsigned long)((bsum % npx) * 10u / npx));

	/* First 16 pixels of the top row, raw little-endian. */
	if (camera_frame_read(0, row, 32) == 0) {
		cli_print(sh, "row0[0..15]:\r\n");
		cli_hexdump(sh, row, 32);
	}
	return 0;
}

static int cmd_camera_off(struct cli_instance *sh, int argc, char **argv)
{
	int rc;

	(void)argc;
	(void)argv;

	rc = camera_power_off();
	if (rc != 0) {
		cli_error(sh, "camera: %s\r\n", cam_strerror(rc));
		return 1;
	}
	cli_print(sh, "camera: power off\r\n");
	return 0;
}

CLI_SUBCMD_SET_CREATE(camera_subcmds,
	CLI_CMD(probe, NULL, "power-cycle + read the OV5640 chip ID", cmd_camera_probe),
	CLI_CMD(info,  NULL, "driver / sensor state", cmd_camera_info),
	CLI_CMD_ARG(capture, NULL, "snapshot QVGA RGB565 + stats ('test' = colorbar)",
	            cmd_camera_capture, 1, 1),
	CLI_CMD(off,   NULL, "cut camera module power", cmd_camera_off),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(camera, camera_subcmds, "B-CAMS-OMV camera (OV5640 on DCMI)",
                 NULL, 1, 0);
