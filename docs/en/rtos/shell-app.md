# Shell app (`shell` / `flash-shell`)

Packages every shell layer built so far ([registration](shell-registration.md) /
[parser](shell-parser.md) / [core](shell-core.md) / [output](shell-output.md) /
[dummy backend](shell-testing.md) / [UART(VCP) backend](shell-backend-uart.md))
into a **CMake library and a `shell` application that runs on real hardware**. It
brings up an interactive shell over the ST-Link Virtual COM Port (VCP) and runs
**two instances concurrently (VCP + dummy)** to demonstrate the multi-instance
architecture on silicon.

What comes up here is the current **minimal line input** (echo / Backspace / CR-LF
dispatch / Ctrl+C / escape swallow). Line editing / VT100 / colour are #9, history
#10, completion #11, `version`/`uptime`/`reboot` #12, `thread` #13, `devmem` #14.

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
CLI_BACKEND_DUMMY_DEFINE(dum_tr);           CLI_INSTANCE_DEFINE(dum_sh, &dum_tr, "dum> ");
static struct cli_instance *const shells[] = { &vcp_sh, &dum_sh };
_Static_assert(SHELL_COUNT <= CLI_MAX_INSTANCES, ...);   /* the §4.2 compile-time gate */
```

| thread | priority | role |
|---|---|---|
| `vcp_sh` shell | `CLI_INSTANCE_PRIORITY`=16 | interactive shell on the USART1 VCP |
| `dum_sh` shell | 16 | shell on the dummy backend (no I/O pins) |
| `led` | 10 | blinks LD1 (PI1) every 250 ms (shows coexistence) |
| `dummy_drv` | 17 | drives the dummy instance and mirrors its transcript to the VCP |

`tx_application_define()` loops `shells[]` calling `cli_init()` then, only on
success, `cli_start()`. Both `cli_init` (backend / ThreadX object create) and
`cli_start` (`tx_thread_create`) can fail, so **on either failure that instance is
disabled and the rest continue** (§9 fail-safe). An `enable()` failure is handled
inside the started thread (uninit → exit), so it needs nothing here.

### Why the dummy driver is race-free

`dummy_drv` runs at a **lower scheduling priority** than the dummy shell thread
(numeric 17 vs 16; in ThreadX a larger number is lower priority). Under ThreadX
strict-priority preemption it executes only while that shell thread is not ready —
i.e. blocked on its RX event, hence not inside `dummy_write()`. The
`cli_transport_notify_rx()` at the end of `cli_dummy_inject()` makes the
higher-priority dummy thread ready and preempts the driver right there, so the line
is fully processed and the thread re-blocks before `inject()` returns. Thus the
driver's `cli_dummy_clear_output()` (before inject) and capture snapshot (after)
never overlap `dummy_write()`. The dummy backend is lock-free by design (see the
[testing doc](shell-testing.md)); this priority invariant is what makes sharing it
safe. The settle sleep is belt-and-suspenders. Any imperfection can only garble the
cosmetic `[dummy]` line — never the VCP instance, whose state is wholly separate
(§10).

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
Ctrl+C cancels the input line. Every few seconds a `[dummy] …` block mirrors the
second instance's transcript.

!!! note "PA9 = VCP_TX / OTG_FS_VBUS shared (UM1907)"
    PA9 is shared between VCP_TX and OTG_FS_VBUS. With the **factory solder
    bridges** the VCP works. On a board whose bridges were changed for USB-OTG host
    use, VCP_TX is unavailable.

!!! warning "Visual interleaving is a known limitation (§10)"
    The `[dummy]` mirror (printf) and the `sh>` input line go to the **same
    USART1**, so they can interleave on screen — the minimal line editor does not
    redraw the input line. **State is never corrupted** (per-instance). Input-line
    redraw is #9.

## Verification

- **Build**: `cmake --build build` builds `threadx` and `shell`; the optional
  demos (`-DBUILD_COREMARK=ON`, etc.) still configure/build (existing demos kept
  building — DoD).
- **Host unit tests**: `sh shell/test/run_host_tests.sh` passes (a separate build,
  unaffected by this app).
- **On target** (`/dev/ttyACM0` @115200 8N1, §18): prompt / echo / Backspace /
  `help` / `echo` / unknown command / Ctrl+C / `[dummy]` two-instance coexistence /
  LD1 blink. Echo latency < 5 ms (§15 preliminary); no dropped characters within
  the RX ring for long lines / pasted input.
