/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_sdram.c
 * @brief   `sdram` shell command: FMC SDRAM status + memtest (issue #40).
 *
 *   sdram info           base / size / controller state
 *   sdram test [bytes]   DESTRUCTIVE write/read-back memtest (default: all 8 MB)
 *
 * The test overwrites the tested span -- everything placed in the `.sdram`
 * linker section (the camera frame buffer, #41) is clobbered, so the test
 * first drops the camera's captured-frame flag via camera_frame_invalidate()
 * (otherwise `camera save`/stats would read test patterns believing them to
 * be a frame).
 *
 * Three passes over the span, word-wise: (1) address pattern (each word holds
 * its own address -- catches stuck/shorted/aliased address lines, which a
 * single repeated value would miss on a 16-bit-bus part), (2) 0x55555555,
 * (3) 0xAAAAAAAA (alternating bits both ways -- catches stuck/coupled data
 * lines).  Each pass writes the full span, then reads it back: a full-span
 * write pass pushes every row out of the SDRAM device's open-row latches and
 * exercises refresh over the multi-ms gap before read-back.  The region is
 * MPU-mapped non-cacheable, so reads hit the device (no cache masking).
 *
 * Cancellable between 64 KB chunks (Ctrl+C); progress is printed per MB.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "sdram.h"
#include "camera.h"   /* camera_frame_invalidate: the test clobbers .sdram */

#include <stdint.h>

/* Cancellation/progress granularity: 16K words = 64 KB per chunk. */
#define SDRAM_TEST_CHUNK_WORDS  (16u * 1024u)

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

static int cmd_sdram_info(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;

	cli_print(sh, "device:  MT48LC4M32B2 (128 Mbit, 16-bit bus -> 8 MB usable)\r\n");
	cli_print(sh, "window:  0x%08lx .. 0x%08lx (FMC bank1)\r\n",
	          (unsigned long)SDRAM_BASE_ADDR,
	          (unsigned long)(SDRAM_BASE_ADDR + SDRAM_SIZE_BYTES - 1u));
	cli_print(sh, "config:  SDCLK 108 MHz (HCLK/2), CAS 3, refresh 0x0603\r\n");
	cli_print(sh, "MPU:     Normal, non-cacheable, XN (DMA-coherent)\r\n");
	cli_print(sh, "state:   %s\r\n", sdram_is_up() ? "up" : "DOWN (init failed)");
	return 0;
}

/* One full write pass then one full read-back pass over [base, base+words).
   pattern==0 selects the address-in-word pattern.  Returns 0, a positive
   1-based fail word index hint via *fail_off (address printed by caller),
   or -1 on cancel. */
static int test_pass(struct cli_instance *sh, volatile uint32_t *base,
                     uint32_t words, uint32_t pattern, uint32_t *fail_off,
                     uint32_t *fail_got)
{
	uint32_t i, chunk;

	for (i = 0; i < words; i += chunk) {
		chunk = (words - i > SDRAM_TEST_CHUNK_WORDS)
		        ? SDRAM_TEST_CHUNK_WORDS : (words - i);
		for (uint32_t j = i; j < i + chunk; j++)
			base[j] = pattern ? pattern
			                  : (uint32_t)(uintptr_t)&base[j];
		if (cli_cancel_requested(sh))
			return -1;
	}

	for (i = 0; i < words; i += chunk) {
		chunk = (words - i > SDRAM_TEST_CHUNK_WORDS)
		        ? SDRAM_TEST_CHUNK_WORDS : (words - i);
		for (uint32_t j = i; j < i + chunk; j++) {
			uint32_t want = pattern ? pattern
			                        : (uint32_t)(uintptr_t)&base[j];
			uint32_t got = base[j];

			if (got != want) {
				*fail_off = j;
				*fail_got = got;
				return 1;
			}
		}
		if (cli_cancel_requested(sh))
			return -1;
	}
	return 0;
}

static int cmd_sdram_test(struct cli_instance *sh, int argc, char **argv)
{
	static const uint32_t patterns[] = { 0u /* address */, 0x55555555u,
	                                     0xAAAAAAAAu };
	volatile uint32_t *base = (volatile uint32_t *)SDRAM_BASE_ADDR;
	uint32_t bytes = SDRAM_SIZE_BYTES;
	uint32_t words, fail_off, fail_got;
	int rc;

	if (!sdram_is_up()) {
		cli_error(sh, "sdram: not initialized\r\n");
		return 1;
	}

	if (argc > 1) {
		if (parse_u32(argv[1], &bytes) != 0 || bytes == 0u ||
		    bytes > SDRAM_SIZE_BYTES || (bytes & 3u) != 0u) {
			cli_error(sh, "sdram: bad length (4-byte multiple, "
			              "4..%lu)\r\n",
			          (unsigned long)SDRAM_SIZE_BYTES);
			return 1;
		}
	}
	words = bytes / 4u;

	if (camera_streaming()) {
		cli_error(sh, "sdram: camera is streaming into .sdram; "
		              "run 'camera stream stop' first\r\n");
		return 1;
	}
	cli_warn(sh, "sdram: DESTRUCTIVE test over %lu KB "
	             "(clobbers .sdram contents, e.g. a captured frame)\r\n",
	         (unsigned long)(bytes / 1024u));
	camera_frame_invalidate();

	for (unsigned p = 0; p < sizeof patterns / sizeof patterns[0]; p++) {
		const char *name = (p == 0) ? "address" :
		                   (patterns[p] == 0x55555555u) ? "0x55555555"
		                                                : "0xAAAAAAAA";

		cli_print(sh, "pass %u/3 (%s): write+verify %lu KB ... ",
		          p + 1u, name, (unsigned long)(bytes / 1024u));
		rc = test_pass(sh, base, words, patterns[p], &fail_off, &fail_got);
		if (rc < 0) {
			cli_print(sh, "cancelled\r\n");
			return 1;
		}
		if (rc > 0) {
			cli_print(sh, "FAIL\r\n");
			cli_error(sh, "sdram: mismatch at 0x%08lx: got 0x%08lx, "
			              "want 0x%08lx\r\n",
			          (unsigned long)(uintptr_t)&base[fail_off],
			          (unsigned long)fail_got,
			          (unsigned long)(patterns[p] ? patterns[p]
			                : (uint32_t)(uintptr_t)&base[fail_off]));
			return 1;
		}
		cli_print(sh, "OK\r\n");
	}

	cli_print(sh, "sdram: %lu KB tested, no errors\r\n",
	          (unsigned long)(bytes / 1024u));
	return 0;
}

CLI_SUBCMD_SET_CREATE(sdram_subcmds,
	CLI_CMD(info, NULL, "FMC SDRAM window / config / state", cmd_sdram_info),
	CLI_CMD_ARG(test, NULL, "DESTRUCTIVE write/read-back memtest [bytes]",
	            cmd_sdram_test, 1, 1),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(sdram, sdram_subcmds, "external FMC SDRAM (8 MB @0xC0000000)",
                 NULL, 1, 0);
