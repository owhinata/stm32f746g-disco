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
 *   camera send [name]      stream the frame to the PC over YMODEM (#50)
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
#include "cmd_xfer.h"

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

/* ---- `camera set` quality controls (issue #44) --------------------------- */

/* Symbolic-value tables for the enumerated settings (name <-> port enum). */
struct cam_named { const char *name; int val; };

static const struct cam_named awb_names[] = {
	{ "auto", CAM_AWB_AUTO }, { "sunny", CAM_AWB_SUNNY },
	{ "office", CAM_AWB_OFFICE }, { "home", CAM_AWB_HOME },
	{ "cloudy", CAM_AWB_CLOUDY }, { NULL, 0 }
};
static const struct cam_named effect_names[] = {
	{ "none", CAM_FX_NONE }, { "bw", CAM_FX_BW }, { "sepia", CAM_FX_SEPIA },
	{ "negative", CAM_FX_NEGATIVE }, { "blue", CAM_FX_BLUE },
	{ "red", CAM_FX_RED }, { "green", CAM_FX_GREEN }, { NULL, 0 }
};
static const struct cam_named flip_names[] = {
	{ "none", CAM_FLIP_NONE }, { "mirror", CAM_FLIP_MIRROR },
	{ "flip", CAM_FLIP_FLIP }, { "both", CAM_FLIP_BOTH }, { NULL, 0 }
};

static int cam_lookup(const struct cam_named *t, const char *s, int *out)
{
	for (; t->name != NULL; t++) {
		if (strcmp(t->name, s) == 0) {
			*out = t->val;
			return 0;
		}
	}
	return -1;
}

static const char *cam_name_of(const struct cam_named *t, int v)
{
	for (; t->name != NULL; t++)
		if (t->val == v)
			return t->name;
	return "?";
}

/* Signed decimal parse for the small bounded levels (brightness/hue/zoom). */
static int cam_parse_int(const char *s, int *out)
{
	const char *p = s;
	int sign = 1, val = 0, any = 0;

	if (p == NULL || *p == '\0')
		return -1;
	if (*p == '-' || *p == '+') {
		sign = (*p == '-') ? -1 : 1;
		p++;
	}
	for (; *p != '\0'; p++) {
		if (*p < '0' || *p > '9')
			return -1;
		val = val * 10 + (*p - '0');
		if (val > 100000)        /* far beyond any camera range */
			return -1;
		any = 1;
	}
	if (!any)
		return -1;
	*out = sign * val;
	return 0;
}

static void print_settings(struct cli_instance *sh,
                           const struct camera_settings *s)
{
	cli_print(sh, "brightness: %d\r\n", s->brightness);
	cli_print(sh, "contrast:   %d\r\n", s->contrast);
	cli_print(sh, "saturation: %d\r\n", s->saturation);
	cli_print(sh, "hue:        %d deg\r\n", s->hue * 30);
	cli_print(sh, "awb:        %s\r\n", cam_name_of(awb_names, s->awb));
	cli_print(sh, "effect:     %s\r\n", cam_name_of(effect_names, s->effect));
	cli_print(sh, "flip:       %s\r\n", cam_name_of(flip_names, s->flip));
	cli_print(sh, "zoom:       x%u\r\n", (unsigned)s->zoom);
	cli_print(sh, "night:      %s\r\n", s->night ? "on" : "off");
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
	struct camera_settings cs;
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

	if (camera_get_settings(&cs) == 0) {
		cli_print(sh, "-- settings (camera set) --\r\n");
		print_settings(sh, &cs);
	}
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

/*
 * Source adapter that streams the captured frame straight from SDRAM (no FS) for
 * `camera send`.  Mirrors save_body's generation check: a concurrent capture
 * between reads re-validates the buffer with NEW pixels, so a changed generation
 * fails the read (-> YM_ERR_SOURCE) rather than silently mixing two frames.
 */
struct cam_send_ctx {
	uint32_t off;
	uint32_t gen0;
	int      have_gen0;
};

static int cam_send_read(void *ctx, uint8_t *dst, uint32_t want, uint32_t *got)
{
	struct cam_send_ctx *cc = (struct cam_send_ctx *)ctx;
	uint32_t rem, n, gen;
	int rc;

	if (cc->off >= CAMERA_FRAME_BYTES) {
		*got = 0;
		return 0;                       /* EOF */
	}
	rem = (uint32_t)CAMERA_FRAME_BYTES - cc->off;
	n = (want < rem) ? want : rem;
	rc = camera_frame_read(cc->off, dst, n, &gen);
	if (rc != 0)
		return -1;
	if (!cc->have_gen0) {
		cc->gen0 = gen;
		cc->have_gen0 = 1;
	} else if (gen != cc->gen0) {
		return -1;                      /* frame changed mid-send */
	}
	cc->off += n;
	*got = n;
	return 0;
}

static int cmd_camera_send(struct cli_instance *sh, int argc, char **argv)
{
	struct camera_info ci;
	struct cam_send_ctx cc = { 0, 0, 0 };
	struct ym_source src;

	if (camera_get_info(&ci) != 0 || !ci.frame_valid) {
		cli_error(sh, "camera: %s\r\n", cam_strerror(CAM_ERR_NO_FRAME));
		return 1;
	}

	src.ctx  = &cc;
	src.name = (argc >= 2) ? argv[1] : "frame.raw";
	src.size = (uint32_t)CAMERA_FRAME_BYTES;
	src.read = cam_send_read;
	return xfer_send_source(sh, &src);
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

/*
 * `camera set` is a real subcommand tree (one leaf per OV5640 control) so the
 * hierarchical help (issue #37) lists every setting via `help camera set`, and
 * a missing/extra value auto-prints the leaf's usage.  Each leaf validates its
 * value, calls the matching port setter and reports the outcome; the settings
 * are cached and applied immediately when the sensor is live, else at the next
 * capture.  brightness/contrast/saturation/hue coexist (the driver fixes up the
 * OV5640's shared SDE_CTRL0/CTRL8 registers).
 */
static int set_reject(struct cli_instance *sh, const char *name, const char *val)
{
	cli_error(sh, "camera: bad value '%s' for %s\r\n", val, name);
	return 1;
}

static int set_report(struct cli_instance *sh, const char *name,
                      const char *val, int rc)
{
	if (rc != 0) {
		cli_error(sh, "camera: %s '%s' for %s\r\n", cam_strerror(rc),
		          val, name);
		return 1;
	}
	cli_print(sh, "camera: %s = %s\r\n", name, val);
	return 0;
}

/* The framework guarantees argv[1] (each leaf is registered mandatory = 2). */
static int cmd_set_brightness(struct cli_instance *sh, int argc, char **argv)
{
	int n;
	(void)argc;
	if (cam_parse_int(argv[1], &n) != 0)
		return set_reject(sh, argv[0], argv[1]);
	return set_report(sh, argv[0], argv[1], camera_set_brightness(n));
}

static int cmd_set_contrast(struct cli_instance *sh, int argc, char **argv)
{
	int n;
	(void)argc;
	if (cam_parse_int(argv[1], &n) != 0)
		return set_reject(sh, argv[0], argv[1]);
	return set_report(sh, argv[0], argv[1], camera_set_contrast(n));
}

static int cmd_set_saturation(struct cli_instance *sh, int argc, char **argv)
{
	int n;
	(void)argc;
	if (cam_parse_int(argv[1], &n) != 0)
		return set_reject(sh, argv[0], argv[1]);
	return set_report(sh, argv[0], argv[1], camera_set_saturation(n));
}

static int cmd_set_hue(struct cli_instance *sh, int argc, char **argv)
{
	int n;
	(void)argc;
	if (cam_parse_int(argv[1], &n) != 0)
		return set_reject(sh, argv[0], argv[1]);
	return set_report(sh, argv[0], argv[1], camera_set_hue(n));
}

static int cmd_set_awb(struct cli_instance *sh, int argc, char **argv)
{
	int n;
	(void)argc;
	if (cam_lookup(awb_names, argv[1], &n) != 0)
		return set_reject(sh, argv[0], argv[1]);
	return set_report(sh, argv[0], argv[1], camera_set_awb((enum camera_awb)n));
}

static int cmd_set_effect(struct cli_instance *sh, int argc, char **argv)
{
	int n;
	(void)argc;
	if (cam_lookup(effect_names, argv[1], &n) != 0)
		return set_reject(sh, argv[0], argv[1]);
	return set_report(sh, argv[0], argv[1],
	                  camera_set_effect((enum camera_effect)n));
}

static int cmd_set_flip(struct cli_instance *sh, int argc, char **argv)
{
	int n;
	(void)argc;
	if (cam_lookup(flip_names, argv[1], &n) != 0)
		return set_reject(sh, argv[0], argv[1]);
	return set_report(sh, argv[0], argv[1],
	                  camera_set_flip((enum camera_flip)n));
}

static int cmd_set_zoom(struct cli_instance *sh, int argc, char **argv)
{
	int n;
	(void)argc;
	if (cam_parse_int(argv[1], &n) != 0)
		return set_reject(sh, argv[0], argv[1]);
	return set_report(sh, argv[0], argv[1], camera_set_zoom(n));
}

static int cmd_set_night(struct cli_instance *sh, int argc, char **argv)
{
	int on;
	(void)argc;
	if (strcmp(argv[1], "on") == 0)
		on = 1;
	else if (strcmp(argv[1], "off") == 0)
		on = 0;
	else
		return set_reject(sh, argv[0], argv[1]);
	return set_report(sh, argv[0], argv[1], camera_set_night(on));
}

static int cmd_set_default(struct cli_instance *sh, int argc, char **argv)
{
	int rc;
	(void)argc;
	(void)argv;
	rc = camera_set_defaults();
	if (rc != 0) {
		cli_error(sh, "camera: %s\r\n", cam_strerror(rc));
		return 1;
	}
	cli_print(sh, "camera: settings reset to defaults\r\n");
	return 0;
}

/* `camera set` with no subcommand shows the current values; an unrecognized
   token falls here too (the parser passes it as an argument when it matches no
   leaf). */
static int cmd_camera_set(struct cli_instance *sh, int argc, char **argv)
{
	struct camera_settings s;
	int rc;

	if (argc > 1) {
		cli_error(sh, "camera: unknown setting '%s'\r\n", argv[1]);
		cli_print(sh, "type 'help camera set' for the list of settings\r\n");
		return 1;
	}
	rc = camera_get_settings(&s);
	if (rc != 0) {
		cli_error(sh, "camera: %s\r\n", cam_strerror(rc));
		return 1;
	}
	print_settings(sh, &s);
	cli_print(sh, "type 'help camera set' for the list of settings\r\n");
	return 0;
}

CLI_SUBCMD_SET_CREATE(camera_set_subcmds,
	CLI_CMD_ARG(brightness, NULL, "<-4..4>", cmd_set_brightness, 2, 0),
	CLI_CMD_ARG(contrast,   NULL, "<-4..4>", cmd_set_contrast, 2, 0),
	CLI_CMD_ARG(saturation, NULL, "<-4..4>", cmd_set_saturation, 2, 0),
	CLI_CMD_ARG(hue,    NULL, "<-180..150> in 30 deg steps", cmd_set_hue, 2, 0),
	CLI_CMD_ARG(awb,    NULL, "<auto|sunny|office|home|cloudy>", cmd_set_awb, 2, 0),
	CLI_CMD_ARG(effect, NULL, "<none|bw|sepia|negative|blue|red|green>",
	            cmd_set_effect, 2, 0),
	CLI_CMD_ARG(flip,   NULL, "<none|mirror|flip|both>", cmd_set_flip, 2, 0),
	CLI_CMD_ARG(zoom,   NULL, "<1|2|4|8>", cmd_set_zoom, 2, 0),
	CLI_CMD_ARG(night,  NULL, "<on|off>", cmd_set_night, 2, 0),
	CLI_CMD(default,    NULL, "reset all settings to neutral", cmd_set_default),
	CLI_SUBCMD_SET_END);

/* ---- streaming (issue #46): non-blocking start / stop / stats ------------ */

static int cmd_stream_start(struct cli_instance *sh, int argc, char **argv)
{
	int colorbar = 0;
	uint32_t frames = 0, secs = 0;
	int i, rc, n;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "test") == 0) {
			colorbar = 1;
		} else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
			if (cam_parse_int(argv[++i], &n) != 0 || n <= 0) {
				cli_error(sh, "camera: bad --frames '%s'\r\n", argv[i]);
				return 1;
			}
			frames = (uint32_t)n;
		} else if (strcmp(argv[i], "--secs") == 0 && i + 1 < argc) {
			if (cam_parse_int(argv[++i], &n) != 0 || n <= 0) {
				cli_error(sh, "camera: bad --secs '%s'\r\n", argv[i]);
				return 1;
			}
			secs = (uint32_t)n;
		} else {
			cli_error(sh, "camera: bad option '%s' "
			          "(test | --frames N | --secs S)\r\n", argv[i]);
			return 1;
		}
	}

	rc = camera_stream_start(colorbar, frames, secs);
	if (rc != 0) {
		cli_error(sh, "camera: %s\r\n", cam_strerror(rc));
		return 1;
	}
	cli_print(sh, "camera: streaming %s started",
	          colorbar ? "colorbar" : "live");
	if (frames)
		cli_print(sh, ", %lu frames", (unsigned long)frames);
	if (secs)
		cli_print(sh, ", %lu s", (unsigned long)secs);
	cli_print(sh, " (`camera stream stats` / `stop`)\r\n");
	return 0;
}

