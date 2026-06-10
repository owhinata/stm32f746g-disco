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
 *   fs info                capacity / free space / FAT type
 *   fs umount              flush + unmount (e.g. before `qspi test`)
 *
 * The media mounts lazily on first use (port/filex/fs_glue.c); on a virgin or
 * non-LevelX flash every command fails with a hint to run `fs format`.  Every
 * mutating command ends with fx_media_flush(), so a `reboot` (or power cycle)
 * any time later finds consistent content -- no explicit unmount needed.
 * Paths are absolute (FX_NO_LOCAL_PATH).  All output is via the cli_* API and
 * the only state is the mutex-guarded media singleton, so the commands are
 * safe from background jobs too (issue #25).
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "fs_glue.h"
#include "fx_lx_nor_driver.h"
#include "lx_nor_qspi_driver.h"
#include "qspi_flash.h"

#include <string.h>

/* One read/copy unit for `fs cat`; small enough for the 4 KB shell stack. */
#define FS_CAT_CHUNK 256u

static const char *fs_strerror(UINT status)
{
	switch (status) {
	case FS_ERR_BUSY:        return "busy (format in progress)";
	case FX_BOOT_ERROR:      return "invalid boot record (run `fs format`)";
	case FX_MEDIA_INVALID:   return "media invalid (run `fs format`)";
	case FX_NOT_FOUND:       return "not found";
	case FX_NOT_A_FILE:      return "not a file";
	case FX_ACCESS_ERROR:    return "access error";
	case FX_FILE_CORRUPT:    return "file corrupt";
	case FX_NO_MORE_SPACE:   return "no space left";
	case FX_ALREADY_CREATED: return "already exists";
	case FX_INVALID_NAME:    return "invalid name";
	case FX_NOT_DIRECTORY:   return "not a directory";
	case FX_DIR_NOT_EMPTY:   return "directory not empty";
	case FX_MEDIA_NOT_OPEN:  return "media not open";
	case FX_WRITE_PROTECT:   return "write protected";
	case FX_IO_ERROR:        return "I/O error";
	default:                 return "filesystem error";
	}
}

/*
 * Run one normal fs subcommand body under a shared op slot, so `fs format` /
 * `fs umount` / raw qspi operations can never yank the media away mid-command
 * (they fail with FS_ERR_BUSY instead while any body is inside).
 */
static int fs_run_op(struct cli_instance *sh, int argc, char **argv,
                     int (*body)(struct cli_instance *, int, char **))
{
	UINT status = fs_op_begin();
	int rc;

	if (status != FX_SUCCESS) {
		cli_error(sh, "fs: %s\r\n", fs_strerror(status));
		return 1;
	}
	rc = body(sh, argc, argv);
	fs_op_end();
	return rc;
}

/* Mount-on-demand wrapper shared by every subcommand that needs the media. */
static FX_MEDIA *fs_mount(struct cli_instance *sh)
{
	FX_MEDIA *media;
	UINT status;

	status = fs_media_acquire(&media);
	if (status != FX_SUCCESS) {
		cli_error(sh, "fs: mount failed: %s (0x%02x)\r\n",
		          fs_strerror(status), status);
		if (status != FS_ERR_BUSY)
			cli_error(sh, "fs: not formatted? run `fs format yes`\r\n");
		return NULL;
	}
	return media;
}

/* ---- fs format ------------------------------------------------------------ */

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
	if (fs_mount(sh) == NULL)
		return 1;
	cli_print(sh, "formatted and mounted\r\n");
	return 0;
}

/* ---- fs ls ---------------------------------------------------------------- */

static int do_fs_ls(struct cli_instance *sh, int argc, char **argv)
{
	char name[FX_MAX_LONG_NAME_LEN];
	const char *path = (argc >= 2) ? argv[1] : "/";
	FX_MEDIA *media = fs_mount(sh);
	UINT attr, status;
	ULONG size;
	int rc = 0;

	if (media == NULL)
		return 1;

	/* The default directory is media-global and the iteration spans many
	 * FileX calls; serialize whole listings against each other (bg jobs). */
	fs_dir_lock();

	status = fx_directory_default_set(media, (CHAR *)path);
	if (status != FX_SUCCESS) {
		cli_error(sh, "fs: %s: %s (0x%02x)\r\n",
		          path, fs_strerror(status), status);
		fs_dir_unlock();
		return 1;
	}

	status = fx_directory_first_full_entry_find(media, name, &attr, &size,
	                                            NULL, NULL, NULL,
	                                            NULL, NULL, NULL);
	while (status == FX_SUCCESS) {
		if (cli_cancel_requested(sh)) {
			rc = 1;
			break;
		}
		if (cli_print(sh, "%c %8lu  %s\r\n",
		              (attr & FX_DIRECTORY) ? 'd' : '-',
		              (unsigned long)size, name) < 0) {
			rc = 1;
			break;
		}
		status = fx_directory_next_full_entry_find(media, name, &attr, &size,
		                                           NULL, NULL, NULL,
		                                           NULL, NULL, NULL);
	}
	if (status != FX_SUCCESS && status != FX_NO_MORE_ENTRIES && rc == 0) {
		cli_error(sh, "fs: ls failed: %s (0x%02x)\r\n",
		          fs_strerror(status), status);
		rc = 1;
	}

	(void)fx_directory_default_set(media, "/");
	fs_dir_unlock();
	return rc;
}

