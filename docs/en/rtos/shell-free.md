# Memory usage command (`free`)

Issue #58. A **runtime, dynamic counterpart** to the build-time `size` output: the
read-only diagnostic command `free` reports, for each of the four physical memory
regions (Flash / DTCM / SRAM / SDRAM), the total / used / free bytes, plus the
newlib heap occupancy. It lives in `shell/cmds/cmd_free.c` and, like `thread`
([Thread info](shell-thread.md)), is linked into the `shell` executable only (it
does not affect the host test harness command set). It changes no state and
touches only the `sh` instance passed to it, so it stays reentrant across
instances (§10).

## Command

| Command | Registration | Behaviour |
|---|---|---|
| `free` | `CLI_CMD_REGISTER(free, NULL, ..., cmd_free, 1, 0)` | Show total/used/free for the 4 regions + heap occupancy |

No subcommands (a single leaf). `free <arg>` is rejected as too many arguments
(mandatory=1 / optional=0).

```text
sh> free
region start          total      used      free use%
Flash  0x08000000   1048576    295796    752780  28%  .isr/.text/.rodata/.data
DTCM   0x20000000     65536      8224     57312  12%  .log_noinit (reset-persistent)
SRAM   0x20010000    262144     82656    179488  31%  .data/.bss/.sram1_dma + heap
SDRAM  0xC0000000   8388608   4657024   3731584  55%  .sdram fixed/cam/eth/ai/model (banks0-3)

heap:  base 0x200242E0  arena 0  in-use 0  free-pool 0
stack: top  0x20050000  main-reserve 1024 B (MSP/ISR grow down into SRAM free)
```

| Column | Meaning |
|---|---|
| region | Region name (Flash / DTCM / SRAM / SDRAM) |
| start | Region base address (linker MEMORY ORIGIN) |
| total | Region size (LENGTH, bytes) |
| used | Static usage (computed below, heap included) |
| free | `total − used` |
| use% | `used / total` percentage |

## Per-region accounting (linker symbols)

The `used` value of each region is derived from symbols provided by
`ldscript/STM32F746NGHx_FLASH.ld`. The symbol's *address* carries the value
(`(uintptr_t)&sym`).

| Region | total | used | Rationale |
|---|---|---|---|
| **Flash** 1MB @0x08000000 | LENGTH | `LOADADDR(.data) + sizeof(.data) − ORIGIN(FLASH)` | `.data`'s load image is the last thing in FLASH, so this is the whole footprint (== `size`'s text+data) |
| **DTCM** 64KB @0x20000000 | LENGTH | `_elog_noinit − _slog_noinit` | `.log_noinit` (reset-persistent log ring) is the only resident |
| **SRAM** 256KB @0x20010000 | LENGTH | `(heap break) − ORIGIN(RAM)` | static (`_end − ORIGIN` = `.data`+`.bss`+`.sram1_dma`) + heap arena |
| **SDRAM** 8MB @0xC0000000 | LENGTH | **sum** of the per-bank NOLOAD sub-regions (see note) | `.sdram.fixed` (bank0 LTDC/display) + `.sdram.cam` (bank1 camera) + `.sdram.eth` (bank2 ETH DMA, #49) + `.sdram.ai` (bank3 lower NN arena, #81/#88) + `.sdram.ai.model` (bank3 upper reloc slots, #92) |

Region ORIGIN/LENGTH are kept in `cmd_free.c` as **compile-time constants**
mirroring the linker MEMORY block (single source of truth: the `.ld`), which keeps
`free` a **zero-linker-change, read-only** command. The same addresses are already
hardcoded in bsp.c (MPU) and the SDRAM/QSPI drivers; they never change without a
linker edit.

!!! note "SDRAM is a per-bank sum, not `_esdram − _ssdram`"
    `.sdram` splits into the `fixed`/`cam`/`eth`/`ai`/`ai.model` sub-regions at FMC
    internal bank boundaries (2 MB/1 MB aligned), leaving **alignment holes** between
    each bank's end and the next bank's start (#65/#81/#92). `_esdram − _ssdram` would
    be the hole-inclusive span (7 MB / 87.5% here) and over-report the true footprint,
    so `free` sums each sub-region (`_e… − _s…`) **individually** and never counts the
    holes. A sub-region a given build does not use is empty (start == end) and adds 0
    (e.g. `.sdram.ai.model` is 0 unless the reloc NN backend is compiled in).

### SRAM layout (heap and stack share the tail)

The SRAM region (0x20010000..0x20050000) stacks `.data` → `.bss` → `.sram1_dma`
from the bottom, with `_end` (= heap base) on top. The **heap grows up from
`_end`**, the **main/ISR stack grows down from `_estack` (0x20050000)**, and the
two share the same free span. Hence:

- `used` = `(heap break) − ORIGIN(RAM)` (static + heap arena)
- `free` = `_estack − (heap break)` (the span shared by heap growth and stack descent)

The trailing `stack:` line shows `_estack` (initial MSP) and `_Min_Stack_Size`
(the link-time guaranteed main-stack reservation).

## Heap occupancy (newlib `mallinfo`)

The heap line comes from newlib's `mallinfo()`:

- **base** = `_end` (lowest heap address)
- **arena** = total bytes obtained from the system (sbrk) = current break − heap base
- **in-use** = `uordblks` (currently used by the program)
- **free-pool** = `fordblks` (reusable bytes left on the free list)

!!! note "Why `mallinfo()` rather than `sbrk(0)`"
    The toolchain's stock `_sbrk()` (libnosys) compares the requested break against
    the *current* stack pointer, which in a ThreadX thread context is the thread's
    PSP (in `.bss`, below the heap) -- so `sbrk(0)` can return a wrong value / fail
    from thread context. `mallinfo()` reads malloc's own accounting and avoids
    `_sbrk` entirely. When malloc was never called, arena=0 and the heap line is
    all zeros.

!!! note "Best-effort"
    The free-list walk is not locked against a concurrent malloc/free on another
    thread. Like `thread`'s stats it is best-effort diagnostics, and this firmware
    barely uses the heap (ThreadX thread stacks live in `.bss`; dynamic allocation
    is handled by byte pools). Per-thread stack high-water is owned by
    [`thread`](shell-thread.md); `free` is limited to the region overview (no
    duplication).

## Verification

- **Build**: `cmake --build build` (`shell` builds; existing demo intact). The
  linker's `Used Size` (DTCM 8224B / FLASH 295796B etc.) and the `free` output
  should agree.
- **On target** (`/dev/ttyACM0` @115200 8N1):
  - `free` appears in `help`; Tab completion `fr` → `free`.
  - `free` shows total/used/free for the 4 regions + the heap line, consistent
    with the build-time `size`.
  - `free x` is a usage/arg error.
