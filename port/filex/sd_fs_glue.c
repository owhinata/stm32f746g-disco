/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    sd_fs_glue.c
 * @brief   Lazy-mount singleton for the microSD filesystem (issue #34).
 *
 * Cloned from fs_glue.c (the QSPI lazy-mount singleton) for the SD media: a
 * separate FX_MEDIA / cache / mutex set so the two filesystems are independent.
 * No LevelX (the SD card has its own FTL), and fx_system_initialize() is NOT
 * called here -- fs_glue_init() owns that.  The raw slot gates `sd info`/`sd
 * read`, which re-probe the card and must not run while the media is mounted.
 *
 * The media cache is plain cached SRAM: fx_sd_driver hands every buffer to
 * sd_card_read/write_blocks, which bounces through its own SRAM1 DMA buffer and
 * does the D-cache maintenance, so no special placement is needed here.
 */
#include "sd_fs_glue.h"
#include "fs_glue.h"        /* FS_ERR_BUSY (shared sentinel) */
#include "fx_sd_driver.h"

#include "tx_api.h"

#define LOG_TAG "sdfs"
#include "log.h"

static FX_MEDIA sd_media;
static UCHAR    sd_media_cache[4096];
static TX_MUTEX sd_mount_lock;
static TX_MUTEX sd_dir_mutex;
static int      sd_mounted;
static int      sd_busy;        /* umount/raw owns the media               */
static int      sd_active_ops;  /* FS commands currently inside a body     */

void sd_fs_glue_init(void)
{
	tx_mutex_create(&sd_mount_lock, "sd_mount", TX_INHERIT);
	tx_mutex_create(&sd_dir_mutex, "sd_dir", TX_INHERIT);
	/* fx_system_initialize() is run once by fs_glue_init(); do not repeat it. */
}

FX_MEDIA *sd_glue_media(void)
{
	return &sd_media;
}

UCHAR *sd_glue_cache(void)
{
	return sd_media_cache;
}

ULONG sd_glue_cache_size(void)
{
	return sizeof sd_media_cache;
}

int sd_is_mounted(void)
{
	return sd_mounted;
}

int sd_is_busy(void)
{
	return sd_busy;
}

UINT sd_op_begin(void)
{
	UINT status = FX_SUCCESS;

	tx_mutex_get(&sd_mount_lock, TX_WAIT_FOREVER);
	if (sd_busy)
		status = FS_ERR_BUSY;
	else
		sd_active_ops++;
	tx_mutex_put(&sd_mount_lock);
	return status;
}

void sd_op_end(void)
{
	tx_mutex_get(&sd_mount_lock, TX_WAIT_FOREVER);
	sd_active_ops--;
	tx_mutex_put(&sd_mount_lock);
}

/* Exclusive owner; @p require_unmounted additionally refuses while mounted
 * (card re-probe / format must never run under a live filesystem). */
static UINT sd_owner_begin(int require_unmounted)
{
	UINT status = FX_SUCCESS;

	tx_mutex_get(&sd_mount_lock, TX_WAIT_FOREVER);
	if (sd_busy || sd_active_ops > 0 || (require_unmounted && sd_mounted))
		status = FS_ERR_BUSY;
	else
		sd_busy = 1;
	tx_mutex_put(&sd_mount_lock);
	return status;
}

UINT sd_exclusive_begin(void)
{
	return sd_owner_begin(0);
}

UINT sd_raw_begin(void)
{
	return sd_owner_begin(1);
}

void sd_exclusive_end(void)
{
	tx_mutex_get(&sd_mount_lock, TX_WAIT_FOREVER);
	sd_busy = 0;
	tx_mutex_put(&sd_mount_lock);
}

void sd_raw_end(void)
{
	sd_exclusive_end();
}

void sd_dir_lock(void)
{
	tx_mutex_get(&sd_dir_mutex, TX_WAIT_FOREVER);
}

void sd_dir_unlock(void)
{
	tx_mutex_put(&sd_dir_mutex);
}

UINT sd_media_acquire(FX_MEDIA **out)
{
	UINT status = FX_SUCCESS;

	tx_mutex_get(&sd_mount_lock, TX_WAIT_FOREVER);

	if (sd_busy) {
		/* umount/raw owns the media; do not mount behind its back. */
		tx_mutex_put(&sd_mount_lock);
		return FS_ERR_BUSY;
	}
	if (!sd_mounted) {
		status = fx_media_open(&sd_media, "sd", fx_sd_driver,
		                       FX_NULL, sd_media_cache,
		                       sizeof sd_media_cache);
		if (status == FX_SUCCESS) {
			sd_mounted = 1;
			LOG_INF("media mounted");
		}
	}

	tx_mutex_put(&sd_mount_lock);

	if (status == FX_SUCCESS && out != FX_NULL)
		*out = &sd_media;
	return status;
}

UINT sd_media_unmount(void)
{
	UINT status = FX_SUCCESS;

	tx_mutex_get(&sd_mount_lock, TX_WAIT_FOREVER);

	if (sd_mounted) {
		/* fx_media_close flushes and sends FX_DRIVER_UNINIT. */
		status = fx_media_close(&sd_media);
		sd_mounted = 0;
		if (status == FX_SUCCESS)
			LOG_INF("media unmounted");
		else
			LOG_ERR("fx_media_close failed (%u)", status);
	}

	tx_mutex_put(&sd_mount_lock);
	return status;
}
