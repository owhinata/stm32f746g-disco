/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    fx_sd_driver.h
 * @brief   FileX media driver on the SDMMC1 block API (issue #34, Epic #32).
 *
 * Maps FileX driver requests onto sd_card_read/write_blocks().  The SD card has
 * its own wear-leveling FTL, so there is no LevelX layer.  Because PC/camera
 * tools format microSD with an MBR + a FAT partition (the VBR is not at LBA 0),
 * the driver presents partition 0 as a superfloppy: INIT reads LBA 0, finds the
 * partition start (sd_part_lba) and size, and every request adds sd_part_lba so
 * FileX -- which never computes a partition offset itself -- sees a flat volume.
 */
#ifndef FX_SD_DRIVER_H
#define FX_SD_DRIVER_H

#include "fx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/** FileX media driver entry (pass to fx_media_open / fx_media_format). */
VOID fx_sd_driver(FX_MEDIA *media_ptr);

#ifdef __cplusplus
}
#endif

#endif /* FX_SD_DRIVER_H */
