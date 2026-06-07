# Shell app (`shell` / `flash-shell`)

Packages every shell layer built so far ([registration](shell-registration.md) /
[parser](shell-parser.md) / [core](shell-core.md) / [output](shell-output.md) /
[dummy backend](shell-testing.md) / [UART(VCP) backend](shell-backend-uart.md))
into a **CMake library and a `shell` application that runs on real hardware**. It
brings up an interactive shell over the ST-Link Virtual COM Port (VCP). The
multi-instance architecture (§4.2/§10) is exercised by the host tests (two dummy
instances, no crosstalk) and was demonstrated on silicon in #8, so this demo keeps
a **single VCP instance** to give the line editor a clean, uninterrupted session
(the dummy backend stays a first-class, host-tested backend in the library).

The input here has the full [#9 interactive line editor](shell-editing.md) (cursor
motion / in-line insert+delete / meta keys / VT100 escapes / terminal-width wrap /
colour). History is #10, completion #11,
[`version`/`uptime`/`reboot`](shell-builtins.md) #12, `thread` #13, `devmem` #14.

## CMake layout

`shell/` is integrated in two layers (existing demos are kept building):

| target | kind | contents |
|---|---|---|
| `shell_obj` | OBJECT library | shell core (`shell/core/*.c`) + backends (`cli_backend_uart.c` / `cli_backend_dummy.c`); carries `bsp_iface` (HAL/CMSIS/`bsp.h`) + ThreadX includes |
| `shell` | executable | `src/app_shell.c` + `shell/cmds/cmd_builtin.c` + `tx_glue.c` + ThreadX core/asm, linking `common` and `shell_obj`; grows the `flash-shell` target |

- The objlib is named **`shell_obj`, not `shell`**: CMake target names are global
  and the executable takes `shell`.
- **`TX_INCLUDE_USER_DEFINE_FILE` is set on both** the objlib and the exe. Because
  `cli_instance.h` pulls in `tx_api.h`, the objlib must see the **same
  `port/threadx/tx_user.h`** as the ThreadX core linked into the exe, or the
  `TX_THREAD` / event-flags / mutex layouts disagree (ABI mismatch).
- Like the `threadx` app, **`src/stm32f7xx_it.c` is omitted** (ThreadX supplies
  PendSV); `USART1_IRQHandler` comes from the UART backend.
- `cmd_builtin.c` (`help`/`echo`) is compiled **into the exe only**. The host
  tests are a separate build, so their registered command set is unaffected.

## Instances and threads (`src/app_shell.c`)

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
| `help` | `CLI_CMD_REGISTER(help, NULL, ...,1,0)` | lists registered root commands via `CLI_ROOT_CMD_FOREACH` (the on-target proof that the `.shell_root_cmds` scan resolved, §18.1) |
| `echo` | `CLI_CMD_REGISTER(echo, NULL, ...,1,CLI_ARG_RAW)` | prints the rest of the line verbatim (RAW argument) |

Both handlers touch only the `sh` passed to them through the output API, so they
are reentrant when several instances run the same command at once (§10).

## Build / flash / connect

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
cmake --build build                          # threadx + shell
cmake --build build --target flash-shell     # write over ST-Link
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

- **Build**: `cmake --build build` builds `threadx` and `shell`; the optional
  demos (`-DBUILD_COREMARK=ON`, etc.) still configure/build (existing demos kept
  building — DoD).
- **Host unit tests**: `sh shell/test/run_host_tests.sh` passes (a separate build,
  unaffected by this app).
- **On target** (`/dev/ttyACM0` @115200 8N1, §18): prompt / echo /
  [line editing](shell-editing.md) (cursor motion, in-line insert+delete, meta keys,
  wrap) / `help` / `echo` / unknown command / Ctrl+C / LD1 blink. Echo latency
  < 5 ms, operation response < 50 ms (§15); no dropped characters within the RX ring
  for long lines / pasted input.
