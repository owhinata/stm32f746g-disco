<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 ThreadX Shell Project
-->

# ThreadX Shell (CLI) Requirements

Requirements for an interactive command-line shell (CLI) running on
STM32F746G-DISCO / Eclipse ThreadX. It is a **clean-room implementation
inspired by the design of the Zephyr RTOS shell subsystem** (`_ref/zephyr`,
v4.4.99); no Zephyr code is reused.

- Status: **Finalized** (codex requirements review LGTM v0.2 / implementation plan not yet started)
- Last updated: 2026-06-06
- Related: parent Issue (Epic) #1

> **Terminology**: the component and directory name is **Shell** (`shell/`); the
> C API prefix is **`cli_` / `CLI_`**. This document uses "Shell" for the feature
> ("shell instance", "Shell core") and `cli` as the identifier prefix.

---

## 1. Purpose and goals

Add a **practical interactive CLI** for development, debugging and operation to a
bare-metal + ThreadX firmware. Commands must be registerable declaratively from
each source file, and it is mandatory that **the shell can run on several
transports (backends) simultaneously**.

## 2. Scope (standard tier)

| Category | Features |
|---|---|
| Line editing | Cursor left/right / Home / End / word-wise movement, character insert/overwrite (insert mode), character delete, Backspace, kill-to-end, word delete, clear line, wrap redraw at the terminal width |
| Terminal control | VT100 escape parsing (arrows / Home / End / Delete / Insert), colour output (error=red / warn=yellow / info=green), Backspace mode (0x08 / 0x7F) toggle |
| Meta keys | Ctrl+a/b/c/d/e/f/k/l/n/p/u/w, Alt+b/f |
| Input aids | Command history (↑↓ / Ctrl+p,n, fixed ring buffer, consecutive-duplicate suppression), Tab completion (longest-common-prefix complete + candidate list), input-line cancel with Ctrl+c |
| Commands | Static subcommand tree, argc/argv validation (mandatory / optional / RAW), command return value |
| Output API | `cli_print` / `cli_error` / `cli_warn` / `cli_info`, `cli_hexdump`, output buffering + autoflush |
| Built-in commands | `help` `clear` `history` `backends` `version` `uptime` `reboot` `thread`(list/stacks/...) `devmem`(peek/poke/dump) |

## 3. Non-goals (excluded now; covered by future extensions)

Extensibility is preserved so these can be added later, but they are not built in
the first implementation:

- Wildcard expansion (`*` `?`), aliases, root switching via `select`, dynamic subcommands
- Log backend integration, network backends (telnet / mqtt / websocket / ssh), remote shell, getopt integration
- UTF-8 / multibyte input (ASCII only, → §13)
- Authentication / login (password prompt)

## 4. Architecture requirements

### 4.1 Core / backend separation
- The core requires only **`init` / `uninit` / `enable` / `write` / `read` / `update`** and **RX/TX events** from a backend, and is hardware-independent.
- A backend is free to choose its implementation (ring buffers, etc.).

### 4.2 Multi-instance (mandatory)
- **1 transport = 1 shell instance**. Each instance has its own **thread / event flags / mutex / line buffer / history / prompt / terminal state** and **several can run simultaneously** (e.g. VCP & Ethernet & Bluetooth).
- The command tree is **read-only shared data** in a linker section, referenced in common by all instances.
- See §10 for the detailed requirements on concurrency, output destination and contention.

### 4.3 Command registration (linker-section scheme)
- Command entries are collected into a dedicated section `.shell_root_cmds` and walked at startup.
- The registration macro `CLI_CMD_REGISTER(name, subcmd, help, handler, mandatory, optional)` (and friends) enables cross-file registration without `#ifdef`.
- Add `KEEP` + alignment + `__cli_root_cmds_start` / `__cli_root_cmds_end` symbols to the linker script.

### 4.4 Mapping to ThreadX primitives

| Abstraction | ThreadX |
|---|---|
| Per-instance thread | `tx_thread_create` |
| RX / TX / KILL signals | `tx_event_flags_create` |
| Lock / TX mutual exclusion (with timeout) | `tx_mutex_create` |
| History memory | Fixed ring array (no dynamic allocation; `tx_byte_pool` not used) |
| Time wait | ThreadX tick (1 ms) |

## 5. Naming conventions

