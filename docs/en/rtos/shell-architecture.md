# Shell architecture overview

The single firmware **`threadx`** is an interactive **ThreadX CLI shell** over the
USART1 VCP; commands and benchmarks (`coremark`) all run as shell commands
([Shell app](shell-app.md)). This page ties the whole picture together --
**defining a command / adding a backend / pin & UART assumptions / the dangerous
command gate** -- and links each topic to its detail page.

The shell (`shell/`) is a **clean-room implementation**: only the *design* and the
*public API behaviour* of the Zephyr RTOS shell were used as a reference, and none
of its code was reused (evidence in
[NOTICE](https://github.com/owhinata/stm32f746g-disco/blob/main/NOTICE)). All new
files are MIT (`SPDX-License-Identifier: MIT` / `Copyright (c) 2026 ThreadX Shell
Project`).

## Layers

Data flow from receive to output (real UART backend):

```
USART1 IRQ -> backend(cli_backend_uart) RX ring
   -> core(cli_core: ThreadX thread / event flags / mutex)
   -> line edit(cli_edit) / history(cli_history) / completion(cli_complete)
   -> parser(cli_parse) on Enter -> dispatch
   -> command handler(shell/cmds/*)
   -> output API(cli_printf, locked + flow-controlled)
   -> backend write() -> USART1 TX ring -> IRQ transmit
```

| Layer | Implementation | Role | Detail |
|---|---|---|---|
| Registration | `.shell_root_cmds` (linker section) + `CLI_CMD_REGISTER` | declarative command registration, walked at startup | [Registration](shell-registration.md) |
| Parser | `cli_parse.c` | tokenize + command-tree walk + argc/argv validation (a pure function, no `struct cli_instance`) | [Parser](shell-parser.md) |
| Core | `cli_core.c` (ThreadX) + `cli_session.c` (pure) | thread / event-flag / mutex lifecycle and the RX->dispatch state machine | [Core](shell-core.md) |
| Line edit | `cli_edit.c` | cursor / in-line edit / meta keys / VT100 / terminal-width wrap / colour | [Line editing](shell-editing.md) |
| History | `cli_history.c` | fixed byte ring (per-instance, FIFO eviction) | [History](shell-editing.md) |
| Completion | `cli_complete.c` | Tab completion (bash-style two-stage) | [Completion](shell-completion.md) |
| Output | `cli_printf.c` | minimal printf + 32B staging + flow control + colour/hexdump | [Output API](shell-output.md) |
| Backend | `cli_backend_uart.c` / `cli_backend_dummy.c` | transport abstraction (real UART / host-test loopback) | [UART backend](shell-backend-uart.md) / [Testing](shell-testing.md) |

Splitting the core into `cli_core` (ThreadX-dependent) and the pure
`cli_session` / `cli_parse` / `cli_printf` lets the pure parts be unit-tested under
host gcc ([test harness](shell-testing.md)). All storage is static -- no runtime
heap.

## Instance and thread model

- **One transport = one `cli_instance`** (statically allocated). The command tree
  is read-only data shared by every instance and the core keeps no mutable
  globals, so **multiple instances run concurrently without interfering**
  (requirements §10). Multi-instance is exercised by the host tests (two dummies).
- Each instance is a ThreadX thread (default priority `CLI_INSTANCE_PRIORITY`=16).
  An ISR only sets an event flag to wake the thread. The ThreadX integration (a
  single SysTick driving both the HAL and ThreadX ticks; the interrupt-priority
  gotcha) is in [ThreadX integration](threadx.md).
- The current firmware runs a **single VCP instance** plus an LED heartbeat
  (priority 10).

!!! note "Two output paths (printf vs cli_print)"
    `cli_print`/`cli_write` go through the instance's transport `write()`
    (**per-instance**). The C-library `printf`, however, goes through the UART
    backend's strong `_write` to a **single global console `g_uart_console`**
    ([UART backend](shell-backend-uart.md)). `coremark` output uses printf.
    Making printf follow the invoking terminal in a multi-instance setup is a
    separate issue (#18).

## Defining a command

Commands are **registered declaratively** from any source file (detail in
[Registration](shell-registration.md)):

```c
#include "cli.h"

static int cmd_version(struct cli_instance *sh, int argc, char **argv) { /* ... */ return 0; }

/* Subcommand tree: thread { list, stacks } */
CLI_SUBCMD_SET_CREATE(sub_thread,
    CLI_CMD_ARG(list,   NULL, "list threads",     cmd_list,   1, 0),
    CLI_CMD_ARG(stacks, NULL, "show stack usage", cmd_stacks, 1, 0),
    CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(version, NULL,       "show version",      cmd_version, 1, 0);  /* root */
CLI_CMD_REGISTER(thread,  sub_thread, "thread operations", NULL,        2, 0);  /* parent */
```

- Handler type `int (*)(struct cli_instance *sh, int argc, char **argv)`; `argv[0]`
  is the command name, 0 means success. argc/argv validation (`mandatory` /
  `optional`, `CLI_ARG_RAW` to capture the raw line tail) runs in the parser before
  the handler.
- A handler that touches only the `sh` passed to it (via the output API) is
  **reentrant** (instances can run the same command at once).
- Command sources need only `#include "cli.h"`, which is ThreadX-independent. The
  `shell/cmds/cmd_*.c` files link into the firmware exe only (host tests are a
  separate build).

## Adding a backend

A new transport implements `struct cli_transport_api` (detail in
[UART backend](shell-backend-uart.md); the dummy is in [Testing](shell-testing.md)):

1. Implement the **API table**: `init` / `enable` / `write` / `read` mandatory,
   `uninit` / `update` optional.
   - `write` contract = **non-blocking**: return the number of bytes accepted
     (0..len). When full (`n<len`), the backend must fire
     `cli_transport_notify_tx()` once space frees (the core waits on `CLI_EVT_TX`
     and realises "block until sent" itself, with a timeout).
   - `read` = non-blocking drain returning 0..cap bytes.
2. Wake the thread from the ISR/callbacks with `cli_transport_notify_rx()` /
   `cli_transport_notify_tx()` (**only set an event flag** -- never take a lock or
   touch line state).
3. Statically define the transport with `CLI_BACKEND_xxx_DEFINE(name, ...)` and
   bind it with `CLI_INSTANCE_DEFINE(inst, &name, "prompt> ")`.
4. In `tx_application_define()`, `cli_init()` then (on success) `cli_start()`; a
   failure disables just that instance (§9 fail-safe).

The byte ring (`cli_uart_ring.h`) is HAL/ThreadX-free and host-unit-tested.

## Pin / UART assumptions

| Item | Value | Note |
|---|---|---|
| VCP | USART1 TX=PA9 / RX=PB7, 115200 8N1 | ST-Link virtual COM (`/dev/ttyACM0`) |
| PA9 shared | VCP_TX / OTG_FS_VBUS | VCP works with the **factory solder bridges** (UM1907); a board rewired for USB-OTG host loses VCP_TX |
| LED | LD1 (green) = PI1 | heartbeat thread |

The `_write` (printf retarget) translates a bare `\n` in printf output to `\r\n`,
so the `coremark` report etc. render cleanly even without a terminal-side LF→CRLF
map (`picocom --imap lfcrlf`). See [Hardware / board](../hardware/board.md) and
[Shell app](shell-app.md).

## Dangerous command gate

- **`CLI_ENABLE_DANGEROUS_CMDS`** (CMake option, default ON) gates `reboot` /
  `devmem` at compile time. With it OFF (production: `-DCLI_ENABLE_DANGEROUS_CMDS=OFF`)
  the handlers and their registration vanish, leaving `.shell_root_cmds` and
  disappearing from help and completion (requirements §12 / §18.10).
- **`devmem` address-range gate**: rather than a single [min,max], a compile-time
  **region allow-list `devmem_map[]`**. An access must lie wholly within one
  region with the right direction (r/w) and width (8/16/32); anything else (e.g. a
  Reserved hole) is rejected without being attempted (avoiding an unmapped-region
  fault that hangs in the weak `Default_Handler`). Detail in [devmem](shell-devmem.md)
  / [Built-in commands](shell-builtins.md).

## Build and layout

- Single firmware `threadx`. The shell core/backends are the OBJECT library
  `shell_obj` and CoreMark is `coremark_obj` (`-O3`), both linked into the
  `threadx` exe. Build with `cmake --build build`, flash with `--target flash`.
  See [Build (CMake)](../build/cmake.md) / [Shell app](shell-app.md) /
  [CoreMark command](shell-coremark.md).

## Clean-room / license

The design used only the *concepts* and *public API behaviour* of the Zephyr RTOS
shell (Apache-2.0); no code, comments, or specific wording was reused
(requirements §16). New shell files are MIT. The clean-room statement and the
third-party attributions (ST HAL/CMSIS, ThreadX, CoreMark) are recorded in
[NOTICE](https://github.com/owhinata/stm32f746g-disco/blob/main/NOTICE).
