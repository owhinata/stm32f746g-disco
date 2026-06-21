# SDRAM (FMC, 8 MB external RAM)

The board's 128-Mbit SDRAM (Micron **MT48LC4M32B2**, 4M×32) is driven by the STM32F746 **FMC SDRAM controller**. The driver lives in `port/sdram/sdram.{c,h}`, the shell command is `sdram` (`shell/cmds/cmd_sdram.c`).

This is **Phase 1.5** (#40) of the camera epic (#22). Its primary purpose is hosting **large DMA-target buffers** (the camera frame buffer, #41).

## Configuration

| Item | Value | Source |
|------|-------|--------|
| Device | MT48LC4M32B2 (128 Mbit, 4 banks × 4096 rows × 256 cols × 32 bit) | UM1907 §6.13 |
| Bus width | **16-bit** (DQ16-31 are pulled down with 10 kΩ on the board, unconnected) | UM1907 §6.13 |
| Usable size | **8 MB** @ **0xC0000000** (FMC bank 1) | 4M words × 2 B |
| SDCLK | **HCLK/2 = 108 MHz** (`FMC_SDRAM_CLOCK_PERIOD_2`) | HCLK = 216 MHz |
| CAS latency | **3** (controller and mode register) | datasheet ratings (see note) |
| Refresh | COUNT=0x0603 (BSP value, computed for 100 MHz) | at 108 MHz rows refresh every ~14.4 µs -- **early**, i.e. on the safe side |
| Timings | TMRD=2 / **TXSR=8** / **TRAS=5** / TRC=7 / TWR=2 / TRP=2 / TRCD=2 (SDCLK cycles) | derived to meet the datasheet minimums at the 9.26 ns period |

!!! warning "Why the BSP's CAS2 cannot be reused (caught in codex-review)"
    The ST BSP (`stm32746g_discovery_sdram.c`) uses CAS2 -- but it assumes **SDCLK = 100 MHz (HCLK 200 MHz)**. This firmware runs HCLK = 216 MHz → SDCLK = 108 MHz, and the MT48LC4M32B2-6 rates **CL2 only up to 100 MHz (tCK(2) ≥ 10 ns)** -- a violation. CL3 is rated to 167 MHz (tCK(3) ≥ 6 ns). TXSR/TRAS also fall short at the 9.26 ns period with the 10 ns-based BSP cycle counts (7/4), missing the datasheet minimums (67 ns / 42 ns), so each gains one cycle. Read sampling margin actually improves (tAC(3) ≤ 5.4 ns against a 9.26 ns period), so RBURST/RPIPE_0 carry over from the BSP.

Pins (all AF12, pull-up): PC3 (SDCKE0) / PD0,1,8-10,14,15 (D2,D3,D13-15,D0,D1) / PE0,1,7-15 (NBL0/1, D4-D12) / PF0-5,11-15 (A0-A5, SDNRAS, A6-A9) / PG0,1,4,5,8,15 (A10,A11,BA0,BA1,SDCLK,SDNCAS) / PH3 (SDNE0), PH5 (SDNWE). No conflict with the existing peripherals (USART1 / QSPI / SDMMC1 / DCMI / I2C1 / LED).

## MPU: Normal non-cacheable (DMA coherency by construction)

In the ARMv7-M default memory map, 0xC0000000 is **Device-typed (XN)**. `bsp_init()` (`mpu_config_sdram()` in `src/bsp.c`) remaps the 8 MB through MPU region 0 as **Normal, non-cacheable, RW, XN** -- **before the D-cache is enabled**:

- **Normal**: avoids Device semantics (word-by-word serialization) for plain data access
- **Non-cacheable** (TEX=1, C=0, B=0): the region's main use is DMA writes (DCMI) followed by CPU reads. Since no cache line is ever allocated, **DMA/CPU coherency problems cannot exist** -- no clean/invalidate choreography, no dirty-eviction races. The cost is slower CPU access; switching to WBWA plus explicit maintenance remains an option if a consumer needs the bandwidth
- **PRIVDEFENA**: every other address keeps the default map (flash/SRAM/peripheral behaviour unchanged)

!!! note "Contrast with the SD bounce buffer (SRAM1)"
    The SDMMC driver keeps its DMA in **cached SRAM1** and manages coherency with a dedicated 32 B-aligned bounce buffer plus clean/invalidate (see [microSD](sdmmc.md)). The SDRAM takes the opposite approach -- make the whole region non-cacheable and eliminate maintenance altogether. The former suits small, frequent transfers that benefit from caching; the latter suits large DMA targets.

## Linker `.sdram` section (the NOLOAD constraint)

`MEMORY` gains `SDRAM 0xC0000000 8M` and a `.sdram (NOLOAD)` section. **The FMC is uninitialized until `sdram_init()` runs**, so the startup code (`.data` copy / `.bss` zeroing) must never touch the region -- objects placed in `.sdram` get **no load image, no zero-init, and do not survive reset**.

```c
static uint16_t cam_view_buf[320*240] __attribute__((aligned(32), section(".sdram.fixed")));
```

## FMC internal-bank placement (#65)

The SDRAM has **4 internal banks** (2 MB each, selected by CPU address **offset bit[22:21]**, RM0385 §13.5.3); each bank can hold one **open row** independently. `.sdram` is split into two bank-aligned sub-regions pinned by the linker:

| Sub-region | Bank | Address | Contents |
|---|---|---|---|
| `.sdram.fixed` (front `.sdram.fixed.ltdc`) | **bank0** | 0xC0000000–0xC01FFFFF | Fixed residents: **`ltdc_fb` (LTDC scan-out READ surface, address-pinned at the front)** / `cam_frame` (snapshot) / `cam_view_buf` / `guix_demo_img` / `sdram_bench_buf`. ~1.45 MB (< 2 MB) |
| `.sdram.cam` | **bank1** | 0xC0200000–0xC03FFFFF | `cam_arena` (**2 MB camera DMA ring arena**, the DCMI WRITE target) |
| (free) | bank2,3 | 0xC0400000–0xC07FFFFF | 4 MB unused (future: #49 NetX, etc.) |

- **Goal (an FE-reduction lever)**: keeping the LTDC READ surface (`ltdc_fb`, bank0) and the DCMI ring WRITE target (`cam_arena`, bank1) in **different banks** lets each bank keep its own row open, avoiding the same-bank row activate/precharge thrashing that drives DMA FIFO errors (FE). Four linker `ASSERT`s enforce "fixed fits in bank0 (< 2 MB) / cam starts at bank1 / cam is exactly 2 MB / total ≤ 8 MB".
- **The effect is measurement-dependent** (below). Even with no effect the placement is harmless -- same MPU region, only the address differs.
- **The camera ring is dynamically partitioned**: the old `cam_ring[4][256KB]` (fixed 1 MB) is gone; at stream start the slot stride = `align32(frame_bytes)` and the slot count = `min(2MB/stride, 8)` are computed from the current mode (fewer than 2 slots is rejected), so small modes get a deeper ring. Observable via `camera stream stats`'s `ring: N slots x M B`.
- `cam_frame` (snapshot) shares bank0 with `ltdc_fb`, but snapshot is one-shot and tolerates FE (FE/DME IRQs are disabled for it, #45); the FE-critical streaming path (bank1 vs bank0) is separated.

**Measured (FE reduction, LTDC scanout ON, same conditions, `camera stream stats`'s `dma fe/s`)**:

| Mode | Old layout (same bank) | #65 (bank0 LTDC / bank1 ring) | Reduction |
|---|---|---|---|
| QVGA RGB565 | 1193.3 fe/s | **859.3 fe/s** | **−28%** |
| 480x272 RGB565 | 2022.0 fe/s | **1489.5 fe/s** | **−26%** |

Both keep `ovr dcmi`/`ovr ring` = 0 and fps 14.8. The bank separation cut **FE by ~26–28%** -- the "uncertain" side-effect confirmed on hardware. The dynamic arena gives both modes `ring: 8 slots` (deeper than the old fixed 4).

## Initialization flow

`sdram_init()` (from `tx_application_define`, before `camera_init()`):

1. GPIO (AF12) + FMC clock
2. `HAL_SDRAM_Init` (16-bit / CAS3 / SDCLK=HCLK/2, timings above)
3. JEDEC power-up sequence: clock enable → **100 µs wait** (`udelay` -- a TIM2 busy-wait, since the ThreadX tick is not running yet) → precharge-all → 8× auto-refresh → load mode register (BL=1 / sequential / CAS3 / single write) → refresh counter

Polling only (no interrupts, DMA, or ThreadX objects). Idempotent and fail-soft (on failure, `sdram` / `camera capture` report it; everything else keeps running).

## The `sdram` shell command

```
sdram info           window / config / state
sdram test [bytes]   DESTRUCTIVE write/read-back memtest (default: all 8 MB, multiple of 4)
```

The `devmem` region allow-list also gains the SDRAM window (0xC0000000, 8 MB), so `devmem peek/poke/dump` reach it directly (anything past the 8 MB window stays rejected).

`sdram test` runs three passes: (1) the **address pattern** (each word holds its own address -- catches stuck/shorted/aliased address lines, which a single repeated value would miss), (2) 0x55555555, (3) 0xAAAAAAAA (alternating data lines in both polarities). Each pass writes the full span and then reads it back, which also exercises refresh across the multi-ms gap. **Destructive** (clobbers `.sdram` objects, e.g. a captured frame). Cancellable with Ctrl+C at 64 KB chunk boundaries.

**Suspend/invalidate contract for the destruction (#65, #47 downstream)**: the test overwrites all of `.sdram` including `ltdc_fb` (bank0), so it (1) **refuses** while the camera is streaming or GUIX owns the display, (2) **suspends** LTDC scanout for the duration if it was running (`ltdc_set_scanout(false)`, so the controller does not fetch the clobbered framebuffer and corrupt the screen), (3) invalidates the snapshot frame (`camera_frame_invalidate()`), and (4) after the test (a single restore path that also covers cancel/FAIL) **clears both framebuffers to black and restores scanout to its prior state** (no leftover pattern shown). If scanout was already off (`lcd off`), it is left untouched.

Example session:

```
sh> sdram test
sdram: DESTRUCTIVE test over 8192 KB (clobbers .sdram contents, e.g. a captured frame)
pass 1/3 (address): write+verify 8192 KB ... OK
pass 2/3 (0x55555555): write+verify 8192 KB ... OK
pass 3/3 (0xAAAAAAAA): write+verify 8192 KB ... OK
sdram: 8192 KB tested, no errors
```

## References

- UM1907 §6.13 — SDRAM (16-bit wiring, 64 Mbit accessible)
- RM0385 §13 — FMC SDRAM controller
- MT48LC4M32B2 datasheet — timing ratings, mode register
- `_ref/STM32Cube_FW_F7_V1.17.0/Drivers/BSP/STM32746G-Discovery/stm32746g_discovery_sdram.c` — reference implementation for timings/sequence (read-only)
