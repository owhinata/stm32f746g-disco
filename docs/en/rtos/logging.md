# Logging and crash dump (`dmesg` / fault)

Issue #28. Adds a RAM ring-buffer log subsystem plus a crash dump on faults. The
log **survives resets**, so after a reboot `dmesg` replays the last messages,
crash information included. Clean-room design (only the *concept* is borrowed
from NuttX ramlog / Zephyr logging; no code is reused).

Components:

- `svc/log.h` / `svc/log.c` — the levelled ring buffer in DTCM and the log API.
- `shell/cmds/cmd_dmesg.c` — the `dmesg` command that replays the log.
- `src/fault.c` — HardFault/MemManage/BusFault/UsageFault handlers and the dump.
- `shell/cmds/cmd_crash.c` — a `crash` command that forces a fault on purpose
  (a dangerous command).
- Formatting reuses the clean-room formatter (`svc/fmt.c`) extracted
  from the [output API](shell-output.md); `cli_print`, `dmesg` and the fault dump
  all share it.

## RAM log ring (DTCM `.log_noinit`)

The log lives in the `.log_noinit (NOLOAD)` section of the linker script
(`ldscript/STM32F746NGHx_FLASH.ld`), placed first in RAM so it lands in the
**DTCM (`0x20000000`)**. That placement is what makes retention work:

- **Bypasses the cache**: the Cortex-M7 TCM is not behind the L1 D-cache, so a
  record written from fault context is committed to SRAM with no cache clean
  (and SWD always sees the latest bytes).
- **Survives resets**: the CMSIS `Reset_Handler` initialises only `.data`/`.bss`,
  never `.noinit`, so the contents carry over a **system reset**
  (reboot = SYSRESETREQ / fault / IWDG / WWDG / NRST / LPWR, RM0385 §5.1.1). A
  **power reset** (POR/PDR/BOR, §5.1.2) is not guaranteed — `log_init()`
  validates the magic and re-initialises when it is invalid.
- `.log_noinit` is inserted before `.data`/`.bss`, which simply shift up
  (`_estack` is unchanged). A linker `ASSERT` keeps it inside the 64 KB DTCM.
  Default size 8 KB.

### Record format

A 32-byte header (magic / version / size / head / tail / seq / boot_count)
followed by variable-length records. `head`/`tail` are **free-running 32-bit byte
offsets** (indexed with `& (size-1)`, size a power of two), so empty
(`head==tail`) and full (`head-tail==size`) are distinct.

Each record (4-byte aligned): `{u16 total_len | u8 level | u8 magic} {u32 ts_ms}
{u32 seq} {char tag[8]} {text NUL-terminated + pad}`. The **tag is an inline
8-byte field** (no flash string pointer), so old records stay readable after a
reflash. A record never straddles the physical end: when it would, a **SKIP
record** (a 4-byte head only, `level=0xFF`) fills the tail fragment and the
record wraps to offset 0. When space is short, whole oldest records are evicted
from the tail.

### Consistency

- Writes run under a **PRIMASK critical section** (no `tx_*` call), so thread,
  ISR and fault context all share it safely. Formatting happens on the stack
  outside the section; the section only evicts, copies, and advances `head`.
- The body is written, then `__DMB()`, then `head` is committed. If a reset hits
  before the head update, the boot walk reads only up to the old head, so a
  **half-written record is never visible**.
- Boot validation walks tail→head and truncates head at the first malformed
  record (resync), so a reset mid-write costs only the trailing record.

## Log API

Define `LOG_TAG` **before** `#include "log.h"`, then call the level macros:

```c
#define LOG_TAG "uart"
#include "log.h"

LOG_INF("baud=%u", baud);
LOG_ERR("init failed: %d", rc);
```

- Levels are `LOG_ERR` / `LOG_WRN` / `LOG_INF` / `LOG_DBG` (lower is more severe).
- A level above the **compile-time threshold** `LOG_COMPILE_LEVEL` (default DBG)
  is removed per call site.
- A level above the **run-time threshold** (default INF) is not recorded; change
  it with `dmesg -n <lvl>`.
- Timestamps come from `HAL_GetTick()` (monotonic ms). **Normal logs are not
  echoed to the console** (so the interactive shell stays clean); read them with
  `dmesg`.
- `bsp_init()` calls `log_init()` **before `fault_init()`**: it validates the
  ring, sets `log_ready`, then records the boot marker
  (`boot: #<N> reset cause: ...`) in that order. `log_write()` is a no-op until
  `log_init()` returns.

## `dmesg` command

```text
sh> dmesg
[     0.000] INF boot: #1 reset cause: POR
[    12.345] INF boot: #2 reset cause: SFT
[    60.001] ERR fault: bus cfsr=00000400 hfsr=00000000 mmfar=00000000 bfar=20080000
[    60.001] ERR fault: pc=080012a4 lr=08001233 psr=61000000 sp=2000c7d8 exc=fffffffd
sh> dmesg -c            # print, then clear
sh> dmesg -n dbg        # set run-time threshold to DBG (records LOG_DBG too)
dmesg: level = DBG
```