static int cmd_stream_stop(struct cli_instance *sh, int argc, char **argv)
{
	int rc;
	(void)argc;
	(void)argv;

	rc = camera_stream_stop();
	if (rc != 0) {
		cli_error(sh, "camera: %s\r\n", cam_strerror(rc));
		return 1;
	}
	cli_print(sh, "camera: stream stop requested\r\n");
	return 0;
}

static int cmd_stream_stats(struct cli_instance *sh, int argc, char **argv)
{
	struct camera_stream_info si;
	int rc;
	(void)argc;
	(void)argv;

	rc = camera_stream_stats(&si);
	if (rc != 0) {
		cli_error(sh, "camera: %s\r\n", cam_strerror(rc));
		return 1;
	}
	cli_print(sh, "state:     %s\r\n",
	          si.active ? "streaming"
	                    : (si.err ? "stopped (overrun)" : "stopped"));
	cli_print(sh, "frames:    %lu\r\n", (unsigned long)si.frames);
	cli_print(sh, "elapsed:   %lu ms\r\n", (unsigned long)si.elapsed_ms);
	if (si.elapsed_ms != 0) {
		/* frames-per-second x10 (one decimal), no floating point */
		uint32_t f10 = (uint32_t)((uint64_t)si.frames * 10000u /
		                          si.elapsed_ms);

		cli_print(sh, "fps:       %lu.%lu\r\n",
		          (unsigned long)(f10 / 10u), (unsigned long)(f10 % 10u));
	}
	cli_print(sh, "delivered: %lu\r\n", (unsigned long)si.delivered);
	cli_print(sh, "dropped:   %lu\r\n", (unsigned long)si.dropped);
	cli_print(sh, "ovr dcmi:  %lu\r\n", (unsigned long)si.dcmi_ovr);
	cli_print(sh, "ovr ring:  %lu\r\n", (unsigned long)si.ring_ovr);
	return 0;
}

