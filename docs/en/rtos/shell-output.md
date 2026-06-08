# Shell output API (buffering, colour, flow control)

The output layer on top of the [shell core](shell-core.md). Handlers print through
it; each call **formats -> 32 B staging -> autoflush -> sends to its own
transport** under the TX lock and flow control. Clean-room design borrowing only
Zephyr shell's *design*, no code reused.

## Layering (the ThreadX-free seam)

| File | ThreadX | Role |
|---|---|---|
| `shell/include/cli.h` | independent | public output API prototypes |
| `shell/core/cli_printf.c` | **calls none** | minimal vprintf -> char sink / 32 B staging + autoflush / colour / hexdump |
| `shell/core/cli_vt100.h` | independent | VT100 SGR colours + `CLI_USE_COLOR` gate |
| `shell/core/cli_core.c` | depends | `cli_lock`/`cli_unlock` (TX mutex) + `cli_tx_send_blocking` (flow control) |

`cli_printf.c` calls no `tx_*`; it reaches ThreadX only through three hooks
(`cli_lock`/`cli_unlock`/`cli_tx_send_blocking`), so the formatter and staging are
host-unit-testable (same approach as [#4](shell-core.md)).

## Public API

```c
int cli_write (struct cli_instance *sh, const void *data, size_t len);   /* raw bytes (echo etc.) */
int cli_print (struct cli_instance *sh, const char *fmt, ...);           /* default colour */
int cli_error (struct cli_instance *sh, const char *fmt, ...);           /* red */
int cli_warn  (struct cli_instance *sh, const char *fmt, ...);           /* yellow */
int cli_info  (struct cli_instance *sh, const char *fmt, ...);           /* green */
int cli_hexdump(struct cli_instance *sh, const void *data, size_t len);  /* canonical hex+ASCII */
```

- Each call is bracketed by `cli_lock` -> format/stage -> autoflush -> `cli_unlock`,
  so `out_buf`/`out_len` are mutually excluded across the whole format (req §10).
  The `format(printf,2,3)` attribute catches misuse.
- Return: **0 = fully sent, `<0` = output failure (a TX timeout dropped bytes, or the output lock could not be acquired)**.
- **Thread-context only, not ISR-safe** (`tx_mutex_get`/`tx_event_flags_get` waits
  are illegal from an ISR). The backend's TX notify from an ISR is only
  `cli_transport_notify_tx` (an event-flag set).
- Colour: `cli_error`=red / `cli_warn`=yellow / `cli_info`=green (req §2);
  `CLI_USE_COLOR=0` emits no SGR.

## Minimal formatter (honours §8)

To satisfy §8's "32 B printf buffer, flush when full" with **no extra buffer**, the
formatter streams one character at a time into staging (`cli_out_putc`, flushing
with `cli_out_flush` when full). It is an original implementation -- no newlib or
Zephyr printf code is reused.

- Supported: `%% %c %s %d %i %u %x %X %p`, length modifiers `l` / `ll` / `z`, field
  width with `0` / `-` flags.
- Not supported: precision, `+` / space / `#` flags. `INT_MIN`/`LLONG_MIN` convert
  safely via an unsigned magnitude.
- `cli_hexdump`: 16 bytes/line of `08x` offset + hex + ASCII (non-printable as `.`).

## Flow control / TX timeout (req §11)

`cli_tx_send_blocking` (cli_core.c, run while the lock is held) unifies §11:

1. The transport `write` is **non-blocking** and returns the accepted byte count
   (`0..len`). A backend that returns `n<len` (TX full) is obliged to fire
   `cli_transport_notify_tx` once space frees again.
2. When TX is full the core waits on `CLI_EVT_TX | CLI_EVT_KILL` via
   `tx_event_flags_get` (this is §11's "blocking send" = a thread suspend), bounded
   by `CLI_TX_TIMEOUT` (ticks, `0` = forever).
3. On timeout it drops the rest, bumps `tx_dropped`, sets `tx_failed`, and the
   output API returns `<0`. A KILL is re-posted and aborts the wait (so even an
   infinite timeout stays killable).
4. **Forced non-zero command exit**: `cli_dispatch_line` sets
   `last_result = (ret==0 && tx_failed) ? nonzero : ret`, so a dropped output fails
   the command even if the handler ignored `cli_print`'s return.

`tx_failed` is reset per command (start of dispatch); once set, the rest of that
command's output is dropped.

Cooperative-cancel interaction (#16): while a command runs (`dispatching`),
`cli_tx_send_blocking` also waits on `CLI_EVT_RX`, so a `Ctrl+c` (`0x03`) arriving
while the send is blocked on TX flow control is caught by the `cli_cancel_poll`
calls around the wait and returns `<0` early (a large-output command stops at
once). See "Cooperative cancellation" in [command registration](shell-registration.md).

## Configuration

| Knob | Default | Meaning |
|---|---|---|
| `CLI_PRINTF_BUFFER_SIZE` | 32 | staging size (flush when full) |
| `CLI_TX_TIMEOUT` | 1000 | TX-space wait cap (ticks, `0` = forever); ~1 s at a 1 kHz tick |
| `CLI_TX_MUTEX_WAIT` | 0 | output-lock acquire wait (ticks, `0` = forever) |
| `CLI_USE_COLOR` | 1 | 0 emits no colour |

## Verification (host unit test)

`shell/test/test_output.c` is compiled with `cli_printf.c` (`shim/tx_api.h`;
output goes through the shared [dummy backend + host glue](shell-testing.md),
actually calling `tr->api->write()`). It asserts the formatter (boundaries:
`INT_MIN`/`LLONG_MIN`/`%p`/`NULL %s`/unknown spec/width+flags), the autoflush
across >32 B, colour (SGR), hexdump, and immediate TX-failure `<0`/`tx_failed`/no
output. Flow control (backpressure completes, timeout drop, `tx_dropped`, clamp)
is path-verified via the host glue by the [#6 integration test](shell-testing.md);
`cli_core.c`'s ThreadX wait/KILL stays covered by the ARM smoke and review.

```bash
sh shell/test/run_host_tests.sh   # => host tests passed
```
