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

## Idle power saving (WFI, `TX_ENABLE_WFI`)

`tx_user.h` defines `TX_ENABLE_WFI`, so when no thread is ready the port asm idle
loop (`__tx_ts_wait`) does not busy-spin: it executes `DSB; WFI; ISB` to **sleep
the CPU** until an interrupt arrives. In real use the shell is suspended waiting
on UART RX and `led` is mostly idle in `tx_thread_sleep`, so replacing the
busy-spin with WFI lowers power draw.

Cortex-M7 WFI Sleep **stops only CPU instruction execution; HCLK and the APB
clocks keep running** (RM0385 Sleep mode). That keeps every wake/timekeeping path
alive:

- **SysTick keeps ticking**: SysTick is clocked from HCLK and keeps counting in
  Sleep, so the 1 ms tick (both HAL tick and ThreadX tick) advances and
  `tx_thread_sleep` expirations still wake threads (the `led` 250 ms blink
  continues).
- **UART RX wakes the core**: USART1 RX is interrupt-driven ([UART
  backend](shell-backend-uart.md), IRQ priority 5). An incoming byte raises the
  USART1 interrupt, which wakes the core from WFI (WFI ignores PRIMASK for
  wake-up). The idle loop enters WFI after `CPSID i`, but a pending interrupt
  still wakes it; the ISR then runs after `CPSIE i`, sets `CLI_EVT_RX`, and the
  shell thread resumes.
- **Timekeeping (TIM2)**: the execution-profile time source (`thread` cpu%, see
  the [thread command](shell-thread.md)) is **TIM2**, not `DWT_CYCCNT`. DWT can
  freeze during WFI Sleep when the core clock is gated, whereas TIM2 keeps its
  clock in Sleep (`TIM2LPEN` reset value = 1), so cpu%/idle stay correct with WFI
  enabled.

!!! note "Debugging caveat (DBG_SLEEP)"
    OpenOCD's `target/stm32f7x.cfg` may set `DBG_SLEEP`/`DBG_STOP`/`DBG_STANDBY`
    in `DBGMCU_CR` on examine. With `DBG_SLEEP` set, the core clock stays alive in
    Sleep *while a debugger is attached*, which gives false-positive WFI / tick /
    current observations. This firmware does not bake in `DBG_SLEEP`, so when
    measuring over SWD confirm `DBGMCU->CR` (`0xE0042004`) bit 0 is 0 (clear it
    first if set). Do the final `tx_thread_sleep`-wake, UART-response, and current
    checks with SWD detached for certainty.

    Conversely, with `DBG_SLEEP=0` a genuinely WFI-sleeping core is *unreadable*
    on the old onboard ST-Link (V2J28M16): a plain `st-info --probe` / OpenOCD
    examine fails and memory reads return garbage (e.g. `0x01010001`). To attach,
    connect under reset (`openocd ... -c "reset_config srst_only connect_assert_srst"`,
    then `reset halt`/`reset run`) or set `DBG_SLEEP` after attaching. The
    unreadable-while-asleep symptom itself confirms the core really sleeps.

!!! note "Out of scope: `TX_LOW_POWER` (tickless)"
    The tickless framework that stops SysTick for deep sleep (`TX_LOW_POWER` +
    `tx_low_power.c`, requires user HW macros) is not used here (future, optional).
    This section covers only the shallow Sleep from the `TX_ENABLE_WFI` port macro.

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
