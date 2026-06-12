# microSD (SDMMC1 + DMA)

The on-board microSD slot (CN3) is driven through the STM32F746 **SDMMC1**
peripheral. The driver lives in `port/sd/sd_card.{c,h}`; the debug shell command
is `sd` (`shell/cmds/cmd_sd.c`).

This is **Phase A** (#33) of the microSD filesystem work (Epic #32). Phase A is
**read-only** (non-destructive on a PC-formatted card); the FileX FAT32
filesystem and writes arrive in Phase B/C.

Where the QSPI NOR driver runs in indirect mode and needs **neither DMA nor
D-cache maintenance**, the SD path uses DMA transfers, so this driver tackles
**ISR-based DMA completion** and **D-cache coherency** head-on.

## Wiring (UM1907 CN3)

| Signal | Pin | AF |
|--------|-----|----|
| SDMMC1_D0 | PC8 | AF12 |
| SDMMC1_D1 | PC9 | AF12 |
| SDMMC1_D2 | PC10 | AF12 |
| SDMMC1_D3 | PC11 | AF12 |
| SDMMC1_CK | PC12 | AF12 |
| SDMMC1_CMD | PD2 | AF12 |
| Card detect | PC13 | input pull-up, **active-low** (low = inserted) |

Data/CMD lines are very-high speed, pull-up. None of these conflict with the
existing peripherals (USART1=PA9/PB7, LED=PI1, TIM2, QSPI=PB2/PB6/PD11-13/PE2).

## Clock

| Item | Value | Basis |
|------|-------|-------|
| Clock source | **CLK48 = 48 MHz** (PLLQ, set in `bsp.c`) | RCC DCKCFGR2 SDMMC1SEL (default); `sd_card_init` selects it explicitly via `__HAL_RCC_SDMMC1_CONFIG(RCC_SDMMC1CLKSOURCE_CLK48)` |
| Transfer clock | `SDMMC_CK = 48 MHz / (ClockDiv + 2) = 24 MHz` (ClockDiv=0) | RM0385 §35.8.2 |
| Identification | < 400 kHz (HAL's internal `SDMMC_INIT_CLK_DIV`) | card spec |
| Bus width | **4-bit** (falls back to 1-bit) | `HAL_SD_ConfigWideBusOperation` |

!!! note "Applying the transfer clock"
    `HAL_SD_Init` leaves `SDMMC_CK` at the 400 kHz identification divider even
    after the card is identified (it does not auto-apply `ClockDiv=0`). The
    24 MHz / 4-bit setting only takes effect when `HAL_SD_ConfigWideBusOperation`
    re-runs `SDMMC_Init`, so it is **always called, on both the 4-bit and 1-bit
    paths**. Build with `-DSD_BUS_WIDE_1B` to force 1-bit (a debugging escape
    hatch).

## DMA (DMA2)

| Direction | Stream / channel | Setting |
|-----------|------------------|---------|
| Receive (card→mem) | **DMA2 Stream3 / Ch4** | PERIPH→MEMORY |
| Transmit (mem→card) | **DMA2 Stream6 / Ch4** | MEMORY→PERIPH |

Common config: SDMMC as the flow controller (**`DMA_PFCTRL`**), 32-bit words,
**INC4 bursts**, full FIFO threshold, very-high priority (RM0385 §35.3.2 /
Table 26). This is the **first DMA-using subsystem** in the firmware.

## DMA completion synchronization

HAL_SD runs the data phase on DMA and signals completion **asynchronously**:

- **Read**: DMA Rx transfer complete (`DMA2_Stream3_IRQHandler` →
  `SD_DMAReceiveCplt` → `HAL_SD_RxCpltCallback`)
- **Write**: DMA Tx complete (`DMA2_Stream6_IRQHandler`) arms the DATAEND
  interrupt → when the card finishes programming, **`SDMMC1_IRQHandler`** →
  `HAL_SD_TxCpltCallback`

So **all three IRQs (SDMMC1 and DMA2 Stream3/6) are enabled**. Each ISR wraps the
HAL handler in the execution-profile (#19) enter/exit under PRIMASK, exactly like
USART1. NVIC priorities are SDMMC1=6 and DMA2_Stream3/6=7 (below USART1=5, above
SysTick=14). The ThreadX M7 port is PRIMASK-based, so `tx_semaphore_put` from an
ISR is safe regardless of the numeric priority.

Completion is signaled through a **count-0 `TX_SEMAPHORE`**; the calling thread
waits on it with a **finite timeout** (no busy-wait). To stop a stale or late
callback from faking success:

- each operation **drains** the semaphore and sets `sd_xfer_active=1` right
  before issuing the DMA
- the callback posts the semaphore only while `sd_xfer_active` is set
- on timeout/error it clears `sd_xfer_active=0` **first**, then calls
  `HAL_SD_Abort` (synchronously clears the SDMMC interrupts/flags and aborts the
  DMA), then drains again
- a write waits for the TRANSFER state again after the last chunk so the data is
  committed

## D-cache coherency (bounce-buffer scheme)

The DMA always targets **`sd_bounce`** only (`8*512 = 4 KiB`, **32 B aligned**,
placed in **SRAM1** via the linker `.sram1_dma` section). The caller's buffer is
touched solely by CPU memcpy, so any alignment is safe.

- **Read**: `SCB_InvalidateDCache_by_Addr(sd_bounce)` before the DMA (so a dirty
  eviction mid-transfer cannot corrupt the DMA'd data) → DMA → invalidate again
  after completion (drop speculative prefetches) → `memcpy` into the caller's
  buffer
- **Write**: `memcpy` into `sd_bounce` → `SCB_CleanDCache_by_Addr(sd_bounce)`
  (flush to physical SRAM) → DMA

`sd_bounce` is 32 B aligned and the chunk length is a multiple of 512 (hence of
32 B), so the clean/invalidate acts only on `sd_bounce`'s own cache lines and
**never disturbs an adjacent buffer**.

!!! note "Why SRAM1, not DTCM"
    DMA2 *can* reach the DTCM through the M7 AHBS slave (RM0385 §2.1.1/§2.1.6).
    SRAM1 is chosen not because the DTCM is unreachable but to **confine D-cache
    maintenance to one 32 B-aligned dedicated buffer**. The linker ASSERTs the
    `.sram1_dma` start ≥ `0x20010000` and end ≤ `0x2004C000` (the DTCM/SRAM1
    region split was introduced in #33).

## Concurrency and thread context

- The public API serializes a whole transfer (state wait → DMA → completion →
  cache) on an internal ThreadX mutex (TX_INHERIT), so shell fg/bg callers can
  interleave safely.
- **Thread-context only**; never call it from an ISR.
- Init runs once from `tx_application_define()` (`src/main.c`, right after
  `fs_glue_init()`); it issues no card I/O (only GPIO/DMA/NVIC/clock-source mux
  plus mutex/semaphore creation). Card identification (`HAL_SD_Init`) happens
  lazily on the first `sd` command.

## `sd` shell command

Low-level (card) commands:

```
sd info        card type / capacity / block geometry / bus width / CID / CSD
sd read <lba>  hexdump one 512 B block (LBA addressing)
```

Filesystem (FileX FAT, #34) commands -- sharing the common core with `fs` (QSPI):

```
sd ls [path]        list a directory (default /)
sd cat <path>       print a file
sd write <p> <txt>  create/overwrite a file (quote for spaces)
sd rm <path>        delete a file / empty directory
sd mkdir <path>     create a directory
sd df               filesystem capacity / free / FAT type (64-bit)
sd umount           flush + unmount
```

- The filesystem **lazy-mounts** on the first `sd` FS command (no reformat), so a
  PC-created FAT32 reads/writes directly and interoperates. Architecture:
  [Filesystem](../rtos/filesystem.md).
- **`sd info` / `sd read` are raw-gated**: they may re-identify the card, so they
  are refused while the FS is mounted (`sd umount` first). When unmounted they
  behave as before (probe only when needed).
- Both MBR (standard PC format) and superfloppy (VBR @ 0) cards mount (the driver
  detects the layout at LBA 0).
- `sd format` (FAT32) is Phase C.

## Bring-up check

Insert a microSD formatted (FAT32 etc.) on a PC (**non-destructive**):

```
sh> sd info
type      : SDHC/SDXC (v2.x)
capacity  : 29820 MiB (61071360 blocks x 512 B)
bus width : 4-bit
rca       : 0xaaaa
...
sh> sd read 0          # MBR / boot sector
00000000  eb 3c 90 ...
000001f0  ... 55 aa    # 55 aa at offset +510
```

Troubleshooting:

- `no card in slot` → card-detect pin (PC13) / insertion
- `sd info` prints but `55 aa` is garbled → read invalidate ordering
- command hangs → DMA completion signal not wired / NVIC / `__HAL_LINKDMA`
- immediate error → clock-source mux / 4-bit negotiation failure (isolate with
  `-DSD_BUS_WIDE_1B`)
