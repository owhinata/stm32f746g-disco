# Hardware / board

Target board: **STM32F746G-DISCO** (MCU: STM32F746NGH6, Cortex-M7, up to 216 MHz).

## Clock

| Item | Setting |
|------|---------|
| Source | HSE 25 MHz (on-board) |
| PLL | M=25, N=432, P=2 → SYSCLK 216 MHz |
| Power | VOS scale 1 + over-drive |
| Flash | 7 wait states (above 210 MHz) |
| Buses | HCLK 216 / APB1 54 / APB2 108 MHz |

Configured in `SystemClock_Config()` in `src/bsp.c`.

## FPU / caches

- FPU: enabled by `SystemInit()` (CMSIS) + hard-float build (`-mfpu=fpv5-sp-d16 -mfloat-abi=hard`)
- I-Cache / D-Cache: `SCB_EnableICache()` / `SCB_EnableDCache()` in `bsp_init()`

## VCP (virtual COM port)

The ST-Link V2.1 VCP is wired to **USART1**:

| Signal | Pin | AF |
|--------|-----|----|
| VCP_TX | PA9 | AF7 |
| VCP_RX | PB7 | AF7 |

115200 8N1. `printf` is retargeted to UART via newlib's `_write()` (`src/bsp.c`).

!!! warning "PA9 is shared"
    Per UM1907, PA9 is **shared between VCP_TX and OTG_FS_VBUS**. The factory
    solder-bridge configuration (R64=ON, R63=OFF, R58=ON) enables VCP_TX. If the
    board is reworked for USB OTG host, VCP_TX is no longer available.

## LED

- User LED **LD1 (green) = PI1** (active high)

## Memory map

| Region | Address | Size |
|--------|---------|------|
| Flash | 0x08000000 | 1 MB |
| ITCM-RAM | 0x00000000 | 16 KB |
| DTCM-RAM | 0x20000000 | 64 KB |
| SRAM1+2 | 0x20010000 | 256 KB |

Linker script: `ldscript/STM32F746NGHx_FLASH.ld` (matches startup symbols
`_estack`/`_sidata`/`_sbss` etc.).

## References

Placed under `_ref/` (not tracked by git):

- `rm0385-*.pdf` — STM32F75x/F74x reference manual
- `um1907-*.pdf` — Discovery kit user manual

## Backup / restore

```bash
st-flash read backup_full.bin 0x08000000 0x100000   # dump full flash
st-flash --reset write backup_full.bin 0x08000000   # restore
```
