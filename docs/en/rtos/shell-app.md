# Shell app (`threadx` / `flash`)

Packages every shell layer built so far ([registration](shell-registration.md) /
[parser](shell-parser.md) / [core](shell-core.md) / [output](shell-output.md) /
[dummy backend](shell-testing.md) / [UART(VCP) backend](shell-backend-uart.md))
into a **CMake library and the single `threadx` firmware that runs on real
hardware**. It brings up an interactive shell over the ST-Link Virtual COM Port (VCP). The
multi-instance architecture (§4.2/§10) is exercised by the host tests (two dummy
instances, no crosstalk) and was demonstrated on silicon in #8, so this demo keeps
a **single VCP instance** to give the line editor a clean, uninterrupted session
(the dummy backend stays a first-class, host-tested backend in the library).

The input here has the full [#9 interactive line editor](shell-editing.md) (cursor
motion / in-line insert+delete / meta keys / VT100 escapes / terminal-width wrap /
colour). History is #10, completion #11,
[`version`/`uptime`/`reboot`](shell-builtins.md) #12, `thread` #13, `devmem` #14,
[`coremark`](shell-coremark.md) #17.

## CMake layout

`shell/` is integrated in two layers to form the single `threadx` firmware:

| target | kind | contents |
|---|---|---|
| `shell_obj` | OBJECT library | shell core (`shell/core/*.c`) + backends (`cli_backend_uart.c` / `cli_backend_dummy.c`); carries `bsp_iface` (HAL/CMSIS/`bsp.h`) + ThreadX includes |
| `coremark_obj` | OBJECT library | CoreMark (`lib/coremark` + `port/coremark`) built at `-O3`, `MEM_METHOD=MEM_STATIC`, with `core_main.c` compiled `-Dmain=coremark_main` ([CoreMark command](shell-coremark.md)) |
| `threadx` | executable | `src/main.c` + `shell/cmds/cmd_*.c` (builtin/system/thread/devmem/coremark) + `tx_glue.c` + ThreadX core/asm, linking `common` / `shell_obj` / `coremark_obj`; adds `-u _printf_float` and grows the `flash` target |

- The objlib is named **`shell_obj`, not `threadx`**: CMake target names are global
  and the executable takes `threadx`.
- **`TX_INCLUDE_USER_DEFINE_FILE` is set on both** the objlib and the exe. Because
  `cli_instance.h` pulls in `tx_api.h`, the objlib must see the **same
  `port/threadx/tx_user.h`** as the ThreadX core linked into the exe, or the
  `TX_THREAD` / event-flags / mutex layouts disagree (ABI mismatch).
- **`src/stm32f7xx_it.c` is not in the build** (ThreadX supplies PendSV; SVC has
  no user); `USART1_IRQHandler` comes from the UART backend.
- `cmd_builtin.c` (`help`/`echo`) is compiled **into the exe only**. The host
  tests are a separate build, so their registered command set is unaffected.

## Instances and threads (`src/main.c`)

```c
CLI_BACKEND_UART_DEFINE(vcp_tr, &huart1);   CLI_INSTANCE_DEFINE(vcp_sh, &vcp_tr, "sh> ");
static struct cli_instance *const shells[] = { &vcp_sh };
_Static_assert(SHELL_COUNT <= CLI_MAX_INSTANCES, ...);   /* the §4.2 compile-time gate */
```

| thread | priority | role |
|---|---|---|
| `vcp_sh` shell | `CLI_INSTANCE_PRIORITY`=16 | interactive shell on the USART1 VCP |
| `led` | 10 | blinks LD1 (PI1) every 250 ms (shows coexistence) |

`tx_application_define()` loops `shells[]` calling `cli_init()` then, only on
success, `cli_start()`. Both `cli_init` (backend / ThreadX object create) and
`cli_start` (`tx_thread_create`) can fail, so **on either failure that instance is
disabled and the rest continue** (§9 fail-safe). An `enable()` failure is handled
inside the started thread (uninit → exit), so it needs nothing here. Add a
transport to `shells[]` to run more instances at once (bounded by the §4.2
compile-time gate).

## Built-in commands (`shell/cmds/cmd_builtin.c`)

| command | registration | behaviour |
|---|---|---|
| `help` | `CLI_CMD_REGISTER(help, NULL, ...,1,CLI_MAX_SUBCMD_DEPTH+1)` | shows command help **hierarchically** (`help` / `help <cmd>` / `help <cmd> <sub>`) via `CLI_ROOT_CMD_FOREACH` + sentinel walk of subcommand sets (the on-target proof that the `.shell_root_cmds` scan resolved, §18.1). See the section below |
| `echo` | `CLI_CMD_REGISTER(echo, NULL, ...,1,CLI_ARG_RAW)` | prints the rest of the line verbatim (RAW argument) |

