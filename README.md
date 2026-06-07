# STM32F746G-DISCO — Eclipse ThreadX shell on the official ST HAL

STM32F746G-DISCO (STM32F746NGH6) firmware built with the **official
STMicroelectronics HAL**. The HAL/CMSIS/ThreadX/CoreMark sources and the ARM GNU
toolchain are fetched automatically — nothing to install by hand except `cmake`,
`ninja`, `git`, `curl`, and (for flashing) `st-flash`.

There is a **single firmware**, `threadx`: an interactive **Eclipse ThreadX CLI
shell** over the ST-Link VCP. Other functionality runs as shell commands rather
than separate images.

| Command | What it does |
|---------|--------------|
| `help` / `echo` | list commands / echo the line |
| `version` / `uptime` / `reboot` | firmware & MCU identity / uptime / reset |
| `thread` | thread list + per-thread stack usage |
| `devmem` | memory peek / poke / dump (address-range gated) |
| `coremark` | run the EEMBC CoreMark benchmark (~12 s) |

Board bring-up (216 MHz clock, caches, VCP UART, printf) is shared in
`src/bsp.c`.

## Chip configuration

| Item    | Setting                                            |
|---------|----------------------------------------------------|
| Core    | Cortex-M7, hard-float (`fpv5-sp-d16`) — **FPU on** |
| I-Cache | **enabled** (`SCB_EnableICache`)                   |
| D-Cache | **enabled** (`SCB_EnableDCache`)                   |
| SYSCLK  | **216 MHz** from 25 MHz HSE (PLL M=25, N=432, P=2) |
| Power   | VOS scale 1 + over-drive, 7 flash wait states      |
| VCP     | USART1, TX=PA9 / RX=PB7, 115200 8N1 (ST-Link VCP)  |
| LED     | LD1 (green) on **PI1**                              |

## Layout

```
include/        main.h (LED defines), bsp.h
src/            main.c (shell instance + LED + tx_application_define), bsp.c
shell/          the CLI shell library: core/ backend/ cmds/ include/ test/
port/threadx/   tx_user.h, tx_glue.c (low-level init + SysTick)
port/coremark/  core_portme.{c,h}   CoreMark port (used by the `coremark` command)
ldscript/       STM32F746NGHx_FLASH.ld
cmake/          arm-none-eabi-toolchain.cmake (auto-downloads the toolchain)
CMakeLists.txt  builds the single firmware: threadx
lib/            git submodules:
                  stm32f7xx_hal_driver   (ST official HAL)
                  cmsis_device_f7        (startup, system_*, device headers)
                  cmsis_core             (CMSIS Core, cm7 branch)
                  threadx                (Eclipse ThreadX RTOS)
                  coremark               (EEMBC CoreMark — the `coremark` command)
```

`stm32f7xx_hal_conf.h` is generated at configure time from the upstream HAL
template (its default `HSE_VALUE` of 25 MHz already matches the board crystal).

## Build

The build is CMake + Ninja. The first `configure` downloads the ARM GNU
toolchain into `./tools` and inits the git submodules automatically.

```bash
git clone --recurse-submodules <this repo>
cd stm32f746g-disco
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
cmake --build build            # builds the single firmware: threadx
```

Artifacts land in `build/threadx.{elf,hex,bin}`.

## Flash / run

```bash
cmake --build build --target flash    # write threadx.bin over ST-Link
```

`flash` writes `threadx.bin` to `0x08000000` over ST-Link and resets.
(`st-flash reset` resets without reflashing.)

Watch the VCP (`/dev/ttyACM0`) with any serial terminal at 115200 8N1, e.g.:

```bash
picocom -b 115200 /dev/ttyACM0
```

(The firmware's printf retarget maps LF→CRLF, so `--imap lfcrlf` is not needed —
even the `coremark` report renders cleanly.)

At the `sh> ` prompt, `help` lists the commands; try `coremark`.

## ThreadX (Eclipse ThreadX)

The firmware runs the upstream **Eclipse ThreadX** RTOS (MIT, `lib/threadx`
submodule) with the Cortex-M7/GNU port. `tx_application_define()` creates:

- the shell instance thread (`vcp_sh`, priority 16) — the interactive CLI on the
  USART1 VCP
- `led` (priority 10) — toggles LD1 every 250 ms as a heartbeat (it keeps
  blinking even while `coremark` runs)

Integration lives in `port/threadx/`:

- `tx_glue.c` provides `_tx_initialize_low_level()` and the `SysTick_Handler`,
  which drives both the HAL tick (`HAL_IncTick`) and ThreadX (`_tx_timer_interrupt`)
  off the single 1 ms SysTick. ThreadX's `PendSV_Handler` comes from the port, so
  the firmware ships no `src/stm32f7xx_it.c` (other vectors fall to the startup
  file's weak `Default_Handler`).
- `tx_user.h` sets `TX_TIMER_TICKS_PER_SECOND = 1000` (1 tick = 1 ms).

**Interrupt-priority gotcha (important):** SysTick must be a *higher* priority
than PendSV (here PendSV = 15, SysTick = 14). When idle, ThreadX spins inside the
PendSV handler waiting for the timer to make a thread ready; if SysTick shared
PendSV's priority it could not preempt that spin, the tick would stall, and
sleeping threads would never wake. Critical sections use PRIMASK, so the higher
SysTick priority is still masked safely inside ThreadX.

## CoreMark (the `coremark` command)

Run `coremark` at the shell prompt. CoreMark is built once into the firmware
(`coremark_obj`: `-O3 -funroll-loops`, `MEM_METHOD=MEM_STATIC`, `core_main.c`
compiled `-Dmain=coremark_main`) and runs synchronously in the shell thread
(~12 s); details in `docs/*/rtos/shell-coremark.md`. Measured on this board
(216 MHz, GCC 13.3, `-O3`, performance run, auto-calibrated):

```
CoreMark 1.0 : 928.818711 / GCC13.3.1 -O3 -mcpu=cortex-m7 -mfpu=fpv5-sp-d16 -mfloat-abi=hard / STATIC
Correct operation validated.
```

**CoreMark ≈ 928.8 ≈ 4.30 CoreMark/MHz.** The port times the run with the 1 ms
SysTick (`HAL_GetTick`) and prints via the VCP `printf`; CoreMark self-calibrates
the iteration count (`ITERATIONS=0`) to run 10–100 s. With the I/D-cache enabled
the hot loops are cache-resident, so the flash 7-wait-state penalty is already
hidden — moving the kernels into ITCM was measured to add only ~0.6 %, and the
remaining gap to ST's published ~1082 (5.0/MHz) is compiler-bound (IAR vs GCC).

> The former standalone `thread_metric` and `exec_profile` images were retired:
> their ThreadX build configs (100 Hz tick + `TX_DISABLE_*`, and
> `TX_EXECUTION_PROFILE_ENABLE` rebuilding the port asm) are incompatible with the
> interactive shell's, so they cannot share the single firmware. Restore from git
> history (≤ `5078914`) if needed.

## Notes

- No software bootloader: the firmware is linked at `0x08000000` and uses the full
  1 MB flash. Field updates can use the STM32 ROM bootloader (BOOT0 high → USB
  DFU / UART) without changing the image.
- `rm -rf build` clears build artifacts; `rm -rf tools` also removes the
  downloaded toolchain.
- The VCP `printf` float support (`%f` in CoreMark's score line) is enabled by
  linking the `threadx` target with `-u _printf_float` (nano newlib).
