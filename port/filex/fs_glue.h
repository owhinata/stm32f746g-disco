/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    fs_glue.h
 * @brief   QSPI filesystem mount state (lazy mount singleton, issue #30).
 *
 * One FX_MEDIA over the LevelX/QSPI stack, mounted on first use.  All calls
 * are thread-context only.  After fs_media_acquire() succeeds, fx_* calls on
 * the returned media are serialized by FileX's own media mutex; this layer's
 * mutex only guards the mount/unmount transitions.
 */
#ifndef FS_GLUE_H
#define FS_GLUE_H

#include "fx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returned by fs_media_acquire()/fs_exclusive_begin() while a format owns the
 * media.  Outside FileX's status space so it cannot be confused with one. */
#define FS_ERR_BUSY 0xF0u

/** Create the glue mutexes + fx_system_initialize().  Call once from
 *  tx_application_define(). */
void fs_glue_init(void);

/**
 * Return the mounted media in *out, mounting (lx open + fx_media_open) on
 * first use.  Returns FX_SUCCESS or the failing fx/lx status; on a virgin or
 * non-LevelX device expect failure -- run `fs format`.
 */
UINT fs_media_acquire(FX_MEDIA **out);

/** Flush + unmount (fx_media_close, lx close).  No-op when not mounted. */
UINT fs_media_unmount(void);

/** Nonzero while the media is mounted (gate for destructive qspi commands). */
int fs_is_mounted(void);

/*
 * Media ownership, reader/writer style.  Every normal fs command holds a
 * shared op slot for its whole duration; `fs format` / `fs umount` take the
 * exclusive slot (fails while any op or another exclusive runs); the
 * destructive raw qspi commands take the raw slot (same as exclusive, but
 * additionally refused while the media is mounted).  begin calls return
 * FX_SUCCESS or FS_ERR_BUSY and never block; every successful begin must be
 * paired with the matching end on all exit paths.
 */
UINT fs_op_begin(void);
void fs_op_end(void);
UINT fs_exclusive_begin(void);
void fs_exclusive_end(void);
UINT fs_raw_begin(void);
void fs_raw_end(void);

/** Nonzero while format/umount/raw owns the media. */
int fs_is_busy(void);

/**
 * Serialize multi-call directory sequences (`fs ls` changes the media-global
 * default directory, iterates, then restores it -- FileX's media mutex only
 * covers each individual call). */
void fs_dir_lock(void);
void fs_dir_unlock(void);

/** The media singleton + its sector cache, for `fs format` orchestration. */
FX_MEDIA *fs_glue_media(void);
UCHAR    *fs_glue_cache(void);
ULONG     fs_glue_cache_size(void);

#ifdef __cplusplus
}
#endif

#endif /* FS_GLUE_H */