/* ---- fs cat --------------------------------------------------------------- */

static int do_fs_cat(struct cli_instance *sh, int argc, char **argv)
{
	char buf[FS_CAT_CHUNK];
	FX_MEDIA *media = fs_mount(sh);
	FX_FILE file;
	ULONG got;
	UINT status;
	int rc = 0;

	(void)argc;

	if (media == NULL)
		return 1;

	status = fx_file_open(media, &file, argv[1], FX_OPEN_FOR_READ);
	if (status != FX_SUCCESS) {
		cli_error(sh, "fs: %s: %s (0x%02x)\r\n",
		          argv[1], fs_strerror(status), status);
		return 1;
	}

	for (;;) {
		if (cli_cancel_requested(sh)) {
			rc = 1;
			break;
		}
		status = fx_file_read(&file, buf, sizeof buf, &got);
		if (status == FX_END_OF_FILE)
			break;
		if (status != FX_SUCCESS) {
			cli_error(sh, "fs: read failed: %s (0x%02x)\r\n",
			          fs_strerror(status), status);
			rc = 1;
			break;
		}
		if (cli_write(sh, buf, got) < 0) {
			rc = 1;
			break;
		}
	}
	(void)fx_file_close(&file);
	if (rc == 0)
		cli_print(sh, "\r\n");
	return rc;
}

/* ---- fs write ------------------------------------------------------------- */

static int do_fs_write(struct cli_instance *sh, int argc, char **argv)
{
	FX_MEDIA *media = fs_mount(sh);
	FX_FILE file;
	ULONG len = (ULONG)strlen(argv[2]);
	UINT status;

	(void)argc;

	if (media == NULL)
		return 1;

	status = fx_file_create(media, argv[1]);
	if (status != FX_SUCCESS && status != FX_ALREADY_CREATED) {
		cli_error(sh, "fs: create %s: %s (0x%02x)\r\n",
		          argv[1], fs_strerror(status), status);
		return 1;
	}

	status = fx_file_open(media, &file, argv[1], FX_OPEN_FOR_WRITE);
	if (status != FX_SUCCESS) {
		cli_error(sh, "fs: open %s: %s (0x%02x)\r\n",
		          argv[1], fs_strerror(status), status);
		return 1;
	}

	/* Overwrite semantics: truncate, write, close, flush to flash. */
	status = fx_file_truncate(&file, 0);
	if (status == FX_SUCCESS && len > 0)
		status = fx_file_write(&file, argv[2], len);
	(void)fx_file_close(&file);
	if (status == FX_SUCCESS)
		status = fx_media_flush(media);

	if (status != FX_SUCCESS) {
		cli_error(sh, "fs: write failed: %s (0x%02x)\r\n",
		          fs_strerror(status), status);
		return 1;
	}
	cli_print(sh, "wrote %lu bytes to %s\r\n", (unsigned long)len, argv[1]);
	return 0;
}

/* ---- fs rm / mkdir -------------------------------------------------------- */

static int do_fs_rm(struct cli_instance *sh, int argc, char **argv)
{
	FX_MEDIA *media = fs_mount(sh);
	UINT status;

	(void)argc;

	if (media == NULL)
		return 1;

	status = fx_file_delete(media, argv[1]);
	if (status == FX_NOT_A_FILE)
		status = fx_directory_delete(media, argv[1]);
	if (status == FX_SUCCESS)
		status = fx_media_flush(media);

	if (status != FX_SUCCESS) {
		cli_error(sh, "fs: rm %s: %s (0x%02x)\r\n",
		          argv[1], fs_strerror(status), status);
		return 1;
	}
	cli_print(sh, "removed %s\r\n", argv[1]);
	return 0;
}

