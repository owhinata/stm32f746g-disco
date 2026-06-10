/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    fx_lx_nor_driver.c
 * @brief   FileX media driver entry on LevelX NOR over QSPI (issue #30).
 *
 * Implements the FileX driver-request contract on lx_nor_flash_sector_*
 * services: FileX logical sector N maps 1:1 to LevelX logical sector N, the
 * boot record lives in sector 0, and FX_DRIVER_RELEASE_SECTORS forwards
 * free-cluster notifications so LevelX can reclaim obsolete sectors (that is
 * what fx_media_driver_free_sector_update enables).  Semantics follow the
 * documented LevelX<->FileX driver contract (the levelx sample driver was used
 * as a reference for the request set; clean-room rewrite, no code reused).
 *
 * The LX_NOR_FLASH instance is owned here and opened/closed idempotently so
 * both the FileX driver path (INIT/UNINIT) and the `fs format` orchestration
 * (which needs the usable-sector count *before* fx_media_format) share one
 * instance.  Thread safety: LX_THREAD_SAFE_ENABLE guards lx internals, the
 * FileX media mutex guards the driver path, and fs_glue's mount mutex guards
 * the open/close transitions.
 */
#include "fx_lx_nor_driver.h"
#include "lx_nor_qspi_driver.h"

#define LOG_TAG "fxlx"
#include "log.h"

static LX_NOR_FLASH lx_nor;
static UINT         lx_is_open;

UINT fx_lx_nor_open(void)
{
	UINT status;

	if (lx_is_open)
		return LX_SUCCESS;
	status = lx_nor_flash_open(&lx_nor, "qspi", lx_nor_qspi_driver_initialize);
	if (status == LX_SUCCESS)
		lx_is_open = 1;
	else
		LOG_WRN("lx_nor_flash_open failed (%u) -- not LevelX-formatted?", status);
	return status;
}

UINT fx_lx_nor_close(void)
{
	UINT status;

	if (!lx_is_open)
		return LX_SUCCESS;
	status = lx_nor_flash_close(&lx_nor);
	lx_is_open = 0;
	return status;
}

LX_NOR_FLASH *fx_lx_nor_flash(void)
{
	return lx_is_open ? &lx_nor : LX_NULL;
}

ULONG fx_lx_nor_format_sectors(void)
{
	if (!lx_is_open)
		return 0;
	/* Hold one block of physical sectors back from the filesystem so LevelX
	 * always has reclaim slack even with every FileX sector live. */
	return lx_nor.lx_nor_flash_total_physical_sectors -
	       lx_nor.lx_nor_flash_physical_sectors_per_block;
}

VOID fx_lx_nor_qspi_driver(FX_MEDIA *media_ptr)
{
	UCHAR *buffer = (UCHAR *)media_ptr->fx_media_driver_buffer;
	ULONG  sector = media_ptr->fx_media_driver_logical_sector;
	ULONG  count  = media_ptr->fx_media_driver_sectors;
	ULONG  i;

	media_ptr->fx_media_driver_status = FX_IO_ERROR;

	switch (media_ptr->fx_media_driver_request) {
	case FX_DRIVER_INIT:
		/* Tell FileX to report freed clusters (RELEASE_SECTORS), which is
		 * what lets LevelX mark obsolete sectors reclaimable. */
		media_ptr->fx_media_driver_free_sector_update = FX_TRUE;
		if (fx_lx_nor_open() != LX_SUCCESS)
			return;
		media_ptr->fx_media_driver_status = FX_SUCCESS;
		break;

	case FX_DRIVER_UNINIT:
		if (fx_lx_nor_close() != LX_SUCCESS)
			return;
		media_ptr->fx_media_driver_status = FX_SUCCESS;
		break;

	case FX_DRIVER_READ:
		for (i = 0; i < count; i++) {
			if (lx_nor_flash_sector_read(&lx_nor, sector + i,
			                             buffer) != LX_SUCCESS)
				return;
			buffer += media_ptr->fx_media_bytes_per_sector;
		}
		media_ptr->fx_media_driver_status = FX_SUCCESS;
		break;

	case FX_DRIVER_WRITE:
		for (i = 0; i < count; i++) {
			if (lx_nor_flash_sector_write(&lx_nor, sector + i,
			                              buffer) != LX_SUCCESS)
				return;
			buffer += media_ptr->fx_media_bytes_per_sector;
		}
		media_ptr->fx_media_driver_status = FX_SUCCESS;
		break;

	case FX_DRIVER_RELEASE_SECTORS:
		for (i = 0; i < count; i++) {
			if (lx_nor_flash_sector_release(&lx_nor, sector + i)
			    != LX_SUCCESS)
				return;
		}
		media_ptr->fx_media_driver_status = FX_SUCCESS;
		break;

	case FX_DRIVER_BOOT_READ:
		if (lx_nor_flash_sector_read(&lx_nor, 0, buffer) != LX_SUCCESS)
			return;
		/* FileX leaves the boot-signature check to the driver (its own
		 * boot_info_extract only validates the BPB fields). */
		if (buffer[510] != 0x55u || buffer[511] != 0xAAu) {
			media_ptr->fx_media_driver_status = FX_MEDIA_INVALID;
			return;
		}
		media_ptr->fx_media_driver_status = FX_SUCCESS;
		break;

	case FX_DRIVER_BOOT_WRITE:
		if (media_ptr->fx_media_bytes_per_sector !=
		    LX_NOR_SECTOR_SIZE * sizeof(ULONG))
			return;                 /* only 512 B sectors are wired up */
		if (lx_nor_flash_sector_write(&lx_nor, 0, buffer) != LX_SUCCESS)
			return;
		media_ptr->fx_media_driver_status = FX_SUCCESS;
		break;

	case FX_DRIVER_FLUSH:
	case FX_DRIVER_ABORT:
		/* Sector writes are synchronous all the way to the flash. */
		media_ptr->fx_media_driver_status = FX_SUCCESS;
		break;

	default:
		LOG_WRN("unhandled driver request %u",
		        media_ptr->fx_media_driver_request);
		break;
	}
}
