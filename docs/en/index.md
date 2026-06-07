# STM32F746G-DISCO Guide

**Bare-metal + Eclipse ThreadX** firmware for the STM32F746G-DISCO
(STM32F746NGH6 / Cortex-M7), built on the official ST HAL with CMake + Ninja.
The HAL/CMSIS/ThreadX/CoreMark sources and the ARM GNU toolchain are fetched
automatically on the first configure.

## Contents

- [Hardware / board](hardware/board.md) — clock, caches, VCP, LED, memory map, references
- [Build (CMake)](build/cmake.md) — configure / build / flash, toolchain auto-download, submodules
- [RTOS (ThreadX)](rtos/threadx.md) — ThreadX integration and the interrupt-priority gotcha

## Firmware

A single firmware, **`threadx`** = the interactive ThreadX shell (USART1 VCP).
Everything else runs as a shell command.

| Command | What it does |
|---------|--------------|
| `help` / `echo` | list commands / echo |
| `version` / `uptime` / `reboot` | firmware & MCU info / uptime / reset |
| `thread` | thread list + stack usage |
| `devmem` | memory peek/poke/dump (address-range gated) |
| `coremark` | run the EEMBC CoreMark benchmark (~12s) |

## Chip configuration

- Cortex-M7, hardware FPU (`fpv5-sp-d16`)
- I-Cache / D-Cache enabled
- SYSCLK **216 MHz** (HSE 25 MHz → PLL M=25 N=432 P=2, VOS1 + over-drive, 7 flash wait states)
- VCP: USART1 (TX=PA9 / RX=PB7, 115200 8N1, ST-Link virtual COM)
