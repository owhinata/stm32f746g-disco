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