- Functions / types: `cli_*`; macros / constants: `CLI_*`; public header: `shell/include/cli.h`
- Main types: `struct cli_instance`, `struct cli_transport`, `struct cli_transport_api`
- Examples: `cli_print(sh, "...")`, `CLI_CMD_REGISTER(...)`

## 6. Directory layout

```
shell/                              # first-class component (MIT, RTOS/app-agnostic)
├── include/cli.h                   # public API (command definition / print / instance-definition macros)
├── core/                           # core implementation
│   ├── cli_core.c                  # thread / state machine / dispatch
│   ├── cli_edit.c                  # line editing / VT100 / meta keys
│   ├── cli_history.c               # history (fixed ring)
│   ├── cli_complete.c              # Tab completion
│   ├── cli_parse.c                 # tokenizing / command-tree search
│   ├── cli_printf.c                # output buffering
│   ├── cli_vt100.h                 # VT100 escape codes
│   └── cli_internal.h
├── backend/
│   ├── cli_backend_uart_stm32.c    # USART1 VCP (IRQ-driven) — initial implementation
│   ├── cli_backend_dummy.c         # for tests / loopback
│   └── (future) cli_backend_net.c / cli_backend_bt.c
├── cmds/
│   ├── cmd_system.c                # version, uptime, reboot
│   ├── cmd_thread.c                # thread list/stacks/...
│   └── cmd_devmem.c                # devmem peek/poke/dump
└── port/
    └── cli_port_threadx.c          # thin wrapper over ThreadX primitives (host-test friendly; optional)

src/app_shell.c                     # VCP + dummy instance definitions + tx_application_define
ldscript/STM32F746NGHx_FLASH.ld     # add the .shell_root_cmds section
```

## 7. Changes to existing code

1. **Linker** (`ldscript/STM32F746NGHx_FLASH.ld`): add `.shell_root_cmds` (`KEEP` + `ALIGN` + `__cli_root_cmds_start/end`) after `.rodata`.
2. **UART**: make USART1 **IRQ-driven with RX/TX ring buffers** (HAL-based). Add `USART1_IRQHandler`, enable `USART1_IRQn` in the `NVIC`.
   - Currently only polled transmit (`_write` = blocking `HAL_UART_Transmit`, no RX).
3. **printf coexistence**: design how other threads' `printf` (blocking) and shell output contend (candidates: route `_write` into a chosen instance / leave them separate, → §10).
4. **CMake**: make `shell` an OBJECT/STATIC library. Add a new `shell` app + a `flash-shell` target. Keep the existing demos (threadx / coremark / thread_metric / exec_profile).

## 8. Configuration parameters / resource limits (compile-time, overridable)

| Parameter | Default | On overflow |
|---|---|---|
| Command buffer length | 256 B | Reject further input chars, ring the bell (BEL) and ignore |
| Max argument count (argc) | 20 | Report an error and do not run the command |
| History ring | 512 B | Evict the oldest entry (FIFO) |
| printf buffer | 32 B | Flush when full (§11) |
| Prompt buffer | 20 B | Truncate |
| Per-instance stack | 2048 B | — |
| Max shell instances | 4 | Fixed at compile time; exceeding is a build-time error |
| Max subcommand nesting depth | 8 | Report an error and do not run the command |
| Max Tab-completion candidates shown | Terminal-width dependent (no extra memory; candidate scan walks the command tree linearly and prints on the fly) | — |

- The number of registered commands depends on the linker-section capacity (effectively unlimited; the walk is linear).
- Everything is statically allocated. No heap allocation at run time.

## 9. Error handling / abnormal-case policy

- **Basic principle (fail-safe)**: under any anomaly the shell instance does not stop; it shows the error and returns to the prompt. One instance's anomaly never propagates to another.
- **Unknown command**: print `<cmd>: command not found` and return non-zero.
- **Bad / mismatched arguments**: show usage (help) or an error and do not call the handler. Return non-zero.
- **Command return value**: keep the most recent command's return value (a `retval`-like feature is a future extension; the standard tier uses non-zero for an error display).
- **Input line too long**: bell + ignore subsequent chars, as in §8.
- **RX overflow** (receive ring full): drop new received bytes and count the drops in internal stats (optionally warn). The core / other threads never block.
- **TX overflow / flow control**: §11.
- **backend init failure**: disable only that instance; other instances and the system continue. Record the failure in the boot log.
- **Cancelling command execution**: the input line can be cancelled with Ctrl+c (forcibly aborting a running handler is out of the standard scope).