static int cmd_camera_stream(struct cli_instance *sh, int argc, char **argv)
{
	if (argc > 1)
		cli_error(sh, "camera: unknown stream subcommand '%s'\r\n", argv[1]);
	cli_print(sh, "usage: camera stream <start [test] [--frames N] [--secs S]"
	          " | stop | stats>\r\n");
	return argc > 1 ? 1 : 0;
}

CLI_SUBCMD_SET_CREATE(camera_stream_subcmds,
	CLI_CMD_ARG(start, NULL,
	            "begin continuous capture (test, --frames N, --secs S)",
	            cmd_stream_start, 1, 5),
	CLI_CMD(stop,  NULL, "stop the running stream", cmd_stream_stop),
	CLI_CMD(stats, NULL, "FPS / frame / overrun counters", cmd_stream_stats),
	CLI_SUBCMD_SET_END);

CLI_SUBCMD_SET_CREATE(camera_subcmds,
	CLI_CMD(probe, NULL, "power-cycle + read the OV5640 chip ID", cmd_camera_probe),
	CLI_CMD(info,  NULL, "driver / sensor state", cmd_camera_info),
	CLI_CMD_ARG(capture, NULL, "snapshot QVGA RGB565 + stats ('test' = colorbar)",
	            cmd_camera_capture, 1, 1),
	CLI_CMD_ARG(save, NULL, "write frame as raw RGB565 <sd|fs> <path>",
	            cmd_camera_save, 3, 0),
	CLI_CMD_ARG(send, NULL, "stream the frame to the PC over YMODEM [name]",
	            cmd_camera_send, 1, 1),
	CLI_CMD_ARG(set,  camera_set_subcmds,
	            "OV5640 image quality (no arg = show current)",
	            cmd_camera_set, 1, 1),
	CLI_CMD_ARG(stream, camera_stream_subcmds,
	            "continuous capture (start/stop/stats)",
	            cmd_camera_stream, 1, 1),
	CLI_CMD(off,   NULL, "cut camera module power", cmd_camera_off),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(camera, camera_subcmds, "B-CAMS-OMV camera (OV5640 on DCMI)",
                 NULL, 1, 0);
