/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_sd.c
 * @brief   `sd` shell command (issue #33): microSD bring-up / debug.
 *
 *   sd info            card type, capacity, block geometry, bus width, CID/CSD
 *   sd read <lba>      hexdump one 512 B block (LBA addressing)
 *
 * Phase A is read-only: it powers up / identifies the inserted card over SDMMC1
 * with DMA and reads blocks, but never writes, so it is safe on a PC-formatted
 * card (the FileX filesystem and writes arrive in Epic #32 Phase B/C).
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "sd_card.h"

#include <stdint.h>
#include <string.h>

static const char *sd_strerror(int rc)
{
	switch (rc) {
	case SD_ERR_PARAM:   return "bad argument";
	case SD_ERR_HAL:     return "SDMMC/DMA transfer failed";
	case SD_ERR_TIMEOUT: return "card not ready / transfer timed out";
	case SD_ERR_STATE:   return "driver not initialized / card not probed";
	case SD_ERR_NO_CARD: return "no card in slot";
	default:             return "unknown error";
	}
}

/* Strict 32-bit parse: 0x-prefixed hex or decimal (same contract as qspi/devmem). */
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

static int cmd_sd_info(struct cli_instance *sh, int argc, char **argv)
{
	const struct sd_card_info *ci;
	int rc;

	(void)argc;
	(void)argv;

	rc = sd_card_probe();
	if (rc != 0) {
		cli_error(sh, "sd: %s\r\n", sd_strerror(rc));
		return 1;
	}

	ci = sd_card_get_info();
	cli_print(sh, "type      : %s (v%s)\r\n",
	          (ci->type == 1u) ? "SDHC/SDXC" : "SDSC",
	          (ci->version == 1u) ? "2.x" : "1.x");
	cli_print(sh, "capacity  : %lu MiB (%lu blocks x %lu B)\r\n",
	          (unsigned long)(ci->capacity_bytes / (1024u * 1024u)),
	          (unsigned long)ci->block_count,
	          (unsigned long)ci->block_size);
	cli_print(sh, "bus width : %lu-bit\r\n", (unsigned long)ci->bus_width);
	cli_print(sh, "rca       : 0x%04lx\r\n", (unsigned long)ci->rca);
	cli_print(sh, "ccc       : 0x%03lx\r\n", (unsigned long)ci->card_class);
	cli_print(sh, "cid       : %08lx %08lx %08lx %08lx\r\n",
	          (unsigned long)ci->cid[0], (unsigned long)ci->cid[1],
	          (unsigned long)ci->cid[2], (unsigned long)ci->cid[3]);
	cli_print(sh, "csd       : %08lx %08lx %08lx %08lx\r\n",
	          (unsigned long)ci->csd[0], (unsigned long)ci->csd[1],
	          (unsigned long)ci->csd[2], (unsigned long)ci->csd[3]);
	return 0;
}

static int cmd_sd_read(struct cli_instance *sh, int argc, char **argv)
{
	uint8_t buf[512];
	uint32_t lba;
	int rc;

	(void)argc;

	/* argv[0] = "read", argv[1] = LBA. */
	if (parse_u32(argv[1], &lba) != 0) {
		cli_error(sh, "sd: bad LBA '%s'\r\n", argv[1]);
		return 1;
	}

	/* Identify the card on first use (or after a swap); reuse it otherwise. */
	if (sd_card_status() != SD_OK) {
		rc = sd_card_probe();
		if (rc != 0) {
			cli_error(sh, "sd: %s\r\n", sd_strerror(rc));
			return 1;
		}
	}

	rc = sd_card_read_blocks(lba, buf, 1);
	if (rc != 0) {
		cli_error(sh, "sd: read failed: %s\r\n", sd_strerror(rc));
		return 1;
	}

	return cli_hexdump_base(sh, buf, sizeof buf,
	                        (unsigned long long)lba * 512ull) == 0 ? 0 : 1;
}

CLI_SUBCMD_SET_CREATE(sd_subcmds,
	CLI_CMD_ARG(info, NULL, "card type/capacity/geometry/CID/CSD", cmd_sd_info, 1, 0),
	CLI_CMD_ARG(read, NULL, "hexdump one 512 B block <lba>",       cmd_sd_read, 2, 0),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(sd, sd_subcmds,
                 "microSD card (SDMMC1, DMA)", NULL, 1, 0);
