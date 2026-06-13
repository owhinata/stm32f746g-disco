/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_lcd.c
 * @brief   `lcd` shell command: LTDC display status + test patterns (issue #52).
 *
 *   lcd info            panel / clock / frame-buffer / LTDC error flags
 *   lcd fill <color>    flood the screen (colour name or 0xRGB565 / decimal)
 *   lcd bar             eight vertical colour bars (RGB channel / bit-order check)
 *   lcd grad            horizontal black->white gradient (pixel-clock check)
 *   lcd clear           fill black
 *   lcd on | lcd off    display-enable + backlight
 *
 * Phase 1 of #48: the LTDC is brought up at boot (src/main.c) and these draw
 * straight into the non-cacheable SDRAM frame buffer with the CPU.  Used to
 * verify the LCD wiring, RGB channel order and pixel clock before DMA2D /
 * double buffering (#53), touch (#54) and GUIX (#55/#56).
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

/* Shared guard for the drawing/backlight commands. */
static int lcd_ready(struct cli_instance *sh)
{
	if (!ltdc_is_up()) {
		cli_error(sh, "lcd: display not initialized\r\n");
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
	cli_print(sh, "clock:   LCD_CLK %lu.%02lu MHz (PLLSAI N=192 R=5, DIVR/4)\r\n",
	          (unsigned long)(LTDC_PIXEL_CLOCK_HZ / 1000000u),
	          (unsigned long)(LTDC_PIXEL_CLOCK_HZ % 1000000u / 10000u));
	cli_print(sh, "fb:      0x%08lx (.sdram, non-cacheable)\r\n",
	          (unsigned long)(uintptr_t)ltdc_framebuffer());
	cli_print(sh, "state:   %s\r\n", ltdc_is_up() ? "up" : "DOWN (init failed)");
	cli_print(sh, "errors:  underrun=%s transfer=%s\r\n",
	          (err & LTDC_ERRFLAG_FIFO_UNDERRUN) ? "YES" : "no",
	          (err & LTDC_ERRFLAG_TRANSFER_ERROR) ? "YES" : "no");
	return 0;
}

static int cmd_lcd_fill(struct cli_instance *sh, int argc, char **argv)
{
	uint16_t color;

	(void)argc;
	if (!lcd_ready(sh))
		return 1;
	if (parse_color(argv[1], &color) != 0) {
		cli_error(sh, "lcd: bad colour '%s' "
		              "(name or 0xRGB565)\r\n", argv[1]);
		return 1;
	}
	ltdc_fill(color);
	return 0;
}

static int cmd_lcd_bar(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	if (!lcd_ready(sh))
		return 1;
	ltdc_colorbar();
	return 0;
}

static int cmd_lcd_grad(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	if (!lcd_ready(sh))
		return 1;
	ltdc_gradient();
	return 0;
}

static int cmd_lcd_clear(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	if (!lcd_ready(sh))
		return 1;
	ltdc_fill(LTDC_RGB565_BLACK);
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
	CLI_CMD(on, NULL, "display-enable + backlight on", cmd_lcd_on),
	CLI_CMD(off, NULL, "display-enable + backlight off", cmd_lcd_off),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(lcd, lcd_subcmds,
                 "on-board 4.3\" LCD (RK043FN48H 480x272, LTDC)", NULL, 1, 0);
