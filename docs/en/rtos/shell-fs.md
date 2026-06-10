# Shell / fs command

Operates the FileX filesystem on the QSPI NOR ([Filesystem](filesystem.md)). `shell/cmds/cmd_fs.c` (#30).

```
fs format [full] yes   format (full = erase all 256 blocks first)
fs ls [path]           list a directory (default /)
fs cat <path>          print a file
fs write <path> <text> create/overwrite a file (quote text with spaces)
fs rm <path>           delete a file or empty directory
fs mkdir <path>        create a directory
fs info                capacity / free / FAT type
fs umount              flush and unmount
```

## Example session

```
sh> fs format full yes        # first time (required if the factory demo data is present)
erase 256/256 (100%)
formatting: 32130 sectors x 512 B (16065 KiB)...
formatted and mounted
sh> fs mkdir /config
created /config
sh> fs write /config/app.ini "hello world"
wrote 11 bytes to /config/app.ini
sh> fs cat /config/app.ini
hello world
sh> fs ls /config
-       11  APP.INI
sh> fs info
state    : mounted
fat      : FAT16 (32104 clusters)
cluster  : 512 B
total    : 16052 KiB
free     : 16051 KiB
sh> reboot
...
sh> fs cat /config/app.ini    # still readable after reboot (persistence)
hello world
```

## Behaviour notes

- **Lazy mount**: the first `fs` command mounts automatically; an unformatted device fails with `run fs format yes`
- **Writes persist immediately**: write/rm/mkdir/format end with `fx_media_flush`, so a reset/power cycle at any later point keeps the content (integrity *during* an interrupted write is out of scope)
- **`fs format` safety latch**: the literal `yes` token is mandatory
- **When `full` is needed**: on a LevelX-formatted device, `fs format yes` takes seconds (erase skipped, wear history preserved). On factory demo data it falls back to the full erase automatically (~3 min; Ctrl+C aborts between blocks and leaves the device unformatted)
- **Ctrl+C**: honored between blocks (`format`), chunks (`cat`) and entries (`ls`)
- **Background jobs**: the only state is a mutex-guarded singleton, so `fs write ... &` is safe
- **Ownership model**: normal fs commands hold a shared slot; `fs format` / `fs umount` / `qspi erase/test` take exclusive ownership. While an fs command is running, format/umount fail with `busy` (and vice versa), so the media can never be yanked away mid-command
- **Interaction with `qspi erase/test`**: refused while mounted; run `fs umount` first
- File names are FAT 8.3 + long names; `fs ls` prints the name stored in the FAT