Both handlers touch only the `sh` passed to them through the output API, so they
are reentrant when several instances run the same command at once (§10).

## Hierarchical help and argument usage (#37)

`help` walks the registered command tree using **public API only**
(`CLI_ROOT_CMD_FOREACH` + a sentinel walk of each subcommand set) and prints in
three forms. It **reuses the one-line `.help`** already on every `struct cli_cmd`;
no new descriptor field is added.

| form | shows |
|---|---|
| `help` | every root command; those with subcommands get a trailing `>` (command-group marker) |
| `help <cmd>` | a group prints its own help + the list of its subcommands; a leaf prints its one-line help as usage |
| `help <cmd> <sub> …` | descends the tree along the path and shows the resolved node as above |

An unknown command/subcommand prints a red `help: no such command '<path>'` and
exits non-zero.

```text
sh> help
Commands:
  echo       echo the rest of the line
  fs         QSPI flash filesystem (FileX + LevelX) >
  help       list commands
  ...
'>' marks a command group; type 'help <command> [subcommand]' for details.

sh> help fs
fs -- QSPI flash filesystem (FileX + LevelX)
Subcommands:
  ls         list directory [path]
  cat        print file <path>
  write      write <path> <text>
  ...
Type 'help fs <subcommand>' for details.

sh> help fs ls
fs ls  list directory [path]
```

**Registration**: `help` uses `optional = CLI_MAX_SUBCMD_DEPTH + 1` (**not**
`CLI_ARG_RAW`, which would collapse the tail into one argument and stop `help fs ls`
from tokenizing). This admits the deepest legal path `help + root +
CLI_MAX_SUBCMD_DEPTH subcommands`.

### Automatic usage on an argument error

On a wrong argument count (`CLI_PARSE_WRONG_ARGS`), `cli_dispatch_segment`
(`cli_session.c`) follows the existing `<cmd>: invalid number of arguments` with a
**usage line**. It builds the full command path from the resolved command that
`cli_parse()` populated before returning `WRONG_ARGS` (`pr.cmd` / `pr.cmd_level`)
plus `sh->argv`, and appends the `.help`:

```text
sh> fs write
write: invalid number of arguments
usage: fs write  (write <path> <text>)
```

The text in `(...)` is the command's one-line `.help` **reused as usage** (note the
`.help` strings mix argument-syntax style — `write <path> <text>` — with description
style — `capacity / free / state`). The usage line is assembled into one local
buffer and emitted with a **single `cli_print()`** so a background-job line cannot
splice into the middle of it (§10/§11).

## Build / flash / connect

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
cmake --build build                          # the single target, threadx
cmake --build build --target flash   # write over ST-Link
picocom -b 115200 /dev/ttyACM0               # connect to the VCP (8N1)
```

On connect the `sh> ` prompt appears. `help` lists the commands, `echo hello world`
prints `hello world`, an unknown command prints a red `…: command not found`, and
Ctrl+C cancels the input line. The full [#9 line editor](shell-editing.md) — cursor
motion, in-line editing, meta keys, wrap redraw — works on the line.

!!! note "PA9 = VCP_TX / OTG_FS_VBUS shared (UM1907)"
    PA9 is shared between VCP_TX and OTG_FS_VBUS. With the **factory solder
    bridges** the VCP works. On a board whose bridges were changed for USB-OTG host
    use, VCP_TX is unavailable.

!!! warning "Interleaving with other-thread printf (§10)"
    The boot banner and any other thread's `printf` also go to the **same USART1**.
    A printf landing in the middle of the line being edited can garble it on screen,
    but **state is never corrupted** (per-instance), and `Ctrl+l`
    [redraws](shell-editing.md) the input line to recover.

## Verification

- **Build**: `cmake --build build` builds the single target `threadx`.
- **Host unit tests**: `sh shell/test/run_host_tests.sh` passes (a separate build,
  unaffected by this app).
- **On target** (`/dev/ttyACM0` @115200 8N1, §18): prompt / echo /
  [line editing](shell-editing.md) (cursor motion, in-line insert+delete, meta keys,
  wrap) / `help` / `echo` / unknown command / Ctrl+C / LD1 blink. Echo latency
  < 5 ms, operation response < 50 ms (§15); no dropped characters within the RX ring
  for long lines / pasted input.
