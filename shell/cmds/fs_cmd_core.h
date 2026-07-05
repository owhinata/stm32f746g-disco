/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    fs_cmd_core.h
 * @brief   Media-independent FileX command core (issue #34, Epic #32).
 *
 * The `fs` (QSPI) and `sd` (microSD) shell commands share one set of FileX
 * command bodies (ls/cat/write/rm/mkdir/info/umount).  Everything media-specific
 * -- the lazy-mount singleton, the reader/writer ownership gates, the wear or
 * card-info line, the mount-failure hint -- is reached through a `struct
 * fs_device` vtable so the bodies stay device-agnostic.  Each command file
 * (cmd_fs.c / cmd_sd.c) owns its `struct fs_device` instance and registers thin
 * wrappers that bind it (the CLI passes only the leaf subcommand name as argv[0],
 * so the device cannot be recovered from argv).
 *
 * Thread-context only; the bodies use the cli_* output API and the device's
 * ownership gates, so they are safe from background jobs.
 */
#ifndef FS_CMD_CORE_H
#define FS_CMD_CORE_H

#include "fx_api.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cli_instance;

/* Shared "media busy" sentinel returned by the ownership gates, outside the
 * FileX status space so fs_strerror() cannot confuse it with one.  Guarded so
 * it can coexist with the identical define in fs_glue.h. */
#ifndef FS_ERR_BUSY
#define FS_ERR_BUSY 0xF0u
#endif

/* Returned by a device's acquire() when the slot is empty (microSD removed).
 * Outside the FileX status space; mapped by fs_strerror(). */
#ifndef FS_ERR_NO_CARD
#define FS_ERR_NO_CARD 0xF1u
#endif

/**
 * Media abstraction for the shared command bodies.  One instance per command
 * file (cmd_fs.c => QSPI, cmd_sd.c => SD); the callbacks forward to that media's
 * glue (fs_glue.c / sd_fs_glue.c).
 */
struct fs_device {
	const char *name;        /* "fs" / "sd": message prefix                    */
	const char *mount_hint;  /* second-line hint printed on a mount failure    */
	UINT (*acquire)(FX_MEDIA **out);   /* lazy mount, return the media          */
	UINT (*unmount)(void);
	int  (*is_mounted)(void);
	UINT (*op_begin)(void);  void (*op_end)(void);     /* shared op slot         */
	UINT (*excl_begin)(void);void (*excl_end)(void);   /* exclusive (umount)     */
	void (*dir_lock)(void);  void (*dir_unlock)(void); /* listing serialization  */
	/* Trailing info line(s) for `info`/`df`: QSPI prints wear, SD prints
	 * nothing (NULL).  Called with the mounted media. */
	void (*info_extra)(struct cli_instance *sh, FX_MEDIA *media);
};

/** FileX/LevelX status -> human string (media-neutral; hints come from the
 *  device's mount_hint). */
const char *fs_strerror(UINT status);

/** Mount-on-demand: returns the media or NULL after printing the failure +
 *  the device's mount_hint.  Exposed for device-specific handlers (e.g. the
 *  QSPI/SD format remount). */
FX_MEDIA *fs_core_mount(const struct fs_device *dev, struct cli_instance *sh);

/* Shared command bodies; each binds a device.  Argument arity matches the
 * registered subcommands (argv[0] = leaf name). */
int fs_core_ls    (const struct fs_device *dev, struct cli_instance *sh, int argc, char **argv);
int fs_core_cat   (const struct fs_device *dev, struct cli_instance *sh, int argc, char **argv);
int fs_core_write (const struct fs_device *dev, struct cli_instance *sh, int argc, char **argv);
int fs_core_rm    (const struct fs_device *dev, struct cli_instance *sh, int argc, char **argv);
int fs_core_mkdir (const struct fs_device *dev, struct cli_instance *sh, int argc, char **argv);
int fs_core_info  (const struct fs_device *dev, struct cli_instance *sh, int argc, char **argv);
int fs_core_umount(const struct fs_device *dev, struct cli_instance *sh, int argc, char **argv);

/**
 * Read an entire file from @p dev into @p buf (capacity @p cap bytes) through the
 * device's shared op gate; fails if the file is larger than @p cap.  Returns 0
 * with *out_len set to the bytes read, or 1 (a message is printed).  Cross-command
 * reuse for `ai model load` (#89), same ownership model as `camera save`.
 */
int fs_core_read_file(const struct fs_device *dev, struct cli_instance *sh,
                      const char *path, void *buf, uint32_t cap, uint32_t *out_len);

/* Device accessors for cross-command reuse (issue #42: `camera save` writes
 * a frame through the same ownership gates as the fs/sd commands).  Defined
 * by the owning command files. */
const struct fs_device *fs_qspi_device(void);   /* cmd_fs.c */
const struct fs_device *fs_sd_device(void);     /* cmd_sd.c */

#ifdef __cplusplus
}
#endif

#endif /* FS_CMD_CORE_H */
