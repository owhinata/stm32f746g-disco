# Memory Access Command (`devmem`)

Issue #14 of M4 "built-in commands". After `version`/`uptime`/`reboot`
([system built-ins](shell-builtins.md)) and `thread`
([thread info](shell-thread.md)), this adds `devmem` — a developer/debug command
to read and write memory directly. It lives in `shell/cmds/cmd_devmem.c` and, like
`cmd_system.c`, is linked into the **executable only** (never the host test harness).

`devmem` is a **dangerous** command (spec §12): the whole file compiles in only
when `CLI_ENABLE_DANGEROUS_CMDS` is set. With it off, the handlers and their
registration vanish, so `devmem` leaves `.shell_root_cmds` and disappears from
`help` and Tab completion — same gate as `reboot`.

## Subcommands

| Command | Args (mandatory / optional) | Behaviour |
|---|---|---|
| `devmem peek <addr> [8\|16\|32]` | 2 / 1 | Read one 8/16/32-bit word (default 32-bit) |
| `devmem poke <addr> <val> [8\|16\|32]` | 3 / 1 | Write one word, then read it back |
| `devmem dump <addr> [len]` | 2 / 1 | Canonical hex+ASCII over `[addr, addr+len)` (default 64 bytes) |

`devmem` is a pure parent (handler `NULL`); `devmem` alone reports *missing or
unknown subcommand*. Tab completion offers `peek`/`poke`/`dump` automatically
because completion walks the command tree (issue #11). The handlers touch only the
`sh` passed to them through the buffered output API, so they stay reentrant across
instances (req §10).

```text
sh> devmem peek 0x20000000
0x20000000: 0xdeadbeef
sh> devmem peek 0x08000000 16
0x08000000: 0x2000
sh> devmem poke 0x20000000 0x1234
0x20000000: 0x00001234
sh> devmem dump 0x08000000 32
08000000  00 04 02 20 c5 01 00 08  d1 01 00 08 d3 01 00 08  ... ............
08000010  d5 01 00 08 d7 01 00 08  d9 01 00 08 00 00 00 00  ................
```

`peek`/`poke` print `0x<addr>: 0x<value>`, with the value zero-padded to the access
width. `poke` always reads the location back after writing, so the printed value is
what the bus actually holds (see the read-clear caveat below). `dump` prints
**absolute** addresses in the offset column via `cli_hexdump_base()`
([output API](shell-output.md)).

## Numbers, width, alignment

- **Address / value / length** accept `0x`/`0X` hex or plain decimal. Parsing is
  strict: an invalid digit, trailing garbage, or 32-bit overflow is an error.
- **Width** is `8`, `16`, or `32` (bits); omitted, it defaults to 32. `poke` also
  rejects a value that does not fit the chosen width (e.g. `poke … 0x1ff 8`).
- **Alignment** follows the width: 16-bit needs a 2-byte-aligned address, 32-bit a
  4-byte-aligned address; 8-bit is unconstrained. A misaligned access is rejected.

## Address-range gate (region allow-list)

Rather than a single `[min,max]`, accesses are checked against a compile-time
**region allow-list** (`devmem_map[]` in `cmd_devmem.c`). An access must lie wholly
inside one region, be permitted for its direction (read vs write), and use a width
that region allows; otherwise it is **rejected with an error instead of attempted**.

This matters because the F746 memory map (RM0385 §2.2.2) is full of Reserved holes.
A read or write into a hole faults to the CMSIS weak `Default_Handler`, whose
infinite loop would **hang the shell** until reset. The gate keeps a typo off those
holes and turns it into a normal CLI error.

Default map:

| Region | Range | read | write | widths |
|---|---|---|---|---|
| ITCM-RAM | `0x00000000`–`0x00003FFF` (16 KB) | ✓ | ✓ | 8/16/32 |
| Flash (AXIM) | `0x08000000`–`0x080FFFFF` (1 MB) | ✓ | ✗ | 8/16/32 |
| DTCM | `0x20000000`–`0x2000FFFF` (64 KB) | ✓ | ✓ | 8/16/32 |
| SRAM1+2 | `0x20010000`–`0x2004FFFF` (256 KB) | ✓ | ✓ | 8/16/32 |
| APB1 | `0x40000000`–`0x40007FFF` | ✓ | ✓ | 32 only |
| APB2 | `0x40010000`–`0x40016BFF` | ✓ | ✓ | 32 only |
| AHB1 | `0x40020000`–`0x4007FFFF` | ✓ | ✓ | 32 only |
| AHB2 | `0x50000000`–`0x50060BFF` | ✓ | ✓ | 32 only |
| PPB | `0xE0000000`–`0xE00FFFFF` (SCB/NVIC/SysTick) | ✓ | ✓ | 32 only |

- All real RAM/Flash plus the on-chip peripheral bus windows and the PPB are
  allowed by default; only Reserved holes and **Flash writes** are blocked. A plain
  store to Flash is not a Flash programming sequence, so Flash is read-only here.
- Flash is the **AXIM alias** (`0x08000000`) only; the ITCM Flash alias
  (`0x00200000`) is not in the map.
- Peripheral buses and the PPB are **word-access only** — many registers fault or
  misbehave on sub-word access. Because `dump` is byte-granular, it is allowed on
  RAM/Flash only; use `peek <addr> 32` to read a peripheral register.
- External memory (FMC SDRAM at `0xC0000000`, QSPI) is **not** in the map: the
  shell does not initialise the FMC/QSPI, so those accesses would fault.

To customise the allowed regions, edit `devmem_map[]` (a compile-time constant).
For production, the cleaner lever is the dangerous-command gate, which removes
`devmem` entirely.

!!! warning "The gate is bus-window granular, not register granular"
    The peripheral/PPB regions cover whole bus windows, which still contain
    unimplemented/Reserved register offsets. An access there (e.g. an unused offset
    inside the APB1 window) passes the gate but can still fault and hang the shell.
    The gate stops the common typo into a *Reserved hole between* windows; it does
    not validate every offset inside one.

!!! warning "Peripheral / PPB writes have system-wide effects"
    Reads of `read-clear` registers disturb state just by reading (so `poke`'s
    read-back may differ from what you wrote, and `peek` is not free of effect).
    A `poke` into NVIC/SCB/RCC/GPIO/SysTick can change interrupts, clocks, or pin
    state and destabilise the whole system. These are allowed by default for debug
    convenience — use them deliberately.

## `dump` length cap

`cli_hexdump` holds the per-instance output lock for the whole run, so an unbounded
`dump` would block other instances' output for a long time (req §10). A request
longer than `CLI_DEVMEM_DUMP_MAX_LEN` (default **256** bytes; override via the CMake
cache variable of the same name) is rejected. A zero length is a no-op.

!!! note "D-cache and stale data"
    The D-cache is enabled (`src/bsp.c`). `dump`/`peek` read through the cache with
    no cache maintenance, so when observing memory updated by DMA or another bus
    master the value may be stale.

## Building with the gate off

```bash
cmake -B build-safe -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
      -DCLI_ENABLE_DANGEROUS_CMDS=OFF
cmake --build build-safe --target shell
arm-none-eabi-nm build-safe/shell.elf | grep -i devmem   # prints nothing
```

## Verification

- **Build**: default ON `cmake --build build` (threadx + shell) passes; host tests
  (`shell/test/run_host_tests.sh`) green, including the `cli_hexdump_base` cases.
- **Gate OFF**: `devmem` (and `reboot`) symbols absent from the `build-safe` ELF
  (spec §18.10).
- **On hardware** (`/dev/ttyACM0`, 115200 8N1, `flash-shell`):
    - `devmem peek 0x20000000` reads DTCM; `devmem peek 0x08000000 16` reads Flash.
    - `devmem dump 0x08000000 64` shows the vector table with absolute addresses.
    - `devmem poke 0x20000000 0xdeadbeef` writes and reads back; confirm with `peek`.
    - Rejections (no hang): `devmem peek 0x20000001 32` (alignment),
      `devmem peek 0x20000000 64` (bad width), `devmem poke 0x20000000 0x1ff 8`
      (value too wide), `devmem peek 0x10000000` (Reserved hole),
      `devmem poke 0x08000000 1` (Flash write), `devmem dump 0x40020000`
      (dump on a 32-bit-only region), `devmem dump 0x20000000 99999` (over cap).
    - `devmem peek 0x40020000 32` reads GPIOA (peripheral, 32-bit).
    - Tab completion: `devmem ` + Tab → `peek poke dump`.
