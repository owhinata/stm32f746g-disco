# RTOS (Eclipse ThreadX)

The firmware (CMake target `threadx`) runs the interactive shell on top of
upstream **Eclipse ThreadX** (MIT, `lib/threadx` submodule, Cortex-M7/GNU port).
`tx_application_define()` creates:

- the shell instance thread (`vcp_sh`, priority 16) — the interactive CLI over the
  USART1 VCP ([Shell app](shell-app.md))
- `led` (priority 10) — toggles LD1 (PI1) every 250 ms as a heartbeat

## Integration (`port/threadx/`)

- `tx_glue.c`: provides `_tx_initialize_low_level()` and the `SysTick_Handler`,
  driving both the HAL tick (`HAL_IncTick`) and the ThreadX tick
  (`_tx_timer_interrupt`) off the single 1 ms SysTick. ThreadX's
  `PendSV_Handler` comes from the port asm, so the firmware ships no
  `src/stm32f7xx_it.c` (SVC has no user here, and every other vector falls to the
  startup file's weak `Default_Handler`).
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

## Benchmarks

CoreMark runs as the shell's **`coremark` command**, not a separate image (see
[CoreMark command](shell-coremark.md)).

!!! note "Retired benchmarks (thread_metric / exec_profile)"
    The former `thread_metric` (ThreadX tick at 100 Hz plus the `TX_DISABLE_*`
    flags) and `exec_profile` (`TX_EXECUTION_PROFILE_ENABLE`, which rebuilds the
    ThreadX port asm) needed a ThreadX configuration incompatible with the
    interactive shell (1 ms tick, error checking on, no asm overhead), so they
    cannot share the single firmware and were removed. Restore from git history
    (≤ `5078914`) if needed.
