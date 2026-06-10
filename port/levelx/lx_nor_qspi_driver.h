/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    lx_nor_qspi_driver.h
 * @brief   LevelX NOR driver glue for the on-board QSPI flash (issue #30).
 */
#ifndef LX_NOR_QSPI_DRIVER_H
#define LX_NOR_QSPI_DRIVER_H

#include "lx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LevelX physical-block geometry: one block = one 64 KB erase sector. */
#define LX_QSPI_TOTAL_BLOCKS     256u
#define LX_QSPI_WORDS_PER_BLOCK  (0x10000u / sizeof(ULONG))   /* 16384 */

/** Driver-initialize callback for lx_nor_flash_open(). */
UINT lx_nor_qspi_driver_initialize(LX_NOR_FLASH *nor_flash);

#ifdef __cplusplus
}
#endif

#endif /* LX_NOR_QSPI_DRIVER_H */