## 10. Concurrency / thread safety / multi-instance contention

- **State isolation**: line buffer, history, terminal state and prompt are per-instance. The core holds no mutable global state (the command tree is read-only shared).
- **Output destination**: each instance's output goes **only to its own transport**; outputs never mix across instances.
- **Handler reentrancy**: multiple instances **may run the same command at the same time**. Command handlers must be written assuming reentrancy. When a handler touches shared hardware/state, **the handler is responsible for the exclusion** (the core does not guarantee it). The built-in standard commands are implemented to be reentrant-safe.
- **System-wide commands** (`reboot`, etc.): executed immediately. Consistency under simultaneous requests is not required (the first one to run resets the system).
- **Coexistence with other threads' printf**: by default other threads' `printf` (blocking `_write`) and shell output are **separate paths**; on a VCP instance sharing the same UART, output may interleave in time (documented as a known constraint). Optionally `_write` can be routed into a specific instance's thread-safe output (future extension).
- **Lock granularity**: per-instance TX/state is protected by a mutex (with timeout). ISR-to-core notification uses event flags only (no lock taken in the ISR).

## 11. Flow control / backpressure

- **Default behaviour**: output runs in that instance's thread context; when the backend TX buffer is full, a **blocking send that waits for TX completion** throttles it (no loss/drop).
- **Timeout**: a compile-time timeout can be set on the TX wait (default decided in the implementation plan; `0` = wait forever). **The behaviour on timeout is defined unambiguously**: the shell instance does not stop and continues, the remaining output is dropped and the drop counter is incremented, the output API (`cli_print`, etc.) returns failure (non-zero / negative), and **the running command exits non-zero**.
- **Bulk-output commands** (`thread stacks`, `devmem dump`, etc.) use the same scheme. No fixed maximum output size; blocking throttles it naturally.
- **Input intake**: even while output is blocking, RX never loses bytes thanks to IRQ + the receive ring (only on buffer overflow does §9 RX overflow apply).

## 12. Safety / dangerous-command policy

- `reboot` and `devmem` (especially `poke`) are **development/debug** features and require:
  - **Compile-time gate**: a macro (e.g. `CLI_ENABLE_DANGEROUS_CMDS`) can disable them all at once. **Default ON for the shell demo app**; production embedded builds **should default to OFF**.
  - **devmem address range**: by default unrestricted (development assumption). Allowed/forbidden address ranges can be configured at compile time, and out-of-range access is rejected with an error. Alignment/width (8/16/32) violations are errors.
  - **reboot**: executed immediately (a confirmation prompt is out of the standard scope). The impact is noted in help.
- Authentication / privilege separation are non-goals (§3). Dangerous commands are managed via the compile-time gate above.

## 13. Input character set / encoding

- Only **ASCII (0x00–0x7F)** input is accepted. Printable characters are 0x20–0x7E.
- Non-ASCII bytes (0x80–0xFF) are **dropped** (filtered) by default and not passed to the command buffer.
- Unsupported / malformed escape sequences are **ignored** and the state returns to normal (not treated as an error).
- UTF-8 / multibyte is a non-goal (§3).

## 14. Startup / shutdown / initialization order

1. `bsp_init()`: clock (216 MHz) / caches / GPIO & UART pin init.
2. `tx_kernel_enter()` → `tx_application_define()`.
3. For each instance, `cli_init()`: walk the command section (`.shell_root_cmds`), init the instance state, backend `init`.
4. backend `enable` → start thread → show prompt.
5. **On failure**: skip that instance and continue with the others (§9).
- `uninit` / KILL (stopping/destroying an instance) is provided as API, but **only init/start are mandatory in the standard scope**; the full stop/uninit lifecycle is verified in a future extension.

## 15. Performance / responsiveness requirements (216 MHz / 115200 8N1; verifiable targets)

- **Echo latency**: from a single-character input to its echo, **< 5 ms** (representative target; IRQ → thread wake → echo).
- **Operation response**: prompt response for Tab completion / history recall / normal commands, **< 50 ms** (representative target).
- **Zero input loss**: zero loss for normal operation input and for **continuous input within the receive-ring capacity** (guaranteed by the receive ring). Same during bulk output and with multiple instances running. **Under overload beyond the ring capacity**, the zero-loss guarantee does not apply and §9 RX overflow (drop + drop stats) applies.
- The representative figures above are finalized in the implementation plan; acceptance is judged in §18.

