# Filesystem (FileX + LevelX)

A persistent filesystem on the external 16 MB QSPI NOR ([Hardware / QSPI NOR Flash](../hardware/qspi-flash.md)). Phase B (#30) of Epic #27.

```
shell `fs` command (shell/cmds/cmd_fs.c)
  → FileX (FAT16)        fx_media_* / fx_file_* / fx_directory_*
     → FileX media driver (port/filex/fx_lx_nor_driver.c)
        → LevelX NOR      lx_nor_flash_* (logical sectors ⇔ physical blocks, wear leveling)
           → LevelX driver glue (port/levelx/lx_nor_qspi_driver.c)
              → QSPI driver (port/qspi/qspi_flash.c, #29)
                 → N25Q128A 16 MB
```

The `lib/filex` / `lib/levelx` submodules (eclipse-threadx, read-only mirrors) are integrated with the same pattern as ThreadX: source GLOB + `port/filex/fx_user.h` / `port/levelx/lx_user.h` (`FX_INCLUDE_USER_DEFINE_FILE` / `LX_INCLUDE_USER_DEFINE_FILE`). NAND support (`lx_nand_*`) and the RAM simulator drivers are excluded from the build.

## Intended use

The QSPI NOR + LevelX + FileX stack covers "**small, frequently updated, persistent**" data (configuration files, calibration values, small logs). Bulk logs and large image sets are out of scope (future microSD + FileX).

## LevelX (wear leveling)

| Item | Value |
|------|-------|
| Physical block | **64 KB erase sector × 256** (erase 0xD8) |
| Logical sector | 512 B |
| Logical sectors per block | 126 (header + mapping cost ~2 sectors, ~1.6 % overhead) |
| Total sectors handed to the FS | `lx_nor_flash_total_physical_sectors` − one block's worth (reclaim slack) ≈ **32,130 ≈ 15.7 MB** |

- **`LX_DIRECT_READ` disabled** is what keeps the D-cache out of the picture: LevelX reads everything through a RAM sector buffer; no CPU access to 0x90000000 (the memory-mapped window) ever happens
- `LX_THREAD_SAFE_ENABLE` (internal LevelX mutex), `LX_NOR_DISABLE_EXTENDED_CACHE`
- LevelX metadata updates **re-program words inside already-programmed pages, clearing bits only**. The N25Q128A explicitly allows this (datasheet: "bits are programmed from one through zero", no documented per-page cumulative program limit)
- Erase counts live in each block header (word 0), so wear leveling continues across reboots

## FileX

- **FAT16**: 512 B/sector × 1 sector/cluster × ~32 K clusters
- `fx_media_format` parameters: 1 FAT, 256 root entries
- **Fault tolerant is disabled** (MVP): the acceptance bar is persistence after a clean flush; power-loss-during-write integrity is a follow-up issue
- `FX_NO_LOCAL_PATH`: absolute paths only
- Media protection is FileX's built-in ThreadX mutex (`FX_SINGLE_THREAD` off) → safe for concurrent shell fg/bg access

## Mount strategy (lazy mount)

The media singleton lives in `port/filex/fs_glue.c`. Only `fx_system_initialize()` and the mount mutex are set up in `tx_application_define()`; **the media mounts on the first `fs` command**:

1. `fs_media_acquire()` runs `fx_media_open` (driver INIT → `lx_nor_flash_open`) once, under the mutex
2. On a virgin device or factory demo data the mount fails visibly → "run `fs format`"
3. Every mutating command finishes with `fx_media_flush`, so no explicit unmount is needed for reboot persistence

The media cache is a static 4 KB (8 × 512 B sectors); the LevelX sector/verify buffers are static too (about 15 KB of RAM in total).

## Constraints / notes

- `fs format full` erases the 256 blocks in sequence (typ ~3 min / max ~13 min), with progress output and Ctrl+C honored between blocks. **A cancelled format leaves the device unformatted** (rerun it)
- `fs format` without `full` skips the erase when the LevelX structures are intact (seconds; also preserves erase-count history)
- While mounted, `qspi erase` / `qspi test` are refused; `fs umount` is the supported escape hatch
- Power-loss-during-write integrity is **out of MVP scope**

Command usage: [Shell / fs command](shell-fs.md).
