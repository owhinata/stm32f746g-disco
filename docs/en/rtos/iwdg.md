# Independent watchdog (`wdt` / IWDG)

Issue #38. Adds the **IWDG (independent watchdog)** so a hung or runaway firmware
**auto-resets and recovers** instead of staying dead until a manual power cycle. A
dedicated high-priority *petter* thread refreshes the watchdog, a short timeout
catches stalls quickly, and the fault handler keeps the board halted only while a
debugger is attached. Clean-room design; no third-party code reused.

Components:

- `include/iwdg.h` / `src/iwdg.c` — the IWDG driver (`iwdg_init` / `iwdg_refresh`)
  and the compile-time gate `BSP_ENABLE_IWDG`.
- `src/bsp.c` — enables the LSI in `SystemClock_Config()` and freezes the IWDG on
  debug halt (`__HAL_DBGMCU_FREEZE_IWDG()`) in `bsp_init()`.
- `src/main.c` — the priority-5 petter thread, and the IWDG *arm* at the end of
  `tx_application_define()`.
- `src/fault.c` — the fault halt loop now refreshes the IWDG only while a debugger
  owns the core.
- `src/log.c` — `log_reset_cause()`, used by `wdt info` (the `IWDG` decode itself
  already existed, RM0385 §5.3.21).
- `shell/cmds/cmd_wdt.c` — the `wdt` command (`info` always, `starve` dangerous).

## Why IWDG

The IWDG is clocked by the **LSI** (~32 kHz RC oscillator), fully independent of
the HSE + PLL 216 MHz tree (RM0385 §32). It keeps counting even if the main clock
or the ThreadX scheduler stops, so it catches failures a software timer cannot.

**Caught** by this design:

- tick / scheduler stall (no thread runs),
- an IRQ-off lockup (`__disable_irq()` + spin),
- a runaway thread at priority < 5 (the petter cannot be scheduled),
- an **unattended fault halt** (no debugger).

**Not caught** (by design): a logical infinite loop *inside* the shell thread
(priority 16). The priority-5 petter keeps refreshing, so the watchdog never
fires — `coremark` (~12 s, no yield) runs the same way and must not reset. Use
Ctrl+C / a thread-level liveness check for that class, not the IWDG.

## Timing

LSI is an RC oscillator with a wide tolerance (datasheet): **17 kHz min /
32 kHz typ / 47 kHz max**. The timeout is `T = prescaler × (RLR+1) / f_LSI`.

| Setting | Value |
|---|---|
| Prescaler | `/64` |
| Reload (RLR) | `1499` |
| Window | disabled |

| LSI corner | Timeout `T` |
|---|---|
| 32 kHz (typ) | **3.00 s** |
| 47 kHz (fast) | **2.04 s** ← worst-case minimum |
| 17 kHz (slow) | 5.65 s |

The petter sleeps `tx_thread_sleep(1000)` (≈1.0 s). Against the **2.04 s**
worst-case minimum that is the standard "refresh within `T/2`" margin (~2×). A
hang longer than ~1 s therefore trips a reset, which is the intent.

## Pet strategy

The petter (`iwdg_entry`, `src/main.c`) runs at **priority 5** — above led (10),
the shell (16) and the bg-job workers (17) — so it preempts CoreMark and any
lower-priority runaway and keeps petting with no extra refresh sprinkled through
the code. Only the System Timer thread (0) and ISRs sit above it. It pets once
immediately, then every ~1 s.

## LSI enable

`SystemClock_Config()` ORs `RCC_OSCILLATORTYPE_LSI` into the existing oscillator
set and sets `LSIState = RCC_LSI_ON`, leaving HSE/PLL untouched;
`HAL_RCC_OscConfig()` waits for `LSIRDY` (RM0385 §5.2). The LSI is independent of
the 216 MHz tree, so the clock bring-up is unaffected.

## DBGMCU freeze

`bsp_init()` calls `__HAL_DBGMCU_FREEZE_IWDG()` (sets
`DBGMCU_APB1_FZ.DBG_IWDG_STOP`, RM0385 §40.16.5) right after `HAL_Init()`, so the
IWDG **stops counting while the core is halted by the debugger**. Without it a
single SWD breakpoint would let the watchdog reset the board mid-session.

## Arm timing (late, on purpose)

The watchdog is armed last, at the **end of `tx_application_define()`**, in the
fixed order *petter create → `iwdg_init()` → `tx_glue_timer_enable()`*:

- All fail-soft bring-up before it — notably QSPI and SD HAL init, which can
  **block up to ~5 s** on a media fault — completes **before** the watchdog arms.
  A media fault then stays "qspi/sd disabled, shell keeps running" instead of
  becoming an IWDG reset loop.