- Output format: `[<sec>.<ms>] <LVL> <tag>: <text>`, oldest → newest.
- `log_iter_next()` copies one record at a time in a short PRIMASK section. The
  pass snapshots `head` at the start, so background logging does not make it chase
  a live tail; if eviction overtakes the cursor it resyncs to tail.
- `cli_cancel_requested()` is polled between records, so **Ctrl+C stops** a long
  replay (the same cooperative cancel as the [output API](shell-output.md)
  hexdump).

## Crash dump (fault handlers)

`src/fault.c` provides **strong** HardFault/MemManage/BusFault/UsageFault
handlers that override the CMSIS startup weak aliases (which only spin in
`Default_Handler`). `fault_init()` (in `bsp_init()`) enables MemManage/Bus/Usage
individually via `SCB->SHCSR` and traps divide-by-zero as a UsageFault via
`CCR.DIV_0_TRP`.

On a fault:

1. A naked stub selects MSP/PSP from `EXC_RETURN` bit2 and passes the exception
   frame, `EXC_RETURN` and the callee-saved registers (R4-R11) to the C handler.
   The type is read from `SCB->ICSR` VECTACTIVE.
2. **RAM log first** (survives even if the UART then wedges): two lines with
   `CFSR/HFSR/MMFAR/BFAR` and `pc/lr/psr/sp/exc`.
3. **Polled USART1 dump** (no HAL/IRQ/ThreadX): type, fault status registers,
   R0-R12/SP/LR/PC/xPSR, a stack dump around SP, and a stack-scan backtrace.
4. **Halt** (busy loop, not WFI). The old ST-Link cannot attach to a sleeping
   core (#20/#24/#26); the halted state is still visible to SWD.

Dump details:

- **SP reconstruction**: `EXC_RETURN` bit4 selects the 8-word (basic) or 26-word
  (FPU-extended) frame, plus the stacked xPSR bit9 (STKALIGN padding)
  (PM0253 §2.4.7).
- **Stack dump**: clamped to the current thread's `tx_thread_stack_start/end`
  via `_tx_thread_current_ptr` for PSP faults, or `_estack` for MSP faults; an
  out-of-range SP falls back to a short dump.
- **Backtrace**: no frame pointer. The stack is scanned for Thumb-tagged words
  pointing into `.text` (`_stext`–`_etext`) whose preceding instruction is a
  `BL`/`BLX` (up to 16 entries).

```text
*** FAULT: BusFault ***
 CFSR=00000400 HFSR=00000000 MMFAR=00000000 BFAR=20080000(valid)
 R0=20080000 R1=00000000 ...
 ...
 stack @sp:
  2000c7d8: 080012a5 20000a40 ...
 backtrace:
  #0 080012a4 (pc)
  #1 08001232 (lr)
  #2 08001a90
 halted.
```

## `crash` test command (dangerous)

A dedicated command to validate the crash dump on hardware. `devmem` cannot do
this — its region allow-list rejects the Reserved address before the access. The
file compiles in only when `CLI_ENABLE_DANGEROUS_CMDS` is set (same gate as
`reboot`/`devmem`). **It halts the board and does not return.**

| Subcommand | Fault forced |
|---|---|
| `crash bus` | read `0x20080000` (Reserved) → precise BusFault (BFAR check) |
| `crash undef` | undefined instruction (`UDF #0`) → UsageFault (UNDEFINSTR) |
| `crash div0` | integer divide by zero → UsageFault (DIVBYZERO) |

## Notes

- Timestamps use `HAL_GetTick()` (ms); the 32-bit value wraps after about
  **49.7 days** (not a practical concern).
- The reset cause is read from `RCC->CSR` (RM0385 §5.3.21) and cleared with
  `RMVF` so the next boot reads its own cause. When several flags are set, one is
  reported with priority LPWR > WWDG > IWDG > SFT > POR > BOR > PIN.

## Verification

- **Build**: `cmake --build build`. Host tests (`shell/test/run_host_tests.sh`)
  stay green (extracting the formatter leaves `cli_print`/hexdump output
  unchanged).
- **On target** (`/dev/ttyACM0`, 115200 8N1, `flash`):
    - right after boot, `dmesg` shows a `boot: #N reset cause: ...` line.
    - after `reboot`, `dmesg` still shows the previous boot's log followed by a
      new `reset cause: SFT` boot line (**retention across reset**).
    - `dmesg -c` empties the log; after `dmesg -n dbg`, `LOG_DBG` is recorded.
    - `crash bus` dumps over UART immediately (`BusFault` / `BFAR=20080000` /
      stack / backtrace) then halts. After a board reset, `dmesg` still shows the
      two fault lines before the new boot line. `crash undef` / `crash div0` show
      a UsageFault the same way.
    - SWD (`openocd` + `gdb-multiarch`) can attach while halted.
