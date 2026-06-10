# QSPI NOR Flash

The on-board external flash, a **Micron N25Q128A13EF840F** (128 Mbit = **16 MB**; newer lots substitute the MT25QL128ABA1EW9, firmware-compatible), is driven by the STM32F746 **QUADSPI** peripheral. The driver lives in `port/qspi/qspi_flash.{c,h}`; the debug shell command is `qspi` (`shell/cmds/cmd_qspi.c`).

This is Phase A (#29) of the QSPI NOR + LevelX + FileX filesystem foundation (Epic #27).

## Wiring (UM1907 Table 13)

| Signal | Pin | AF |
|--------|-----|----|
| QUADSPI_CLK | PB2 | AF9 |
| QUADSPI_BK1_NCS | PB6 | AF10 (pull-up) |
| QUADSPI_BK1_IO0 | PD11 | AF9 |
| QUADSPI_BK1_IO1 | PD12 | AF9 |
| QUADSPI_BK1_IO2 | PE2 | AF9 |
| QUADSPI_BK1_IO3 | PD13 | AF9 |

!!! note "PB2 doubles as BOOT1"
    PB2 is shared with the BOOT1 strap, but the strap is sampled only while in reset; driving it as CLK at run time is harmless (board-default assignment, UM1907).

## Controller configuration

| Item | Value | Rationale |
|------|-------|-----------|
| Clock | AHB3 = HCLK 216 MHz, prescaler 3 → **SCLK 54 MHz** | 2x margin against FAST_READ 0x0B's 108 MHz rating; every 1-line command is legal |
| Mode | **indirect only (polled FIFO transfers)** | no memory-mapped access (0x90000000), so **no D-cache coherency concern** |
| FSIZE | 23 (2^24 = 16 MB) | RM0385 §14.5.2 |
| Sampling | half-cycle shift | same as the ST BSP |
| CS high time | 6 cycles (≥50 ns) | device requirement |

## Command set (all 1-1-1)

| Operation | opcode | Notes |
|-----------|--------|-------|
| JEDEC ID | 0x9F | expect `20 BA 18` |
| Read | 0x0B FAST_READ | 8 dummy cycles (power-on default; no config-register write needed) |
| Write enable | 0x06 | before every program/erase; WEL latch verified by read-back |
| Page program | 0x02 | 256 B unit; the driver splits at page boundaries automatically |
| Erase | 0x20 (4 KB) / 0xD8 (64 KB) / 0xC7 (chip) | typ 0.25 s / 0.7 s / minutes |
| Status | 0x05 (WIP poll), 0x70 Flag Status (P_ERR/E_ERR check, cleared via 0x50) | failures are invisible in WIP alone, so the FSR is checked after every program/erase |

Quad read (0x6B) is planned as Epic #27 Phase C.

## Locking and thread context

- Each public API call serializes the **whole flash operation** (WREN → command → WIP wait → FSR check) under an internal ThreadX mutex (TX_INHERIT), so concurrent callers (shell fg/bg, the future LevelX layer) are safe
- **Thread-context only** — never call from an ISR. Erase operations poll WIP with `tx_thread_sleep(1)` between reads so other threads keep running
- Initialized once from `tx_application_define()` (`src/main.c`); init issues no flash transaction (GPIO/RCC/QUADSPI setup and mutex creation only)

## The `qspi` shell command

```
qspi id                read the JEDEC ID (expect 20 BA 18)
qspi info              geometry / clock / status
qspi read <addr> [len] hexdump flash content (max 256 B)
qspi erase <addr>      erase one 4 KB subsector (must be 4 KB aligned; dangerous)
qspi test <addr>       destructive self-test (dangerous)
```

`qspi erase` / `qspi test` compile in only with `CLI_ENABLE_DANGEROUS_CMDS` (default ON), like devmem. `qspi test` runs **erase → blank check → program a counting pattern page by page → read back and verify** on one 4 KB subsector and reports PASS/FAIL; Ctrl+C is honored between pages.

!!! warning "Factory demo data"
    As shipped, the QSPI flash holds resources of the ST demonstration firmware (STemWin edition). `qspi erase` / `qspi test` destroy them. See `_ref/backup_full.bin` for the backup (docs/hardware/board.md).

## Verification

```
sh> qspi id
JEDEC ID: 20 BA 18  (Micron N25Q128A/MT25QL128, 16 MiB)
sh> qspi read 0 64        # factory demo data or FF
sh> qspi test 0xFF0000    # scratch area near the end
...
PASS: erase/program/verify 4 KB at 0x00ff0000
```

Failure triage:

- ID reads `00 00 00` / `FF FF FF` → check pins / AF (only PB6 is AF10) / RCC enables
- ID correct but reads are garbage → dummy-cycle count or prescaler
- Program fails (FSR P_ERR) → missing WREN or a page-boundary crossing bug
