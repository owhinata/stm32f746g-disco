/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_qspi.c
 * @brief   `qspi` shell command (issue #29): QSPI NOR flash bring-up/debug.
 *
 *   qspi id                read the JEDEC ID (expect 20 BA 18)
 *   qspi info              geometry, clock, status/flag registers
 *   qspi read <addr> [len] hexdump flash content (indirect read, no 0x90000000)
 *   qspi erase <addr>      erase the 4 KB subsector containing <addr>   (danger)
 *   qspi test <addr>       destructive self-test on one 4 KB subsector  (danger)
 *
 * `qspi test` is the Phase A hardware acceptance: erase -> blank check ->
 * program a counting pattern page by page -> read back and verify.  It polls
 * cli_cancel_requested() between pages so Ctrl+C aborts promptly (the subsector
 * is left partially programmed -- it is a scratch area by contract).
 *
 * erase/test rewrite flash content, so they compile in only with
 * CLI_ENABLE_DANGEROUS_CMDS, like devmem.  Once the filesystem lands (#27
 * Phase B) they will additionally be refused while the FileX media is mounted.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "qspi_flash.h"

#include <stdint.h>
#include <string.h>

/* Upper bound on one `qspi read` hexdump, mirroring devmem's dump cap. */
#define QSPI_READ_MAX_LEN 256u

static const char *qspi_strerror(int rc)
{
	switch (rc) {
	case QSPI_FLASH_ERR_PARAM:   return "address/length out of range";
	case QSPI_FLASH_ERR_HAL:     return "QUADSPI transfer failed";
	case QSPI_FLASH_ERR_TIMEOUT: return "flash busy timeout";
	case QSPI_FLASH_ERR_FLASH:   return "flash reported program/erase error";
	case QSPI_FLASH_ERR_STATE:   return "driver not initialized";
	default:                     return "unknown error";
	}
}

/* Strict 32-bit parse: 0x-prefixed hex or decimal (same contract as devmem). */
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

static int cmd_qspi_id(struct cli_instance *sh, int argc, char **argv)
{
	uint8_t id[3];
	int rc;

	(void)argc; (void)argv;

	rc = qspi_flash_read_id(id);
	if (rc != 0) {
		cli_error(sh, "qspi: read id failed: %s\r\n", qspi_strerror(rc));
		return 1;
	}
	cli_print(sh, "JEDEC ID: %02X %02X %02X", id[0], id[1], id[2]);
	if (id[0] == 0x20 && id[1] == 0xBA && id[2] == 0x18)
		cli_print(sh, "  (Micron N25Q128A/MT25QL128, 16 MiB)\r\n");
	else
		cli_print(sh, "  (unexpected -- check wiring/AF)\r\n");
	return 0;
}

static int cmd_qspi_info(struct cli_instance *sh, int argc, char **argv)
{
	const struct qspi_flash_info *fi = qspi_flash_get_info();
	uint8_t sr;
	int rc;

	(void)argc; (void)argv;

	cli_print(sh, "size      : %lu KiB\r\n", (unsigned long)(fi->size / 1024u));
	cli_print(sh, "sector    : %lu KiB (erase 0xD8)\r\n",
	          (unsigned long)(fi->sector_size / 1024u));
	cli_print(sh, "subsector : %lu KiB (erase 0x20)\r\n",
	          (unsigned long)(fi->subsector_size / 1024u));
	cli_print(sh, "page      : %lu B (program 0x02)\r\n",
	          (unsigned long)fi->page_size);
	cli_print(sh, "sclk      : %lu MHz, 1-line, indirect mode\r\n",
	          (unsigned long)(fi->sclk_hz / 1000000u));

	rc = qspi_flash_read_status(&sr);
	if (rc != 0) {
		cli_error(sh, "qspi: read status failed: %s\r\n", qspi_strerror(rc));
		return 1;
	}
	cli_print(sh, "status    : 0x%02x%s\r\n", sr,
	          (sr & 0x01u) ? " (write in progress)" : "");
	return 0;
}

static int cmd_qspi_read(struct cli_instance *sh, int argc, char **argv)
{
	uint8_t buf[QSPI_READ_MAX_LEN];
	uint32_t addr, len = 64;
	int rc;

	if (parse_u32(argv[1], &addr) != 0) {
		cli_error(sh, "qspi: bad address '%s'\r\n", argv[1]);
		return 1;
	}
	if (argc >= 3 && parse_u32(argv[2], &len) != 0) {
		cli_error(sh, "qspi: bad length '%s'\r\n", argv[2]);
		return 1;
	}
	if (len == 0)
		return 0;
	if (len > QSPI_READ_MAX_LEN) {
		cli_error(sh, "qspi: length %lu exceeds max %u\r\n",
		          (unsigned long)len, (unsigned)QSPI_READ_MAX_LEN);
		return 1;
	}

	rc = qspi_flash_read(addr, buf, len);
	if (rc != 0) {
		cli_error(sh, "qspi: read failed: %s\r\n", qspi_strerror(rc));
		return 1;
	}
	return cli_hexdump_base(sh, buf, len, addr) == 0 ? 0 : 1;
}

#if CLI_ENABLE_DANGEROUS_CMDS

/* Parse + gate a destructive target: must be subsector-aligned and in range. */
static int parse_subsector(struct cli_instance *sh, const char *s, uint32_t *addr)
{
	if (parse_u32(s, addr) != 0) {
		cli_error(sh, "qspi: bad address '%s'\r\n", s);
		return -1;
	}
	if (*addr >= QSPI_FLASH_SIZE) {
		cli_error(sh, "qspi: 0x%08lx beyond flash end\r\n", (unsigned long)*addr);
		return -1;
	}
	if (*addr % QSPI_FLASH_SUBSECTOR_SIZE != 0) {
		cli_error(sh, "qspi: 0x%08lx not 4 KB aligned\r\n", (unsigned long)*addr);
		return -1;
	}
	return 0;
}

static int cmd_qspi_erase(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t addr;
	int rc;

	(void)argc;

	if (parse_subsector(sh, argv[1], &addr) != 0)
		return 1;
	rc = qspi_flash_erase_subsector(addr);
	if (rc != 0) {
		cli_error(sh, "qspi: erase failed: %s\r\n", qspi_strerror(rc));
		return 1;
	}
	cli_print(sh, "erased 4 KB at 0x%08lx\r\n", (unsigned long)addr);
	return 0;
}

/*
 * Destructive self-test on one 4 KB subsector: erase -> blank check -> program
 * a counting pattern (page-indexed so a misdirected page is caught) -> read
 * back -> verify.  One 256 B working buffer; cancel polled between pages.
 */
static int cmd_qspi_test(struct cli_instance *sh, int argc, char **argv)
{
	uint8_t buf[QSPI_FLASH_PAGE_SIZE];
	uint32_t addr, off, i;
	int rc;

	(void)argc;

	if (parse_subsector(sh, argv[1], &addr) != 0)
		return 1;

	cli_print(sh, "erase 4 KB at 0x%08lx...\r\n", (unsigned long)addr);
	rc = qspi_flash_erase_subsector(addr);
	if (rc != 0) {
		cli_error(sh, "qspi: erase failed: %s\r\n", qspi_strerror(rc));
		return 1;
	}

	for (off = 0; off < QSPI_FLASH_SUBSECTOR_SIZE; off += sizeof buf) {
		if (cli_cancel_requested(sh))
			return 1;
		rc = qspi_flash_read(addr + off, buf, sizeof buf);
		if (rc != 0) {
			cli_error(sh, "qspi: read failed: %s\r\n", qspi_strerror(rc));
			return 1;
		}
		for (i = 0; i < sizeof buf; i++) {
			if (buf[i] != 0xFF) {
				cli_error(sh, "FAIL: not blank at 0x%08lx (0x%02x)\r\n",
				          (unsigned long)(addr + off + i), buf[i]);
				return 1;
			}
		}
	}
	cli_print(sh, "blank check ok, programming pattern...\r\n");

	for (off = 0; off < QSPI_FLASH_SUBSECTOR_SIZE; off += sizeof buf) {
		if (cli_cancel_requested(sh))
			return 1;
		for (i = 0; i < sizeof buf; i++)
			buf[i] = (uint8_t)(i ^ (off >> 8));
		rc = qspi_flash_write(addr + off, buf, sizeof buf);
		if (rc != 0) {
			cli_error(sh, "qspi: program failed at 0x%08lx: %s\r\n",
			          (unsigned long)(addr + off), qspi_strerror(rc));
			return 1;
		}
	}

	for (off = 0; off < QSPI_FLASH_SUBSECTOR_SIZE; off += sizeof buf) {
		if (cli_cancel_requested(sh))
			return 1;
		rc = qspi_flash_read(addr + off, buf, sizeof buf);
		if (rc != 0) {
			cli_error(sh, "qspi: read failed: %s\r\n", qspi_strerror(rc));
			return 1;
		}
		for (i = 0; i < sizeof buf; i++) {
			uint8_t want = (uint8_t)(i ^ (off >> 8));

			if (buf[i] != want) {
				cli_error(sh, "FAIL: 0x%08lx read 0x%02x want 0x%02x\r\n",
				          (unsigned long)(addr + off + i), buf[i], want);
				return 1;
			}
		}
	}

	cli_print(sh, "PASS: erase/program/verify 4 KB at 0x%08lx\r\n",
	          (unsigned long)addr);
	return 0;
}

#endif /* CLI_ENABLE_DANGEROUS_CMDS */

CLI_SUBCMD_SET_CREATE(qspi_subcmds,
	CLI_CMD_ARG(id,    NULL, "read JEDEC ID",                cmd_qspi_id,    1, 0),
	CLI_CMD_ARG(info,  NULL, "geometry/clock/status",        cmd_qspi_info,  1, 0),
	CLI_CMD_ARG(read,  NULL, "hexdump <addr> [len]",         cmd_qspi_read,  2, 1),
#if CLI_ENABLE_DANGEROUS_CMDS
	CLI_CMD_ARG(erase, NULL, "erase 4KB subsector <addr>",   cmd_qspi_erase, 2, 0),
	CLI_CMD_ARG(test,  NULL, "destructive self-test <addr>", cmd_qspi_test,  2, 0),
#endif
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(qspi, qspi_subcmds,
                 "QSPI NOR flash (N25Q128A, 16 MiB)", NULL, 1, 0);
