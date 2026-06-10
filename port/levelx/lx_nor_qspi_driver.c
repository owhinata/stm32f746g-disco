/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    lx_nor_qspi_driver.c
 * @brief   LevelX NOR driver glue: LX_NOR_FLASH <-> qspi_flash (issue #30).
 *
 * Geometry: 256 LevelX blocks, one per 64 KB erase sector of the N25Q128A
 * (16 MB total), 512 B logical sectors.  64 KB blocks keep the LevelX metadata
 * overhead near 1.6 % (vs 12.5 % with 4 KB subsectors) and a full-device erase
 * in minutes rather than tens of minutes; wear granularity is coarser, which is
 * fine for a 16 MB configuration-data store.
 *
 * Address convention: lx_nor_flash_base_address is 0, so the ULONG* flash
 * addresses LevelX passes to the callbacks are plain byte offsets into the
 * device (LevelX advances them with ULONG pointer arithmetic, i.e. 4 bytes per
 * word).  No memory-mapped window is involved (LX_DIRECT_READ undefined).
 *
 * LevelX metadata updates re-program words inside already-programmed pages,
 * clearing further bits each time (never 0 -> 1).  The N25Q128A explicitly
 * allows this: "bits are programmed from one through zero", with no documented
 * limit on program operations per page between erases (datasheet rev M).
 *
 * Thread context only (the qspi_flash layer sleeps while erasing); LevelX'
 * own LX_THREAD_SAFE_ENABLE mutex serializes callers.
 */
#include "lx_nor_qspi_driver.h"
#include "qspi_flash.h"

#define LOG_TAG "lx"
#include "log.h"

/* 512 B sector buffer LevelX requires when LX_DIRECT_READ is off, plus a
 * scratch buffer for the erased-verify sweep. */
static ULONG lx_sector_buffer[LX_NOR_SECTOR_SIZE];
static ULONG lx_verify_buffer[LX_NOR_SECTOR_SIZE];

static UINT lx_qspi_read(ULONG *flash_address, ULONG *destination, ULONG words)
{
	if (qspi_flash_read((uint32_t)(uintptr_t)flash_address, destination,
	                    words * sizeof(ULONG)) != 0)
		return LX_ERROR;
	return LX_SUCCESS;
}

static UINT lx_qspi_write(ULONG *flash_address, ULONG *source, ULONG words)
{
	if (qspi_flash_write((uint32_t)(uintptr_t)flash_address, source,
	                     words * sizeof(ULONG)) != 0)
		return LX_ERROR;
	return LX_SUCCESS;
}

static UINT lx_qspi_block_erase(ULONG block, ULONG erase_count)
{
	(void)erase_count;   /* wear info lives in the LevelX block header */

	if (qspi_flash_erase_sector(block * QSPI_FLASH_SECTOR_SIZE) != 0)
		return LX_ERROR;
	return LX_SUCCESS;
}

static UINT lx_qspi_block_erased_verify(ULONG block)
{
	uint32_t base = block * QSPI_FLASH_SECTOR_SIZE;
	uint32_t off, i;

	for (off = 0; off < QSPI_FLASH_SECTOR_SIZE; off += sizeof lx_verify_buffer) {
		if (qspi_flash_read(base + off, lx_verify_buffer,
		                    sizeof lx_verify_buffer) != 0)
			return LX_ERROR;
		for (i = 0; i < LX_NOR_SECTOR_SIZE; i++) {
			if (lx_verify_buffer[i] != 0xFFFFFFFFu)
				return LX_ERROR;
		}
	}
	return LX_SUCCESS;
}

static UINT lx_qspi_system_error(UINT error_code)
{
	/* Surfaced in `dmesg`; LevelX also fails the calling operation. */
	LOG_ERR("LevelX system error %u", error_code);
	return LX_SUCCESS;
}

UINT lx_nor_qspi_driver_initialize(LX_NOR_FLASH *nor_flash)
{
	if (qspi_flash_init() != 0)
		return LX_ERROR;

	/* Base 0: callback addresses are byte offsets into the device. */
	nor_flash->lx_nor_flash_base_address    = (ULONG *)0;
	nor_flash->lx_nor_flash_total_blocks    = LX_QSPI_TOTAL_BLOCKS;
	nor_flash->lx_nor_flash_words_per_block = LX_QSPI_WORDS_PER_BLOCK;

	nor_flash->lx_nor_flash_driver_read                = lx_qspi_read;
	nor_flash->lx_nor_flash_driver_write               = lx_qspi_write;
	nor_flash->lx_nor_flash_driver_block_erase         = lx_qspi_block_erase;
	nor_flash->lx_nor_flash_driver_block_erased_verify = lx_qspi_block_erased_verify;
	nor_flash->lx_nor_flash_driver_system_error        = lx_qspi_system_error;

	nor_flash->lx_nor_flash_sector_buffer = lx_sector_buffer;

	return LX_SUCCESS;
}
