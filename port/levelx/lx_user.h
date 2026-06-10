/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    lx_user.h
 * @brief   LevelX build configuration for the QSPI NOR filesystem (issue #30).
 *
 * Pulled in by lx_api.h via LX_INCLUDE_USER_DEFINE_FILE (CMakeLists.txt).
 *
 * Deliberately NOT defined:
 *   - LX_DIRECT_READ: the QSPI flash is not memory-mapped in this firmware
 *     (indirect mode only, #29), so LevelX must read through the driver into a
 *     RAM sector buffer.  This is also what keeps the D-cache out of the
 *     picture entirely -- no CPU access to 0x90000000 ever happens.
 *   - LX_FREE_SECTOR_DATA_VERIFY: skip the free-sector scan at open; erased
 *     state is verified per block through the erased-verify driver callback.
 */
#ifndef LX_USER_H
#define LX_USER_H

/* The shell never enables the extended cache; drop its arrays/code from
 * LX_NOR_FLASH (the per-open RAM win is several KB). */
#define LX_NOR_DISABLE_EXTENDED_CACHE

/* Guard LevelX internals with their own ThreadX mutex.  The FileX media mutex
 * already serializes the `fs` command paths, but this keeps any future direct
 * lx_* caller (diagnostics, wear stats) safe too. */
#define LX_THREAD_SAFE_ENABLE

#endif /* LX_USER_H */
