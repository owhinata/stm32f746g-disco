/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_fs.c
 * @brief   `fs` shell command (issue #30): FileX filesystem on the QSPI NOR.
 *
 *   fs format [full] yes   (re)format; `full` first erases all 256 blocks
 *   fs ls [path]           list a directory (default /)
 *   fs cat <path>          print a file
 *   fs write <path> <text> create/overwrite a file with <text> (quote for spaces)
 *   fs rm <path>           delete a file or empty directory
 *   fs mkdir <path>        create a directory
 *   fs info                capacity / free space / FAT type / wear
 *   fs umount              flush + unmount (e.g. before `qspi test`)
 *
 * Since issue #34 the media-independent bodies (ls/cat/write/rm/mkdir/info/
 * umount) live in fs_cmd_core.c and are shared with the `sd` command; this file
 * keeps only the QSPI-specific pieces: the LevelX format, the wear info line,
 * the `struct fs_device` instance binding the QSPI glue, and the thin wrappers
 * that register them.  Behaviour is unchanged from #30/#31.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "fs_cmd_core.h"
#include "fs_glue.h"
#include "fx_lx_nor_driver.h"
#include "lx_nor_qspi_driver.h"
#include "qspi_flash.h"

#include <string.h>

/* ---- QSPI device binding -------------------------------------------------- */

/* Wear-leveling visibility (issue #31): LevelX keeps per-block erase counts in
 * the block headers and tracks min/max live (updated at open and on every
 * reclaim), so the spread shows leveling at work.  Trailing line of `fs info`. */
static void qspi_info_extra(struct cli_instance *sh, FX_MEDIA *media)
{
	LX_NOR_FLASH *lx = fx_lx_nor_flash();

	(void)media;
	if (lx != NULL)
		cli_print(sh, "wear     : erase count min %lu / max %lu\r\n",
		          (unsigned long)lx->lx_nor_flash_minimum_erase_count,
		          (unsigned long)lx->lx_nor_flash_maximum_erase_count);
}

static const struct fs_device qspi_dev = {
	.name       = "fs",
	.mount_hint = "not formatted? run `fs format yes`",
	.acquire    = fs_media_acquire,
	.unmount    = fs_media_unmount,
	.is_mounted = fs_is_mounted,
	.op_begin   = fs_op_begin,   .op_end   = fs_op_end,
	.excl_begin = fs_exclusive_begin, .excl_end = fs_exclusive_end,
	.dir_lock   = fs_dir_lock,   .dir_unlock = fs_dir_unlock,
	.info_extra = qspi_info_extra,
};

/* ---- fs format (QSPI-specific: LevelX + full erase) ----------------------- */

/*
 * Erase all 256 blocks with progress + Ctrl+C between blocks.  A cancelled run
 * leaves the device unformatted by design (rerun `fs format`).
 */
static int fs_erase_all(struct cli_instance *sh)
{
	uint32_t block;
	int rc;

	for (block = 0; block < LX_QSPI_TOTAL_BLOCKS; block++) {
		if (cli_cancel_requested(sh)) {
			cli_print(sh, "\r\n");
			cli_warn(sh, "fs: format cancelled; flash left unformatted\r\n");
			return -1;
		}
		rc = qspi_flash_erase_sector(block * QSPI_FLASH_SECTOR_SIZE);
		if (rc != 0) {
			cli_print(sh, "\r\n");
			cli_error(sh, "fs: erase failed at block %lu (%d)\r\n",
			          (unsigned long)block, rc);
			return -1;
		}
		cli_print(sh, "\rerase %3lu/%u (%lu%%)",
		          (unsigned long)(block + 1), (unsigned)LX_QSPI_TOTAL_BLOCKS,
		          (unsigned long)((block + 1) * 100u / LX_QSPI_TOTAL_BLOCKS));
	}
	cli_print(sh, "\r\n");
	return 0;
}