- The petter already exists, and the scheduler starts immediately after, so the
  init→first-pet window is **sub-millisecond** — far below the 2.04 s minimum.
- `HAL_IWDG_Init()` polls the prescaler/reload update (SR PVU/RVU) with a
  `HAL_GetTick()` timeout. The SysTick ISR runs `HAL_IncTick()` from boot
  (only `_tx_timer_interrupt()` is gated until `tx_glue_timer_enable()`), so the
  tick advances here exactly as it does for the existing QSPI/SD init.

## Fault halt integration

`src/fault.c` funnels all halt paths through `fault_halt()`:

- **Debugger attached** (`CoreDebug->DHCSR & C_DEBUGEN`): the loop writes the
  reload key (`IWDG->KR = 0xAAAA`) so the board stays halted and SWD post-mortem
  works. The key is written directly (no HAL handle), so it is safe even with
  IRQs disabled and no valid driver state.
- **No debugger**: the loop does **not** pet, so the IWDG times out (~2–6 s) and
  resets the board — an unattended crash recovers on its own.

After the next reset, `dmesg` still shows the crash lines (the log survives in
DTCM, see [logging](logging.md)) followed by a fresh `reset cause: IWDG` boot.

## `wdt` command

```text
sh> wdt info
IWDG:       enabled
  timeout:  ~3.0s typ (2.0-5.7s over LSI tol), prescaler /64 RLR=1499
  pet:      ~1s by the priority-5 petter thread
  last reset: IWDG
sh> wdt starve            # dangerous: stop petting -> board resets
wdt: starving the IWDG (IRQ off, spin) -> reset in ~timeout
```

- `wdt info` is **always present**. With `BSP_ENABLE_IWDG=OFF` it prints
  `IWDG: disabled (build)`; if `HAL_IWDG_Init()` reported an error it prints
  `init failed (may be armed)` (HAL starts the IWDG before polling SR, so a
  failure does not prove it is unarmed). `last reset` comes from
  `log_reset_cause()`.
- `wdt starve` is a **dangerous** command (spec §12) and needs the watchdog built
  in, so it compiles only when `CLI_ENABLE_DANGEROUS_CMDS` **and**
  `BSP_ENABLE_IWDG` are both set. It drains the UART (≈50 ms, like `reboot`),
  `__disable_irq()`s to stop the petter, then spins — the IWDG resets the board.

## Compile-time gate `BSP_ENABLE_IWDG`

ON by default. Build `-DBSP_ENABLE_IWDG=OFF` to drop the IWDG entirely: no LSI
enable, no DBGMCU freeze, no petter thread, no IWDG arm, and `wdt info` reports
`disabled (build)`. The define lives on the **`bsp_iface` INTERFACE** target (not
the `threadx` executable) because `src/bsp.c` is compiled in the `common` object
library, which only sees `bsp_iface`'s definitions — so every translation unit
gets the same value. Verify with `nm threadx.elf | grep -i iwdg` (the
`iwdg_*`/`hiwdg` symbols vanish in the OFF build).

## Caveats

- **Bring-up failures are not auto-recovered.** The IWDG arms only after the full
  bring-up completes, so a failure in HSE/PLL/UART/QSPI/SD init halts before the
  watchdog is armed (`Error_Handler()` spins, SWD can attach). This is deliberate
  — a permanent clock fault would otherwise reset-loop forever.
- **`C_DEBUGEN` is a development heuristic**, not proof a debugger is physically
  connected now. Depending on the detach sequence, `C_DEBUGEN` may stay set and
  the fault halt keeps refreshing (no reset). For production, gate or document
  this separately.

## Verification

- **Build**: `cmake --build build` (default ON). For the OFF build,
  `cmake -B build-off -DBSP_ENABLE_IWDG=OFF ...` then
  `nm build-off/threadx.elf | grep -i iwdg` shows the IWDG symbols gone.
- **On target** (`/dev/ttyACM0`, 115200 8N1, `flash`):
    - normal operation does **not** reset: shell input, `coremark` (~12 s),
      `sleep`, `watch`.
    - `wdt info` shows `enabled`, the timeout and `last reset`.
    - a SWD breakpoint (`openocd`/`st-util` + `gdb-multiarch`) does **not** reset
      the board (DBGMCU freeze).
    - `wdt starve` resets the board; after reboot `dmesg` / `wdt info` show reset
      cause `IWDG`.
    - with no debugger, an `crash bus` fault halt resets via the IWDG; with a
      debugger attached it stays halted for inspection.
