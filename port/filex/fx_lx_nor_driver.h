/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    fx_lx_nor_driver.h
 * @brief   FileX media driver on LevelX NOR over QSPI (issue #30).
 */
#ifndef FX_LX_NOR_DRIVER_H
#define FX_LX_NOR_DRIVER_H

#include "fx_api.h"
#include "lx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/** FileX media driver entry (pass to fx_media_open / fx_media_format). */
VOID fx_lx_nor_qspi_driver(FX_MEDIA *media_ptr);

/**
 * Open/close the underlying LevelX instance.  Idempotent; the driver INIT /
 * UNINIT requests call these too.  Opening scans the 256 block headers; on a
 * fully erased device LevelX formats its own structures, on garbage content
 * (e.g. the factory demo data) it returns LX_ERROR -- erase first.
 */
UINT fx_lx_nor_open(void);
UINT fx_lx_nor_close(void);

/** The LevelX instance (for wear/diagnostic queries; open it first). */
LX_NOR_FLASH *fx_lx_nor_flash(void);

/**
 * Logical sector count to pass to fx_media_format: usable physical sectors
 * minus one block's worth held back as wear-leveling/compaction slack.
 * Requires fx_lx_nor_open() to have succeeded.
 */
ULONG fx_lx_nor_format_sectors(void);

#ifdef __cplusplus
}
#endif

#endif /* FX_LX_NOR_DRIVER_H */