## 16. License / clean-room policy

- **No Zephyr (Apache-2.0) source code is reused**. Only public documentation, design concepts and understanding of public-API behaviour may be referenced; copying code fragments, comments or distinctive wording is prohibited.
- All new shell files are **MIT**, `SPDX-License-Identifier: MIT`, `Copyright (c) 2026 ThreadX Shell Project`.
- **Provenance (mandatory)**: `NOTICE` or the README must **state explicitly** that "the design is a clean-room implementation inspired by the Zephyr RTOS shell". Reviews include "no code/wording reuse from Zephyr" as a check.

## 17. Documentation

- During implementation, add same-named `.md` to `docs/ja` / `docs/en` (Japanese + English mandatory) in the mkdocs RTOS category.
- Contents: architecture diagram, how to define commands, how to add a backend, pin / UART assumptions, how to gate dangerous commands.

## 18. Acceptance criteria

Completion is judged by the following pass/fail:

1. **Basic interaction**: connect a terminal to the VCP and see the prompt. `help` lists the registered commands.
2. **Line editing**: cursor movement / insert / delete / Backspace / word delete / clear line are reflected correctly on the terminal.
3. **History**: ↑↓ / Ctrl+p,n recall past commands. Consecutive duplicates are not recorded.
4. **Completion**: Tab auto-completes a single candidate; multiple candidates yield longest-common-prefix completion + a list.
5. **Colour / meta keys**: errors display in red, etc. Ctrl+a/e/u/w, etc. work.
6. **Built-in commands**: `version` `uptime` `reboot` `thread list/stacks` `devmem` (peek/poke/dump) behave as expected.
7. **Abnormal cases**: on unknown command / bad arguments / line too long, the shell does not crash, returns to the prompt and shows an appropriate error (§9).
8. **Multi-instance**: **a VCP and a dummy instance run simultaneously**, each with independent input/output and no mixed output.
9. **Zero loss**: even during bulk output (e.g. repeated `thread stacks`), normal operation input within the receive-ring capacity is not lost (overload allowed as §9 RX overflow, §15).
10. **Dangerous-command gate**: a build with `CLI_ENABLE_DANGEROUS_CMDS` OFF disables `reboot` / `devmem`.
11. **Automated tests**: through the dummy backend, input → output verification passes for at least the basic and abnormal cases.
12. **Build**: the `flash-shell` target builds and does not break the existing demos (threadx, etc.).

## 19. Verification method

- Connect to `/dev/ttyACM0` with a 115200 8N1 terminal (e.g. picocom) and confirm §18.
- Verify the core (parser / editing / completion / history / abnormal cases) with automated tests via the dummy backend.
- Per the CLAUDE.md rules, run `codex-review` (design / MCU function vs RM0385 / HW resource contention) before finalizing the implementation plan and after implementation, and commit after on-hardware verification.

## 20. Work breakdown (candidate subtasks of the parent Issue)

1. Linker-section foundation (`.shell_root_cmds` + start/end symbols)
2. Core: instance / thread / state machine / dispatch (ThreadX primitives)
3. Parser: tokenizing / command-tree search / argc/argv validation
4. Command-registration macros (`CLI_CMD_REGISTER` / subcommands / SUBCMD_SET)
5. Line editing / VT100 / meta keys / colour
6. History (fixed ring)
7. Tab completion
8. Output API (`cli_print/error/warn/info`, hexdump, buffering, flow control §11)
9. Transport abstraction + UART(VCP) backend (USART1 IRQ-driven, IRQ handler, NVIC)
10. Dummy backend + multi-instance proof + automated-test foundation
11. Built-in commands: version / uptime / reboot (dangerous-command gate §12)
12. Built-in command: thread (list / stacks / ...)
13. Built-in command: devmem (peek / poke / dump, address-range gate §12)
14. App integration: `src/app_shell.c` + CMake `shell` library + `flash-shell`
15. Documentation `docs/ja` / `docs/en` + NOTICE/README (clean-room provenance §16)
16. codex-review (pre-plan / post-implementation) + on-hardware verification
