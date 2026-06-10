/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    fs_glue.c
 * @brief   Lazy-mount singleton for the QSPI filesystem (issue #30).
 *
 * Why lazy: the LevelX open scans all 256 block headers (tens of ms of QSPI
 * traffic), and a virgin/garbage flash plainly fails to mount -- doing this on
 * the first `fs` command keeps boot untouched and turns the failure into a
 * visible command error ("run `fs format`") instead of a silent boot message.
 *
 * The 4 KB media cache gives FileX eight 512 B sector buffers -- enough to keep
 * the FAT, directory and data sector of a single-user CLI workload resident.
 */
#include "fs_glue.h"
#include "fx_lx_nor_driver.h"

#include "tx_api.h"

#define LOG_TAG "fs"
#include "log.h"

static FX_MEDIA fs_media;
static UCHAR    fs_media_cache[4096];
static TX_MUTEX fs_mount_lock;
static TX_MUTEX fs_dir_mutex;
static int      fs_mounted;
static int      fs_busy;        /* format/umount/raw owns the media        */
static int      fs_active_ops;  /* fs commands currently inside a body     */

void fs_glue_init(void)
{
	tx_mutex_create(&fs_mount_lock, "fs_mount", TX_INHERIT);
	tx_mutex_create(&fs_dir_mutex, "fs_dir", TX_INHERIT);
	fx_system_initialize();
}

FX_MEDIA *fs_glue_media(void)
{
	return &fs_media;
}

UCHAR *fs_glue_cache(void)
{
	return fs_media_cache;
}

ULONG fs_glue_cache_size(void)
{
	return sizeof fs_media_cache;
}

int fs_is_mounted(void)
{
	return fs_mounted;
}

int fs_is_busy(void)
{
	return fs_busy;
}

UINT fs_op_begin(void)
{
	UINT status = FX_SUCCESS;

	tx_mutex_get(&fs_mount_lock, TX_WAIT_FOREVER);
	if (fs_busy)
		status = FS_ERR_BUSY;
	else
		fs_active_ops++;
	tx_mutex_put(&fs_mount_lock);
	return status;
}

void fs_op_end(void)
{
	tx_mutex_get(&fs_mount_lock, TX_WAIT_FOREVER);
	fs_active_ops--;
	tx_mutex_put(&fs_mount_lock);
}

/* Exclusive owner; @p require_unmounted additionally refuses while mounted
 * (raw flash operations must never run under a live filesystem). */
static UINT fs_owner_begin(int require_unmounted)
{
	UINT status = FX_SUCCESS;

	tx_mutex_get(&fs_mount_lock, TX_WAIT_FOREVER);
	if (fs_busy || fs_active_ops > 0 || (require_unmounted && fs_mounted))
		status = FS_ERR_BUSY;
	else
		fs_busy = 1;
	tx_mutex_put(&fs_mount_lock);
	return status;
}

UINT fs_exclusive_begin(void)
{
	return fs_owner_begin(0);
}

UINT fs_raw_begin(void)
{
	return fs_owner_begin(1);
}

void fs_exclusive_end(void)
{
	tx_mutex_get(&fs_mount_lock, TX_WAIT_FOREVER);
	fs_busy = 0;
	tx_mutex_put(&fs_mount_lock);
}

void fs_raw_end(void)
{
	fs_exclusive_end();
}

void fs_dir_lock(void)
{
	tx_mutex_get(&fs_dir_mutex, TX_WAIT_FOREVER);
}

void fs_dir_unlock(void)
{
	tx_mutex_put(&fs_dir_mutex);
}

UINT fs_media_acquire(FX_MEDIA **out)
{
	UINT status = FX_SUCCESS;

	tx_mutex_get(&fs_mount_lock, TX_WAIT_FOREVER);

	if (fs_busy) {
		/* A format owns the media; do not mount behind its back. */
		tx_mutex_put(&fs_mount_lock);
		return FS_ERR_BUSY;
	}
	if (!fs_mounted) {
		status = fx_media_open(&fs_media, "qspi", fx_lx_nor_qspi_driver,
		                       FX_NULL, fs_media_cache,
		                       sizeof fs_media_cache);
		if (status == FX_SUCCESS) {
			fs_mounted = 1;
			LOG_INF("media mounted");
		}
	}

	tx_mutex_put(&fs_mount_lock);

	if (status == FX_SUCCESS && out != FX_NULL)
		*out = &fs_media;
	return status;
}

UINT fs_media_unmount(void)
{
	UINT status = FX_SUCCESS;

	tx_mutex_get(&fs_mount_lock, TX_WAIT_FOREVER);

	if (fs_mounted) {
		/* fx_media_close flushes and sends FX_DRIVER_UNINIT (lx close). */
		status = fx_media_close(&fs_media);
		fs_mounted = 0;
		if (status == FX_SUCCESS)
			LOG_INF("media unmounted");
		else
			LOG_ERR("fx_media_close failed (%u)", status);
	}
	/* Belt and braces: make sure LevelX is closed even if the media was
	 * never mounted (e.g. a failed format attempt left lx open). */
	(void)fx_lx_nor_close();

	tx_mutex_put(&fs_mount_lock);
	return status;
}