static int cmd_fs_format(struct cli_instance *sh, int argc, char **argv)
{
	int full = 0;
	int rc = 1;
	ULONG sectors;
	UINT status;

	/* `fs format [full] yes` -- the literal `yes` is the safety latch. */
	if (argc >= 2 && strcmp(argv[1], "full") == 0)
		full = 1;
	if (argc != 2 + full || strcmp(argv[1 + full], "yes") != 0) {
		cli_error(sh, "fs: destructive; confirm with `fs format %syes`\r\n",
		          full ? "full " : "");
		return 1;
	}

	/* Own the media for the whole format: from here until exclusive_end no
	 * other thread can (re)mount and the destructive qspi cmds stay refused. */
	status = fs_exclusive_begin();
	if (status != FX_SUCCESS) {
		cli_error(sh, "fs: %s\r\n", fs_strerror(status));
		return 1;
	}

	/* Quiesce the stack: flush + close the media and LevelX. */
	(void)fs_media_unmount();

	if (!full) {
		/* Reuse intact LevelX structures (keeps erase-count history and
		 * finishes in seconds); fall back to the full erase if the flash
		 * is virgin or holds non-LevelX content (factory demo). */
		if (fx_lx_nor_open() != LX_SUCCESS) {
			cli_warn(sh, "fs: not LevelX-formatted; full erase needed (~3 min)\r\n");
			full = 1;
		}
	}
	if (full) {
		(void)fx_lx_nor_close();
		if (fs_erase_all(sh) != 0)
			goto out;
		if (fx_lx_nor_open() != LX_SUCCESS) {
			cli_error(sh, "fs: LevelX open failed after erase\r\n");
			goto out;
		}
	}

	sectors = fx_lx_nor_format_sectors();
	cli_print(sh, "formatting: %lu sectors x 512 B (%lu KiB)...\r\n",
	          (unsigned long)sectors, (unsigned long)(sectors / 2u));

	/* 1 FAT, 256 root entries, 512 B/sector, 1 sector/cluster -> ~32 K
	 * clusters, which puts FileX in FAT16 territory.  fx_media_format ends
	 * with FX_DRIVER_UNINIT (LevelX closed again). */
	status = fx_media_format(fs_glue_media(), fx_lx_nor_qspi_driver, FX_NULL,
	                         fs_glue_cache(), fs_glue_cache_size(),
	                         "QSPI", 1, 256, 0, sectors, 512, 1, 1, 1);
	if (status != FX_SUCCESS) {
		cli_error(sh, "fs: format failed: %s (0x%02x)\r\n",
		          fs_strerror(status), status);
		goto out;
	}
	rc = 0;

out:
	fs_exclusive_end();
	if (rc != 0)
		return 1;
	if (fs_core_mount(&qspi_dev, sh) == NULL)
		return 1;
	cli_print(sh, "formatted and mounted\r\n");
	return 0;
}

/* ---- thin wrappers: bind qspi_dev to the shared command bodies ------------ */

static int cmd_fs_ls(struct cli_instance *sh, int argc, char **argv)
{
	return fs_core_ls(&qspi_dev, sh, argc, argv);
}

static int cmd_fs_cat(struct cli_instance *sh, int argc, char **argv)
{
	return fs_core_cat(&qspi_dev, sh, argc, argv);
}

static int cmd_fs_write(struct cli_instance *sh, int argc, char **argv)
{
	return fs_core_write(&qspi_dev, sh, argc, argv);
}

static int cmd_fs_rm(struct cli_instance *sh, int argc, char **argv)
{
	return fs_core_rm(&qspi_dev, sh, argc, argv);
}

static int cmd_fs_mkdir(struct cli_instance *sh, int argc, char **argv)
{
	return fs_core_mkdir(&qspi_dev, sh, argc, argv);
}

static int cmd_fs_info(struct cli_instance *sh, int argc, char **argv)
{
	return fs_core_info(&qspi_dev, sh, argc, argv);
}

static int cmd_fs_umount(struct cli_instance *sh, int argc, char **argv)
{
	return fs_core_umount(&qspi_dev, sh, argc, argv);
}

CLI_SUBCMD_SET_CREATE(fs_subcmds,
	CLI_CMD_ARG(format, NULL, "format [full] -- confirm with 'yes'", cmd_fs_format, 1, 2),
	CLI_CMD_ARG(ls,     NULL, "list directory [path]",              cmd_fs_ls,     1, 1),
	CLI_CMD_ARG(cat,    NULL, "print file <path>",                  cmd_fs_cat,    2, 0),
	CLI_CMD_ARG(write,  NULL, "write <path> <text>",                cmd_fs_write,  3, 0),
	CLI_CMD_ARG(rm,     NULL, "delete file/empty dir <path>",       cmd_fs_rm,     2, 0),
	CLI_CMD_ARG(mkdir,  NULL, "create directory <path>",            cmd_fs_mkdir,  2, 0),
	CLI_CMD_ARG(info,   NULL, "capacity / free / state",            cmd_fs_info,   1, 0),
	CLI_CMD_ARG(umount, NULL, "flush and unmount",                  cmd_fs_umount, 1, 0),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(fs, fs_subcmds,
                 "QSPI flash filesystem (FileX + LevelX)", NULL, 1, 0);
