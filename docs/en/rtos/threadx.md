# RTOS (Eclipse ThreadX)

The `threadx` app runs upstream **Eclipse ThreadX** (MIT, `lib/threadx`
submodule, Cortex-M7/GNU port). `tx_application_define()` creates two threads:

- `led` — toggles LD1 every 250 ms
- `print` — prints a counter over the VCP every 1 s

## Integration (`port/threadx/`)

- `tx_glue.c`: provides `_tx_initialize_low_level()` and the `SysTick_Handler`,
  driving both the HAL tick (`HAL_IncTick`) and the ThreadX tick
  (`_tx_timer_interrupt`) off the single 1 ms SysTick. ThreadX's
  `PendSV_Handler` comes from the port (`tx_thread_schedule.S`), so
  `src/stm32f7xx_it.c` is excluded from this app.
- `tx_user.h`: `TX_TIMER_TICKS_PER_SECOND = 1000` (1 tick = 1 ms).

## Interrupt-priority gotcha (important)

**SysTick must be a higher priority than PendSV** (PendSV = 15, SysTick = 14).

When idle, ThreadX spins inside the PendSV handler waiting for a thread to become
ready. If SysTick shares PendSV's priority it cannot preempt that spin (equal
priority does not preempt), the tick stalls, and sleeping threads never wake
(deadlock).

Critical sections use PRIMASK (`CPSID i`), so the higher SysTick priority is
still masked safely inside ThreadX.

!!! note "How to verify"
    Read `_tx_timer_system_clock` over SWD one second apart: it advances by
    ~1000/s when healthy. If it is stuck, the tick is broken. Check whether `$pc`
    is stuck in `__tx_ts_wait` (the PendSV idle spin).

## Build notes

- Compile `common/src/*.c` (core) + `ports/cortex_m7/gnu/src/*.S` (context
  switch etc.). `tx_misra` (both `.c` and `.S`) is **excluded** (duplicate
  definitions).
- The port `.S` files `#include "tx_user.h"`, so the ASM include path must be
  set and `TX_INCLUDE_USER_DEFINE_FILE` defined.

## Thread-Metric benchmark (optional)

The Thread-Metric suite bundled with ThreadX (RTOS benchmark, 8 tests) runs as
the `thread_metric` app.

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
      -DBUILD_THREAD_METRIC=ON -DTHREAD_METRIC_TEST=basic
cmake --build build --target flash-thread_metric
```

`THREAD_METRIC_TEST`: `basic` (default) / `cooperative` / `preemptive` /
`memory` / `message` / `sync` / `interrupt` / `interrupt_preempt`. Results print
over the VCP every 30 s (e.g. basic: `Time Period Total: 252879`).

Integration notes:

- ThreadX runs at **100 Hz** (`TX_GLUE_TICK_DIV=10` +
  `TX_TIMER_TICKS_PER_SECOND=100`) to match the porting layer's
  `TM_THREADX_TICKS_PER_SECOND`, so `tm_thread_sleep(30)` is a real 30 s.
- The interrupt tests raise `SVC #0` (`TM_CAUSE_INTERRUPT`); `port/threadx/tm_svc.c`
  routes `SVC_Handler` to `tm_interrupt_handler` (linked only for those tests).
- The benchmark counters are non-volatile globals updated in tight loops, so the
  selected test source is built at `-O0` (at -O2 GCC keeps the counter in a
  register and the reporting thread reads a stale 0 → "thread died"). ThreadX
  stays at `-O2`.
