# Delay & watch commands (`sleep` / `usleep` / `watch`)

Issue #21. Two delays (`sleep`/`usleep`) and a periodic monitor (`watch`). `sleep`
and `watch` ride on #16's cooperative cancellation (`cli_sleep` /
`cli_cancel_requested`), so they are **interruptible with Ctrl+C**.

## `sleep N` — sleep N seconds (cancellable)

```
sh> sleep 3        # blocks ~3 s; Ctrl+C prints ^C and returns at once
```

- Integer seconds (MVP). Internally `cli_sleep(sh, N*1000)` (1 tick = 1 ms).
- `cli_sleep` waits on the instance event flags, so an incoming `0x03` wakes it,
  the call is cancelled, and the dispatcher prints `^C` and returns to the prompt
  (see "Cooperative cancellation" in [command registration](shell-registration.md)).
- Capped by `CLI_SLEEP_MAX_SEC` (default 86400 = 1 day). Out-of-range / non-numeric
  input is an error (non-zero exit).

## `usleep N` — busy-wait N microseconds (**not interruptible**)

```
sh> usleep 500     # waits ~500 us
```

- Busy-waits on the free-running **TIM2** (108 MHz, started in #19) via
  `bsp_udelay()` — 108 counts == 1 us.
- It **holds the CPU and does not yield, so it cannot be interrupted**. Interrupts
  (SysTick / UART) still run, so the ThreadX tick, the LED and higher-priority
  threads are unaffected, but the shell thread blocks for the duration.
- Capped by `CLI_USLEEP_MAX_US` (default 10000 = 10 ms). Use `sleep` for long
  delays. Out-of-range / invalid input is an error.

## `watch [-n SEC] CMD...` — re-run CMD periodically

```
sh> watch -n 1 thread     # re-run `thread` every 1 s (screen cleared). Ctrl+C stops
sh> watch thread          # default 2 s interval
```

- Each refresh clears the screen (`\x1b[2J\x1b[H`), prints a header, runs CMD,
  then waits `-n SEC` (default `CLI_WATCH_DEFAULT_SEC` = 2, max `CLI_WATCH_MAX_SEC`).
  Ctrl+C stops it and returns to the prompt. Great as a `top`-style view of the
  `thread` cpu% column.
- **Re-dispatch**: CMD (the RAW tail) is **copied to a local buffer, parsed with
  `cli_parse`, and the resolved handler is called directly** — the instance's line
  buffer / parser scratch are never touched. Subcommands resolve too
  (`watch thread list`).
- **Cancellation**: a Ctrl+C is observed by the inner command's own
  `cli_cancel_requested()` poll, by the interval `cli_sleep`, or by the explicit
  check at the top of the loop (`cancel_req` is sticky until the outer dispatch ends).
- **Recursion / danger guard**: the parser-normalised root token is checked, so
  `watch` (unbounded stack recursion), `coremark` (~12 s, not interruptible, prints
  via printf and fights the screen clear) and `reboot` (would reset on the first
  iteration) are rejected — quote/escape evasions like `watch "reboot"` are caught
  too.
- A CMD that fails to parse (unknown command, etc.) errors once and does not loop.

!!! warning "watch runs arbitrary commands"
    Anything not on the denylist can be repeated. `devmem peek`/`dump` and `thread`
    are safe to watch, but **repeating a side-effecting command such as
    `devmem poke` is the user's responsibility**.

## Configuration (`cli_config.h`)

| Knob | Default | Meaning |
|---|---|---|
| `CLI_SLEEP_MAX_SEC` | 86400 | `sleep` max seconds |
| `CLI_USLEEP_MAX_US` | 10000 | `usleep` max microseconds (busy-wait) |
| `CLI_WATCH_DEFAULT_SEC` | 2 | `watch` default interval |
| `CLI_WATCH_MAX_SEC` | 3600 | `watch -n` max seconds |

## Related
- Cooperative cancellation: [command registration](shell-registration.md) / [line editing](shell-editing.md)
- `cli_sleep` / `cli_cancel_requested`: #16. TIM2 time source: #19 ([ThreadX integration](threadx.md)).