static int do_fs_mkdir(struct cli_instance *sh, int argc, char **argv)
{
	FX_MEDIA *media = fs_mount(sh);
	UINT status;

	(void)argc;

	if (media == NULL)
		return 1;

	status = fx_directory_create(media, argv[1]);
	if (status == FX_SUCCESS)
		status = fx_media_flush(media);

	if (status != FX_SUCCESS) {
		cli_error(sh, "fs: mkdir %s: %s (0x%02x)\r\n",
		          argv[1], fs_strerror(status), status);
		return 1;
	}
	cli_print(sh, "created %s\r\n", argv[1]);
	return 0;
}

/* ---- fs info / umount ------------------------------------------------------ */

static int do_fs_info(struct cli_instance *sh, int argc, char **argv)
{
	FX_MEDIA *media;
	LX_NOR_FLASH *lx;
	ULONG avail = 0, total_bytes, clusters;
	UINT status;

	(void)argc; (void)argv;

	if (!fs_is_mounted())
		cli_print(sh, "state    : unmounted (mounting...)\r\n");
	media = fs_mount(sh);
	if (media == NULL)
		return 1;

	clusters    = media->fx_media_total_clusters;
	total_bytes = clusters * media->fx_media_sectors_per_cluster *
	              media->fx_media_bytes_per_sector;
	status = fx_media_space_available(media, &avail);
	if (status != FX_SUCCESS) {
		cli_error(sh, "fs: space query failed: %s (0x%02x)\r\n",
		          fs_strerror(status), status);
		return 1;
	}

	cli_print(sh, "state    : mounted\r\n");
	cli_print(sh, "fat      : FAT%s (%lu clusters)\r\n",
	          (clusters < 4085u) ? "12" : (clusters < 65525u) ? "16" : "32",
	          (unsigned long)clusters);
	cli_print(sh, "cluster  : %lu B\r\n",
	          (unsigned long)(media->fx_media_sectors_per_cluster *
	                          media->fx_media_bytes_per_sector));
	cli_print(sh, "total    : %lu KiB\r\n", (unsigned long)(total_bytes / 1024u));
	cli_print(sh, "free     : %lu KiB\r\n", (unsigned long)(avail / 1024u));

	/* Wear-leveling visibility (issue #31): LevelX keeps per-block erase
	 * counts in the block headers and tracks min/max live (updated at open
	 * and on every reclaim), so the spread shows leveling at work. */
	lx = fx_lx_nor_flash();
	if (lx != NULL)
		cli_print(sh, "wear     : erase count min %lu / max %lu\r\n",
		          (unsigned long)lx->lx_nor_flash_minimum_erase_count,
		          (unsigned long)lx->lx_nor_flash_maximum_erase_count);
	return 0;
}

static int cmd_fs_umount(struct cli_instance *sh, int argc, char **argv)
{
	UINT status;
	int rc = 0;

	(void)argc; (void)argv;

	/* Unmount closes the media (and any open files with it), so it needs the
	 * same exclusive ownership as format: refused while a command is inside. */
	status = fs_exclusive_begin();
	if (status != FX_SUCCESS) {
		cli_error(sh, "fs: %s\r\n", fs_strerror(status));
		return 1;
	}

	if (!fs_is_mounted()) {
		cli_print(sh, "not mounted\r\n");
	} else {
		status = fs_media_unmount();
		if (status != FX_SUCCESS) {
			cli_error(sh, "fs: unmount failed: %s (0x%02x)\r\n",
			          fs_strerror(status), status);
			rc = 1;
		} else {
			cli_print(sh, "unmounted\r\n");
		}
	}

	fs_exclusive_end();
	return rc;
}

/* Thin wrappers: every normal subcommand body runs under a shared op slot. */
static int cmd_fs_ls(struct cli_instance *sh, int argc, char **argv)
{
	return fs_run_op(sh, argc, argv, do_fs_ls);
}

static int cmd_fs_cat(struct cli_instance *sh, int argc, char **argv)
{
	return fs_run_op(sh, argc, argv, do_fs_cat);
}

static int cmd_fs_write(struct cli_instance *sh, int argc, char **argv)
{
	return fs_run_op(sh, argc, argv, do_fs_write);
}

static int cmd_fs_rm(struct cli_instance *sh, int argc, char **argv)
{
	return fs_run_op(sh, argc, argv, do_fs_rm);
}

static int cmd_fs_mkdir(struct cli_instance *sh, int argc, char **argv)
{
	return fs_run_op(sh, argc, argv, do_fs_mkdir);
}

static int cmd_fs_info(struct cli_instance *sh, int argc, char **argv)
{
	return fs_run_op(sh, argc, argv, do_fs_info);
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
