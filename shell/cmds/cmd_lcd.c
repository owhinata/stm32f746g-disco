/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_lcd.c
 * @brief   `lcd` shell command: LTDC display status + test patterns (issue #52/#53).
 *
 *   lcd info            panel / clock / frame-buffer / LTDC error flags
 *   lcd fill <color>    flood the screen (colour name or 0xRGB565 / decimal)
 *   lcd bar             eight vertical colour bars (RGB channel / bit-order check)
 *   lcd grad            horizontal black->white gradient (pixel-clock check)
 *   lcd clear           fill black
 *   lcd anim            bouncing rectangle (tear-free double-buffer demo)
 *   lcd blit            DMA2D M2M demo (copy the colour bars left->right half)
 *   lcd on | lcd off    display-enable + backlight
 *
 * Phase 2 of #48 (#53): the LTDC is brought up at boot (src/main.c) with a
 * DMA2D-accelerated, tear-free double buffer.  Each drawing command paints the
 * back buffer (DMA2D fills/blits or, for the gradient, the CPU) then presents it
 * with ltdc_flip() -- a vertical-blanking reload confirmed via SRCR.VBR.  Used
 * to verify the DMA2D path and the tear-free swap before touch (#54) and GUIX
 * (#55/#56).
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "ltdc_display.h"

#include <stdint.h>
#include <string.h>

static int parse_u32(const char *s, uint32_t *out)
{
	uint32_t base = 10;
	uint32_t val = 0;
	const char *p = s;

	if (p == NULL || *p == '\0')
		return -1;
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
		base = 16;
		p += 2;
		if (*p == '\0')
			return -1;
	}
	for (; *p != '\0'; p++) {
		uint32_t digit;
		char c = *p;

		if (c >= '0' && c <= '9')
			digit = (uint32_t)(c - '0');
		else if (base == 16 && c >= 'a' && c <= 'f')
			digit = (uint32_t)(c - 'a' + 10);
		else if (base == 16 && c >= 'A' && c <= 'F')
			digit = (uint32_t)(c - 'A' + 10);
		else
			return -1;
		if (val > (0xFFFFFFFFu - digit) / base)
			return -1;
		val = val * base + digit;
	}
	*out = val;
	return 0;
}

/* Colour name or numeric RGB565 (0x.... / decimal, 0..0xFFFF). */
static int parse_color(const char *s, uint16_t *out)
{
	static const struct {
		const char *name;
		uint16_t    rgb;
	} names[] = {
		{ "black",   LTDC_RGB565_BLACK   },
		{ "blue",    LTDC_RGB565_BLUE    },
		{ "green",   LTDC_RGB565_GREEN   },
		{ "cyan",    LTDC_RGB565_CYAN    },
		{ "red",     LTDC_RGB565_RED     },
		{ "magenta", LTDC_RGB565_MAGENTA },
		{ "yellow",  LTDC_RGB565_YELLOW  },
		{ "white",   LTDC_RGB565_WHITE   },
	};
	uint32_t v;

	for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
		if (strcmp(s, names[i].name) == 0) {
			*out = names[i].rgb;
			return 0;
		}
	}
	if (parse_u32(s, &v) == 0 && v <= 0xFFFFu) {
		*out = (uint16_t)v;
		return 0;
	}
	return -1;
}

/* Shared guard for the drawing/backlight/info commands. */
static int lcd_ready(struct cli_instance *sh)
{
	if (!ltdc_is_up()) {
		cli_error(sh, "lcd: display not initialized\r\n");
		return 0;
	}
	return 1;
}

/* Guard for the *drawing* commands: additionally refuse while GUIX owns the
   display (#55).  The draw helpers / ltdc_flip() are no-ops under GUIX ownership
   anyway, but failing up-front with a clear message is friendlier.  Backlight
   (on/off) and info do NOT use this -- they are harmless while GUIX runs. */
static int lcd_can_draw(struct cli_instance *sh)
{
	if (!lcd_ready(sh))
		return 0;
	if (ltdc_gui_owns()) {
		cli_error(sh, "lcd: display owned by gui; run 'gui stop' first\r\n");
		return 0;
	}
	return 1;
}

static int cmd_lcd_info(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t err = ltdc_errors(false);

	(void)argc;
	(void)argv;

	cli_print(sh, "panel:   RK043FN48H-CT 480x272 RGB565 (LTDC layer 0)\r\n");
	cli_print(sh, "clock:   LCD_CLK %lu.%02lu MHz (PLLSAI N=192 R=5, DIVR/8)\r\n",
	          (unsigned long)(LTDC_PIXEL_CLOCK_HZ / 1000000u),
	          (unsigned long)(LTDC_PIXEL_CLOCK_HZ % 1000000u / 10000u));
	cli_print(sh, "fb:      0x%08lx (.sdram, non-cacheable)\r\n",
	          (unsigned long)(uintptr_t)ltdc_framebuffer());
	cli_print(sh, "buffers: 2 (double, tear-free VBR)\r\n");
	cli_print(sh, "front:   %u\r\n", (unsigned)ltdc_active_buffer());
	cli_print(sh, "DMA2D:   on\r\n");
	cli_print(sh, "state:   %s\r\n", ltdc_is_up() ? "up" : "DOWN (init failed)");
	cli_print(sh, "errors:  underrun=%s transfer=%s\r\n",
	          (err & LTDC_ERRFLAG_FIFO_UNDERRUN) ? "YES" : "no",
	          (err & LTDC_ERRFLAG_TRANSFER_ERROR) ? "YES" : "no");
	return 0;
}

/* Draw-then-present helper: the caller drew into the back buffer; flip it and
   warn (but do not fail the command) if the tear-free swap did not land. */
static void lcd_present(struct cli_instance *sh)
{
	if (ltdc_flip() != 0)
		cli_warn(sh, "lcd: present failed\r\n");
}

static int cmd_lcd_fill(struct cli_instance *sh, int argc, char **argv)
{
	uint16_t color;

	(void)argc;
	if (!lcd_can_draw(sh))
		return 1;
	if (parse_color(argv[1], &color) != 0) {
		cli_error(sh, "lcd: bad colour '%s' "
		              "(name or 0xRGB565)\r\n", argv[1]);
		return 1;
	}
	ltdc_lock_frame();
	ltdc_fill(color);
	lcd_present(sh);
	ltdc_unlock_frame();
	return 0;
}

static int cmd_lcd_bar(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	if (!lcd_can_draw(sh))
		return 1;
	ltdc_lock_frame();
	ltdc_colorbar();
	lcd_present(sh);
	ltdc_unlock_frame();
	return 0;
}

static int cmd_lcd_grad(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	if (!lcd_can_draw(sh))
		return 1;
	ltdc_lock_frame();
	ltdc_gradient();
	lcd_present(sh);
	ltdc_unlock_frame();
	return 0;
}

static int cmd_lcd_clear(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	if (!lcd_can_draw(sh))
		return 1;
	ltdc_lock_frame();
	ltdc_fill(LTDC_RGB565_BLACK);
	lcd_present(sh);
	ltdc_unlock_frame();
	return 0;
}

static int cmd_lcd_anim(struct cli_instance *sh, int argc, char **argv)
{
	/* A 40x40 rectangle bouncing on a dark background.  ltdc_flip() blocks
	   until the vertical-blanking reload commits, so the loop is naturally
	   paced to the panel's frame rate -- no explicit sleep needed; Ctrl+C is
	   polled once per frame for prompt cancellation. */
	int x = 0, y = 0, dx = 5, dy = 3;
	const int w = 40, h = 40;
	const int maxx = (int)LTDC_LCD_WIDTH - w;
	const int maxy = (int)LTDC_LCD_HEIGHT - h;

	(void)argc;
	(void)argv;
	if (!lcd_can_draw(sh))
		return 1;

	for (;;) {
		int rc;

		/* If `gui start` takes the display mid-animation (this command may run
		   as a background job), stop -- the draw/flip below would no-op/refuse
		   anyway, but exiting cleanly avoids a spurious "present failed". */
		if (ltdc_gui_owns())
			break;

		ltdc_lock_frame();
		ltdc_fill(0x0008u);                              /* dim background */
		ltdc_fill_rect((uint16_t)x, (uint16_t)y,
		               (uint16_t)w, (uint16_t)h, LTDC_RGB565_CYAN);
		rc = ltdc_flip();
		ltdc_unlock_frame();
		if (rc != 0) {
			cli_error(sh, "lcd: present failed\r\n");
			return 1;
		}

		x += dx;
		y += dy;
		if (x <= 0 || x >= maxx) { dx = -dx; x += dx; }
		if (y <= 0 || y >= maxy) { dy = -dy; y += dy; }

		if (cli_cancel_requested(sh))
			break;
	}
	return 0;
}

static int cmd_lcd_blit(struct cli_instance *sh, int argc, char **argv)
{
	/* DMA2D M2M demo (single frame): draw the colour bars into the back buffer,
	   then copy its left half over its right half with a strided M2M blit, and
	   present once.  The right half should mirror the left's bars (left: full
	   8-bar set; right: a copy of the left 4 bars) -- verifies the M2M path. */
	int rc;

	(void)argc;
	(void)argv;
	if (!lcd_can_draw(sh))
		return 1;

	ltdc_lock_frame();
	ltdc_colorbar();      /* pattern into the back buffer */
	ltdc_blit_demo();     /* M2M copy left half -> right half (same buffer) */
	rc = ltdc_flip();
	ltdc_unlock_frame();
	if (rc != 0) {
		cli_error(sh, "lcd: present failed\r\n");
		return 1;
	}
	return 0;
}

static int cmd_lcd_on(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	if (!lcd_ready(sh))
		return 1;
	ltdc_backlight(true);
	return 0;
}

static int cmd_lcd_off(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	if (!lcd_ready(sh))
		return 1;
	ltdc_backlight(false);
	return 0;
}

CLI_SUBCMD_SET_CREATE(lcd_subcmds,
	CLI_CMD(info, NULL, "panel / clock / frame buffer / LTDC error flags",
	        cmd_lcd_info),
	CLI_CMD_ARG(fill, NULL, "flood with a colour (name or 0xRGB565)",
	            cmd_lcd_fill, 2, 0),
	CLI_CMD(bar, NULL, "8 vertical colour bars (RGB wiring check)", cmd_lcd_bar),
	CLI_CMD(grad, NULL, "horizontal gradient (pixel-clock check)", cmd_lcd_grad),
	CLI_CMD(clear, NULL, "fill black", cmd_lcd_clear),
	CLI_CMD(anim, NULL, "bouncing rectangle (tear-free double-buffer demo)",
	        cmd_lcd_anim),
	CLI_CMD(blit, NULL, "DMA2D M2M demo (copy bars left->right half)",
	        cmd_lcd_blit),
	CLI_CMD(on, NULL, "display-enable + backlight on", cmd_lcd_on),
	CLI_CMD(off, NULL, "display-enable + backlight off", cmd_lcd_off),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(lcd, lcd_subcmds,
                 "on-board 4.3\" LCD (RK043FN48H 480x272, LTDC)", NULL, 1, 0);
