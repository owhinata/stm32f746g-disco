# System built-in commands (`version` / `uptime` / `reboot`)

The first step of M4 "built-in commands" (#12). On top of `help`/`echo` (the
[shell app](shell-app.md)) it adds **`version` / `uptime` / `reboot`** and, with
them, the **compile-time gate for dangerous commands `CLI_ENABLE_DANGEROUS_CMDS`**
(requirements §12). They live in `shell/cmds/cmd_system.c` and, like `cmd_builtin.c`,
are linked into the **executable only** (the host test harness keeps its own set).

## Command list

| command | registration | behaviour |
|---|---|---|
| `version` | `CLI_CMD_REGISTER(version, NULL, ...,1,0)` | FW name/version, git describe, build date, ThreadX version, MCU silicon id (device/rev/flash/UID) |
| `uptime` | `CLI_CMD_REGISTER(uptime, NULL, ...,1,0)` | time since boot as `Dd HH:MM:SS (N ms)` |
| `reboot` | `CLI_CMD_REGISTER(reboot, NULL, ...,1,0)` (`#if CLI_ENABLE_DANGEROUS_CMDS`) | immediate software reset. A **dangerous command**, wrapped by the gate and absent in OFF builds |

All three handlers touch only the passed `sh` through the output API, so they stay
reentrant across concurrent instances (§10).

## `version`

```text
sh> version
ThreadX Shell v0.1.0 (d264989-dirty)
Built:    Jun  7 2026 12:00:00
ThreadX:  6.5.0
MCU:      STM32F746 (devid 0x449 rev 0x1001)
Flash:    1024 KB
UID:      0x00370027 0x32355119 0x...
```

Hardware identity is read straight from CMSIS register macros (checked against RM0385):

| field | source | RM0385 |
|---|---|---|
| device id / revision | `DBGMCU->IDCODE` (`DBGMCU_IDCODE_DEV_ID_Msk` / `REV_ID_Pos`) | §40.6.1 (`0xE0042000`) |
| 96-bit unique UID | `UID_BASE` (`0x1FF0F420`, 3×u32) | §41.1 |
| flash size (KB) | `FLASHSIZE_BASE` (`0x1FF0F442`, u16) | §41.2 |

- **git describe** is captured at CMake configure time (`git describe --always --dirty
  --tags`) and substituted into `cmake/cli_version.h.in` → `build/gen/cli_version.h`
  (a **configure-time snapshot**; re-run cmake to refresh after committing).
- **Build date** comes from the compiler's `__DATE__` / `__TIME__` (refreshed when that
  TU recompiles).
- 32-bit values print via an `(unsigned long)` cast + `%lu` / `%08lx` to match
  `cli_print`'s long-width `%l*` path.

## `uptime`

Converts the millisecond counter of `HAL_GetTick()` (SysTick → `HAL_IncTick`, 1 kHz =
1 ms tick, `port/threadx/tx_glue.c`) into days/hours/minutes/seconds, with the raw ms
alongside.

```text
sh> uptime
up 0d 00:03:12 (192341 ms)
```

!!! note "Wraps after ~49.7 days"
    `HAL_GetTick()` is `uint32_t`, so it rolls over to 0 after ~49.7 days. Treat the
    reading on long-running boards as indicative.

## `reboot` (dangerous command)

Performs an **immediate** reset via `HAL_NVIC_SystemReset()` (SCB→AIRCR `SYSRESETREQ`,
RM0385 §5.1.1). No confirmation prompt — requirements §12 keeps that out of scope.

```text
sh> reboot
rebooting...
(the board resets and the boot banner reappears)
```

- `cli_print()` **enqueues** into the IRQ-driven UART TX ring and returns before the
  bytes leave the wire. Resetting immediately would truncate the last line, so
  `tx_thread_sleep(50)` (~50 ms) lets the ring drain first. 50 ms covers the
  **worst-case full 512 B ring** (~44 ms at 115200 8N1) as best-effort headroom.
- `__disable_irq()` is **not** used before the reset: the TX-complete IRQ is what drains
  the ring, so interrupts stay enabled during the sleep.

## Dangerous-command gate `CLI_ENABLE_DANGEROUS_CMDS` (§12)

`reboot` (and later #14's `devmem poke`) can be compiled out wholesale:

- The default lives in `shell/include/cli_config.h` as an `#ifndef` set to **ON (=1)**
  (same style as the other knobs; a fall-back for targets that do not define it, e.g.
  host tests).
- A **CMake cache variable does not reach the compiler on its own**, so the `shell`
  target forwards the CMake option to the define explicitly:

```cmake
option(CLI_ENABLE_DANGEROUS_CMDS "Build the dangerous shell commands (reboot, devmem)" ON)
target_compile_definitions(shell PRIVATE
    CLI_ENABLE_DANGEROUS_CMDS=$<BOOL:${CLI_ENABLE_DANGEROUS_CMDS}>)
```

When OFF, `reboot`'s handler and its `CLI_CMD_REGISTER` are excluded by `#if`, so the
descriptor never enters `.shell_root_cmds` — it disappears from `help` and Tab completion.

```bash
# production-style build (dangerous commands disabled)
cmake -B build-safe -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
      -DCLI_ENABLE_DANGEROUS_CMDS=OFF
cmake --build build-safe --target shell
arm-none-eabi-nm build-safe/shell.elf | grep -i reboot   # prints nothing (reboot gone)
```

## Verification

- **Build**: with the default ON, `cmake --build build` (`threadx` + `shell`) succeeds
  (existing demos unaffected).
- **Gate OFF**: the `build-safe` above drops the `reboot` symbols while `version`/`uptime`
  remain (§18.10).
- **On target** (`/dev/ttyACM0` @115200 8N1):
  `help` lists the three commands / `version` prints the rich output / `uptime` grows over
  time / `reboot` restarts the board (banner reappears) / Tab completion resolves
  `ver`→`version` and `re`→`reboot`.
