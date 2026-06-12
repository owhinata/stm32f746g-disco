/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_camera.c
 * @brief   `camera` shell command: B-CAMS-OMV / OV5640 control (issue #39).
 *
 *   camera probe   power-cycle the module and read the OV5640 chip ID
 *   camera info    driver / sensor state
 *   camera off     cut module power
 *
 * Phase 1 of Epic #22 -- sensor detection only.  `camera capture` (DCMI+DMA,
 * issue #41) and `camera save` (issue #42) come later.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "camera.h"

static const char *cam_strerror(int rc)
{
	switch (rc) {
	case CAM_ERR_PARAM:     return "bad argument";
	case CAM_ERR_HAL:       return "I2C/sensor I/O failed";
	case CAM_ERR_TIMEOUT:   return "operation timed out";
	case CAM_ERR_STATE:     return "driver not initialized";
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
	CLI_CMD(off,   NULL, "cut camera module power", cmd_camera_off),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(camera, camera_subcmds, "B-CAMS-OMV camera (OV5640 on DCMI)",
                 NULL, 1, 0);
