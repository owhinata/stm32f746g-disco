/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_camera.c
 * @brief   `camera` shell command: B-CAMS-OMV / OV5640 control (#39/#41/#42).
 *
 *   camera probe            power-cycle the module and read the OV5640 chip ID
 *   camera info             driver / sensor state
 *   camera capture [test]   snapshot one QVGA RGB565 frame (test = colorbar)
 *   camera save <sd|fs> <p> write the captured frame to a file, raw RGB565
 *   camera off              cut module power
 *
 * `capture` prints per-channel min/max/mean statistics plus a first-pixels
 * hexdump so a frame can be sanity-checked without any display: the colorbar
 * pattern gives known flat bands (DCMI polarity/timing check independent of
 * optics), and covering the lens must move the live mean.
 *
 * `save` streams the frame row by row through camera_frame_read into a FileX
 * file on the chosen medium (sd = microSD FAT32, fs = QSPI NOR), raw
 * little-endian RGB565 (153600 B) -- convert on the PC with
 * scripts/rgb565_to_png.py.  It reuses the fs/sd commands' fs_device gates
 * (shared op slot), so format/umount cannot yank the media mid-save.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "camera.h"
#include "fs_cmd_core.h"

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
	uint32_t gen0 = 0, gen;
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
		rc = camera_frame_read(y * sizeof row, row, sizeof row, &gen);
		if (rc != 0) {
			cli_error(sh, "camera: %s\r\n", cam_strerror(rc));
			return 1;
		}
		if (y == 0)
			gen0 = gen;
		else if (gen != gen0) {
			cli_error(sh, "camera: frame changed mid-read "
			              "(concurrent capture)\r\n");
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

	/* First 16 pixels of the top row, raw little-endian.  Only when still
	   the same frame the stats above were computed over. */
	if (camera_frame_read(0, row, 32, &gen) == 0 && gen == gen0) {
		cli_print(sh, "row0[0..15]:\r\n");
		cli_hexdump(sh, row, 32);
	}
	return 0;
}

/*
 * Write the captured frame to <path> on the chosen medium, raw little-endian
 * RGB565, streamed row by row (640 B) so no full-frame staging buffer is
 * needed.  Runs under the medium's shared op slot (same gate as the fs/sd
 * command bodies); the camera driver mutex inside camera_frame_read
 * serializes each row against a concurrent capture.
 */
static int save_body(const struct fs_device *dev, struct cli_instance *sh,
                     const char *path)
{
	uint8_t row[CAMERA_FRAME_WIDTH * 2];
	FX_MEDIA *media = fs_core_mount(dev, sh);
	FX_FILE file;
	uint32_t gen0 = 0, gen;
	UINT status;
	int rc;

	if (media == NULL)
		return 1;

	status = fx_file_create(media, (CHAR *)path);
	if (status != FX_SUCCESS && status != FX_ALREADY_CREATED) {
		cli_error(sh, "camera: create %s: %s (0x%02x)\r\n",
		          path, fs_strerror(status), status);
		return 1;
	}
	status = fx_file_open(media, &file, (CHAR *)path, FX_OPEN_FOR_WRITE);
	if (status != FX_SUCCESS) {
		cli_error(sh, "camera: open %s: %s (0x%02x)\r\n",
		          path, fs_strerror(status), status);
		return 1;
	}

	status = fx_file_truncate(&file, 0);
	for (uint32_t y = 0; status == FX_SUCCESS && y < CAMERA_FRAME_HEIGHT;
	     y++) {
		rc = camera_frame_read(y * sizeof row, row, sizeof row, &gen);
		if (rc != 0) {
			(void)fx_file_close(&file);
			cli_error(sh, "camera: %s (file left partial)\r\n",
			          cam_strerror(rc));
			return 1;
		}
		/* Generation check: a concurrent capture between rows replaces
		   the pixels and re-validates the buffer -- without this the
		   file would silently mix two frames. */
		if (y == 0) {
			gen0 = gen;
		} else if (gen != gen0) {
			(void)fx_file_close(&file);
			cli_error(sh, "camera: frame changed mid-save by a "
			              "concurrent capture (file left partial)\r\n");
			return 1;
		}
		status = fx_file_write(&file, row, sizeof row);
		if (cli_cancel_requested(sh)) {
			(void)fx_file_close(&file);
			cli_error(sh, "camera: cancelled (file left partial)\r\n");
			return 1;
		}
	}
	(void)fx_file_close(&file);
	if (status == FX_SUCCESS)
		status = fx_media_flush(media);

	if (status != FX_SUCCESS) {
		cli_error(sh, "camera: write failed: %s (0x%02x)\r\n",
		          fs_strerror(status), status);
		return 1;
	}
	cli_print(sh, "wrote %lu bytes (%ux%u RGB565 raw) to %s:%s\r\n",
	          (unsigned long)CAMERA_FRAME_BYTES,
	          (unsigned)CAMERA_FRAME_WIDTH, (unsigned)CAMERA_FRAME_HEIGHT,
	          dev->name, path);
	cli_print(sh, "PC: python3 scripts/rgb565_to_png.py <file> out.png\r\n");
	return 0;
}

static int cmd_camera_save(struct cli_instance *sh, int argc, char **argv)
{
	const struct fs_device *dev;
	struct camera_info ci;
	UINT status;
	int rc;

	(void)argc;

	if (strcmp(argv[1], "sd") == 0) {
		dev = fs_sd_device();
	} else if (strcmp(argv[1], "fs") == 0) {
		dev = fs_qspi_device();
	} else {
		cli_error(sh, "camera: unknown medium '%s' (sd or fs)\r\n",
		          argv[1]);
		return 1;
	}

	/* Early no-frame check so we do not create an empty file; the per-row
	   reads still fail cleanly if the frame is invalidated mid-save. */
	if (camera_get_info(&ci) != 0 || !ci.frame_valid) {
		cli_error(sh, "camera: %s\r\n", cam_strerror(CAM_ERR_NO_FRAME));
		return 1;
	}

	status = dev->op_begin();
	if (status != FX_SUCCESS) {
		cli_error(sh, "camera: %s: %s\r\n", dev->name,
		          fs_strerror(status));
		return 1;
	}
	rc = save_body(dev, sh, argv[2]);
	dev->op_end();
	return rc;
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
	CLI_CMD_ARG(save, NULL, "write frame as raw RGB565 <sd|fs> <path>",
	            cmd_camera_save, 3, 0),
	CLI_CMD(off,   NULL, "cut camera module power", cmd_camera_off),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(camera, camera_subcmds, "B-CAMS-OMV camera (OV5640 on DCMI)",
                 NULL, 1, 0);
