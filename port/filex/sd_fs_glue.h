/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    sd_fs_glue.h
 * @brief   Lazy-mount singleton for the microSD filesystem (issue #34).
 *
 * The microSD analogue of fs_glue.h: one FX_MEDIA over fx_sd_driver, mounted on
 * first use, with the same reader/writer ownership model.  Independent of the
 * QSPI media (separate FX_MEDIA / cache / mutexes), so the two filesystems do
 * not interfere.  fx_system_initialize() is owned by fs_glue_init() and is NOT
 * called again here.  Thread-context only.
 */
#ifndef SD_FS_GLUE_H
#define SD_FS_GLUE_H

#include "fx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create the SD glue mutexes.  Call once from tx_application_define(), after
 *  fs_glue_init() (which runs fx_system_initialize). */
void sd_fs_glue_init(void);

/** Return the mounted media in *out, mounting (fx_media_open) on first use.
 *  Returns FX_SUCCESS or the failing fx status; a card without a FAT volume
 *  fails -- there is no SD format yet (Phase C). */
UINT sd_media_acquire(FX_MEDIA **out);

/** Flush + unmount (fx_media_close).  No-op when not mounted. */
UINT sd_media_unmount(void);

/** Nonzero while the SD media is mounted. */
int sd_is_mounted(void);

/** Nonzero while format/umount/raw owns the SD media. */
int sd_is_busy(void);

/*
 * Media ownership, reader/writer style (mirrors fs_glue): every FS command
 * holds a shared op slot; umount takes the exclusive slot; the card-disruptive
 * `sd info`/`sd read` (which re-probe the card) take the raw slot, additionally
 * refused while the media is mounted.  begin calls return FX_SUCCESS or
 * FS_ERR_BUSY and never block; pair every successful begin with its end.
 */
UINT sd_op_begin(void);
void sd_op_end(void);
UINT sd_exclusive_begin(void);
void sd_exclusive_end(void);
UINT sd_raw_begin(void);
void sd_raw_end(void);

/** Serialize multi-call directory sequences (`sd ls`). */
void sd_dir_lock(void);
void sd_dir_unlock(void);

/** The media singleton + its sector cache (for a future `sd format`). */
FX_MEDIA *sd_glue_media(void);
UCHAR    *sd_glue_cache(void);
ULONG     sd_glue_cache_size(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_FS_GLUE_H */
