# Memory benchmark command (`membench`)

Issue #57. Where `size` / `free` ([Memory usage](shell-free.md)) report **capacity**, `membench` reports each
memory's **speed** — sequential bandwidth (read/write/copy, MB/s) and a **pointer-chase latency curve**
(ns/access, swept working set) — measured at cycle precision with the Cortex-M7 **DWT CYCCNT**. The goal is to
make the **L1 D$(16KB) → SRAM → SDRAM latency step** visible on the board (input for #65 `.sdram` layout and
#49 NetX buffer sizing). It lives in `shell/cmds/cmd_membench.c` and, like `coremark`/`sdram`, is linked into
the `shell` executable only.

lmbench is a large POSIX/process/mmap/FS suite and is **not ported**; only its two core ideas are borrowed
(`bw_mem` bandwidth / `lat_mem_rd` pointer chase) into a self-contained micro-bench (~280 lines).

## Command

| Command | Registration | Behaviour |
|---|---|---|
| `membench [region]` | `CLI_CMD_REGISTER(membench, NULL, ..., cmd_membench, 1, 1)` | Show bandwidth + latency curve |

`region` ∈ `{dtcm, sram, sdram, flash, all}`, default `all`. `membench x` errors on an unknown region. The whole
run takes under a second. Each measurement boundary polls `cli_cancel_requested()`, so **Ctrl+C interrupts** it
(prints `^C` and restores the prompt — cooperative cancellation #16).

```text
sh> membench
DWT CYCCNT @216MHz; warm-up + tick-guarded min; D$=16KB/32B line; SDRAM/DTCM non-cacheable.

bandwidth (MB/s)             read    write     copy
  DTCM   (16KB)               618      756      425
  SRAM   ( 4KB, cached)       550      751      421
  SRAM   (64KB, refill)       384      756      297
  SDRAM  (64KB, non-cache)     69      208       31
  Flash  (64KB, AXIM+L1D$)    276       --       --

latency (ns/access, dependent-load chain, 64B stride)
  WSS       DTCM    SRAM   SDRAM
  1KB       14.0    14.0   104.4
  2KB       14.0    14.0   103.6
  4KB       14.0    14.4    99.2
  8KB       14.0    36.1   104.5
  16KB      14.0    42.2   104.1
  32KB        --    44.6   104.5
  64KB        --    46.0   104.3
```

A sample measurement (216 MHz, LTDC/camera idle). SRAM latency **steps from ~14 ns for WSS≤4KB (L1 D$ hit) up
to ~36→46 ns from 8KB on** (D$ overflow → SRAM refill); DTCM is flat-fastest ~14 ns; SDRAM is flat ~104 ns
(non-cached) — the D$→SRAM→SDRAM hierarchy is visible as intended. Bandwidth likewise orders DTCM >
SRAM(cached) > SRAM(refill, read) > SDRAM. SRAM write being ~750 for both cached and refill is the write-back
cache absorbing the stores (the CPU-observed caveat above).

## Measurement machinery

### Clock: DWT CYCCNT (cycle precision)

DWT is normally unused (the ThreadX exec-profile uses TIM2 because DWT freezes with the core clock in WFI sleep,
[#19](https://github.com/owhinata/stm32f746g-disco/issues/19)). `membench` enables it locally:

```c
CoreDebug->DEMCR |= TRCENA;                  /* trace enable               */
*(volatile uint32_t *)0xE0001FB0 = 0xC5ACCE55;  /* DWT_LAR unlock (M7)     */
if (DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) abort; /* CYCCNT not implemented     */
DWT->CYCCNT = 0; DWT->CTRL |= CYCCNTENA;
/* self-test: retry up to 3x until CYCCNT advances, else abort (anti-hang)   */
```

!!! warning "Cortex-M7 DWT software lock (CYCCNT freezes at 0 with no debugger)"
    The M7 DWT has a **software lock**: even with TRCENA + CYCCNTENA set, **CYCCNT stays frozen at 0 when no
    debugger is attached**. Writing the key `0xC5ACCE55` to `DWT->LAR` (0xE0001FB0) **unlocks it and the counter
    starts**. Skipping this makes every measured delta 0, which clamps the calibration's reps to the maximum →
    a **multi-minute hang with all-zero results**. `membench` therefore **self-tests that CYCCNT advances** after
    unlocking and aborts immediately if it does not (and `calibrate` clamps reps to 1 on an anomalous delta),
    so it can never hang.

TRCENA can be set in software without a debugger attached (RM0385 §40.13; CMSIS `core_cm7.h`: NOCYCCNT bit25 /
CYCCNTENA bit0 / DEMCR.TRCENA bit24). **The bench never sleeps**, so the DWT-freeze issue (WFI) does not apply.
The clock value is read from `HAL_RCC_GetHCLKFreq()` (not a hardcoded 216 MHz). Sampling boundaries use `__DSB();__ISB();`.

- `MB/s = bytes × hclk / (cycles × 1e6)`, `ns/access = cycles × 1e9 / (hclk × accesses)` (64-bit intermediate).

### Preemption removal: short runs + tick-guard rejection + min

Each measurement calibrates the cycles-per-access, then fixes the iteration count so **one run is ~0.3 ms
(< one 1 kHz SysTick period)**. Each run reads `HAL_GetTick()` before and after; **runs during which the
millisecond tick advanced (a SysTick ISR fired) are rejected**, and the **minimum** over the tick-clean runs is
reported (up to 16 attempts, stop after 3 clean). Interrupts are **never disabled** (issue constraint: no long
IRQ-off, so the prio-5 IWDG petter keeps refreshing).

!!! note "What tick-guard guarantees"
    Tick-guard removes only **SysTick-ISR** contamination (`SysTick_Handler → HAL_IncTick`). Other IRQs (USART,
    DMA, DCMI completion), the camera producer wake, and LTDC/FMC bus-master contention are NOT detectable via
    the tick; they are merely reduced by the min and by running with **`camera stream stop` / `lcd disable`**.
    It does not guarantee a fully uncontended run.

### DCE / line-reuse defeat

So `-O2/-O3` cannot delete the loops: reads go through `const volatile uint32_t*` into a non-volatile
accumulator ending in a volatile sink; writes are `volatile uint32_t*` stores; both unrolled ×8. **Latency is a
dependent load chain** `p = *(void**)p` (each load depends on the previous result, suppressing the core's
speculative issue and line reuse).

## Bandwidth (bw_mem) — cache-aware labelling

| Row | Meaning |
|---|---|
| **DTCM (16KB)** | tightly-coupled, non-D-cached (single-cycle class) real memory bandwidth |
| **SRAM (4KB, cached)** | resident in the D$(16KB) ⇒ **L1 D$ speed** |
| **SRAM (64KB, refill)** | exceeds the D$. read is refill-bound, close to real SRAM read. **write/copy is CPU-observed streaming throughput, NOT an external write-completion figure** (write-back + write-allocate: the last dirty footprint is not written back to external SRAM at measurement end unless cleaned) |
| **SDRAM (64KB, non-cache)** | MPU Normal non-cacheable (`src/bsp.c`) ⇒ real SDRAM bandwidth |
| **Flash (64KB, AXIM+L1D$)** | read only. A data read at 0x08000000 goes via **AXIM and is L1 D-cached** (the ART accelerator serves the instruction path, not this data read — RM0385 §3.2/§3.4) |

`copy` moves each buffer's first half into its second half, so the **MB/s denominator is the actually-copied
bytes (= half the span)**.

## Latency curve (lat_mem_rd)

A **deterministic stride permutation** builds the chase chain: for working set W, `n = W/64` nodes (stride 64B ≥
the 32B cache line, so each node hits a distinct line), linked `slot[i] → slot[(i+1)%n]` in a single cycle (no
RNG, reproducible). W is swept **1–64 KB** (up to each region's buffer):

- **DTCM**: W ≤ 16KB (16KB buffer). Non-cached, near-flat fastest baseline; 32/64KB show `--`.
- **SRAM**: W ≤ 64KB. Cacheable ⇒ **step above 16KB** (D$ overflow → SRAM refill).
- **SDRAM**: W ≤ 64KB. Non-cacheable ⇒ flat but higher latency than SRAM.

Flash latency is out of scope (a self-referential const chain is hard to generate at link time ⇒ read
bandwidth only).

## Buffer placement

| Region | Buffer | Placement |
|---|---|---|
| DTCM | `dtcm_bench_buf[16KB]` | **new `.dtcm_bench`(NOLOAD)** (`ldscript/STM32F746NGHx_FLASH.ld`, after `.log_noinit`, ASSERT keeps it inside the 64KB DTCM) |
| SRAM | `sram_bench_buf[64KB]` | `.bss` (zero-init) |
| SDRAM | `sdram_bench_buf[64KB]` | existing `.sdram`(NOLOAD) |
| Flash | none | reads from the flash base (`FLASH_BASE`) |

New reservations are DTCM 16KB / SRAM 64KB / SDRAM 64KB (all diagnostic-only, reflected in `free`'s used). The
`.dtcm_bench` section is the **only linker change**. `sdram`/`all` checks `sdram_is_up()` and skips the SDRAM
measurement (shows `down`) when the FMC is down.

## Verification

- **Build**: `cmake --build build`. Linker `Used Size` grows DTCM +16KB (24608B) / RAM +64KB / SDRAM +64KB.
- **On target** (`/dev/ttyACM0` @115200 8N1):
  - `membench` appears in `help`; Tab completion `mem` → `membench`. `membench` / `membench sram` /
    `membench x` (usage err).
  - bandwidth: DTCM > SRAM(cached) ≳ SRAM(refill, read) > SDRAM ordering, Flash read shown.
  - latency curve: **SRAM flat-low for WSS≤16KB → step up at 32/64KB**, DTCM flat-fastest, SDRAM high latency.
  - Ctrl+C interrupts; repeated runs are stable (tick-guarded min); no watchdog reset.
