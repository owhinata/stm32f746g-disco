# STM32F746G-DISCO — Eclipse ThreadX on the official ST HAL

STM32F746G-DISCO (STM32F746NGH6) firmware built with the **official
STMicroelectronics HAL**. The HAL/CMSIS sources and the ARM GNU toolchain are
fetched automatically — nothing to install by hand except `cmake`, `ninja`,
`git`, `curl`, and (for flashing) `st-flash`.

| App             | What it does                                                       |
|-----------------|-------------------------------------------------------------------|
| `threadx`       | Eclipse ThreadX RTOS: two threads (LED blink + UART print)        |
| `coremark`      | EEMBC CoreMark benchmark — **optional**, `-DBUILD_COREMARK=ON`     |
| `thread_metric` | Thread-Metric RTOS benchmark — **optional**, `-DBUILD_THREAD_METRIC=ON` |

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
include/        main.h (LED defines), bsp.h, stm32f7xx_it.h
src/            bsp.c (clock/cache/UART/printf), app_threadx.c, stm32f7xx_it.c
port/threadx/   tx_user.h, tx_glue.c (low-level init + SysTick)
port/coremark/  core_portme.{c,h}   CoreMark port (kept for the optional app)
ldscript/       STM32F746NGHx_FLASH.ld
cmake/          arm-none-eabi-toolchain.cmake (auto-downloads the toolchain)
CMakeLists.txt  builds threadx (and coremark when -DBUILD_COREMARK=ON)
lib/            git submodules:
                  stm32f7xx_hal_driver   (ST official HAL)
                  cmsis_device_f7        (startup, system_*, device headers)
                  cmsis_core             (CMSIS Core, cm7 branch)
                  threadx                (Eclipse ThreadX RTOS)
                  coremark               (EEMBC CoreMark — used by the optional app)
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
cmake --build build            # builds threadx
```

Artifacts land in `build/threadx.{elf,hex,bin}`.

To also build CoreMark (the submodule and port are kept in the tree):

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
      -DBUILD_COREMARK=ON
cmake --build build            # now builds threadx + coremark
```

## Flash / run

```bash
cmake --build build --target flash-threadx    # ThreadX demo
cmake --build build --target flash-coremark   # CoreMark (needs -DBUILD_COREMARK=ON)
```

Each `flash-<app>` target writes `<app>.bin` to `0x08000000` over ST-Link and
resets. (`st-flash reset` resets without reflashing.)

Watch the VCP (`/dev/ttyACM0`) with any serial terminal at 115200 8N1, e.g.:

```bash
picocom -b 115200 --imap lfcrlf /dev/ttyACM0
```

## ThreadX (Eclipse ThreadX)

The `threadx` app runs the upstream **Eclipse ThreadX** RTOS (MIT,
`lib/threadx` submodule) with the Cortex-M7/GNU port. `tx_application_define()`
creates two threads:

- `led`   — toggles LD1 every 250 ms
- `print` — prints a counter over the VCP every 1 s (run them concurrently)

Integration lives in `port/threadx/`:

- `tx_glue.c` provides `_tx_initialize_low_level()` and the `SysTick_Handler`,
  which drives both the HAL tick (`HAL_IncTick`) and ThreadX (`_tx_timer_interrupt`)
  off the single 1 ms SysTick. ThreadX's `PendSV_Handler` comes from the port, so
  `src/stm32f7xx_it.c` is excluded from this app.
- `tx_user.h` sets `TX_TIMER_TICKS_PER_SECOND = 1000` (1 tick = 1 ms).

**Interrupt-priority gotcha (important):** SysTick must be a *higher* priority
than PendSV (here PendSV = 15, SysTick = 14). When idle, ThreadX spins inside the
PendSV handler waiting for the timer to make a thread ready; if SysTick shared
PendSV's priority it could not preempt that spin, the tick would stall, and
sleeping threads would never wake. Critical sections use PRIMASK, so the higher
SysTick priority is still masked safely inside ThreadX.

## CoreMark result (optional app)

Built with `-DBUILD_COREMARK=ON` and measured on this board (216 MHz, GCC 13.3,
`-O3`, performance run, auto-calibrated to ~11.8 s / 11000 iterations):

```
CoreMark 1.0 : 928.818711 / GCC13.3.1 -O3 -mcpu=cortex-m7 -mfpu=fpv5-sp-d16 -mfloat-abi=hard / STACK
Correct operation validated.
```

**CoreMark ≈ 928.8 ≈ 4.30 CoreMark/MHz.** The port times the run with the 1 ms
SysTick (`HAL_GetTick`) and prints via the VCP `printf`; CoreMark self-calibrates
the iteration count (`ITERATIONS=0`) to run 10–100 s. With the I/D-cache enabled
the hot loops are cache-resident, so the flash 7-wait-state penalty is already
hidden — moving the kernels into ITCM was measured to add only ~0.6 %, and the
remaining gap to ST's published ~1082 (5.0/MHz) is compiler-bound (IAR vs GCC).

## Thread-Metric (optional app)

The Thread-Metric RTOS benchmark suite (8 tests measuring RTOS event throughput)
runs on ThreadX. The `lib/threadx/utility/benchmarks/thread_metric` sources are
used as-is; the runner lives in `src/app_thread_metric.c` + `port/threadx/`.

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
      -DBUILD_THREAD_METRIC=ON -DTHREAD_METRIC_TEST=basic
cmake --build build --target flash-thread_metric
```

`THREAD_METRIC_TEST` selects the test: `basic` (default), `cooperative`,
`preemptive`, `memory`, `message`, `sync`, `interrupt`, `interrupt_preempt`.
Results print over the VCP every 30 s, e.g. the basic test:

```
**** Thread-Metric Basic Single Thread Processing Test **** Relative Time: 30
Time Period Total:  252879
```

Integration notes:

- ThreadX runs at **100 Hz** here (`TX_GLUE_TICK_DIV=10`,
  `TX_TIMER_TICKS_PER_SECOND=100`) to match the porting layer's
  `TM_THREADX_TICKS_PER_SECOND`, so `tm_thread_sleep(30)` is a real 30 s.
- The interrupt tests raise `SVC #0` (`TM_CAUSE_INTERRUPT`); `port/threadx/tm_svc.c`
  routes `SVC_Handler` to `tm_interrupt_handler` (linked only for those tests).
- The benchmark counters are non-volatile globals updated in tight loops, so the
  selected test source is built at `-O0` (otherwise GCC keeps the counter in a
  register and the reporting thread sees a stale 0 → "thread died"). ThreadX
  itself stays at `-O2`, so the measured RTOS operations are representative.

## Notes

- No software bootloader: each app is linked at `0x08000000` and uses the full
  1 MB flash. Field updates can use the STM32 ROM bootloader (BOOT0 high → USB
  DFU / UART) without changing the image.
- `rm -rf build` clears build artifacts; `rm -rf tools` also removes the
  downloaded toolchain.
- The VCP `printf` float support (`%f` in CoreMark) is enabled by linking with
  `-u _printf_float` (nano newlib) on the `coremark` target only.
