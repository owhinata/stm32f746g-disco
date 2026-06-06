# Shell test harness (dummy backend, host automated tests)

The layer that drives the Shell core **input -> execute -> output** with no
hardware (UART/VCP). It is an in-memory **dummy (loopback) backend** plus a
**host glue** that replaces `cli_core.c`'s ThreadX plumbing, so the whole suite
runs under host gcc. Clean-room design borrowing only Zephyr shell's *design*, no
code reused.

## Layering

| File | ThreadX | Role |
|---|---|---|
| `shell/backend/cli_backend_dummy.{c,h}` | independent | loopback transport (`init/enable/write/read`): RX FIFO / TX capture log / TX-capacity model |
| `shell/test/host_glue.{c,h}` | independent | `cli_core.c` stand-in: `cli_lock/unlock` + notify no-ops, faithful `cli_tx_send_blocking`, RX pump |
| `shell/test/test_core.c` | independent | #4 session unit (ASCII filter / state machine / dispatch / fail-safe) |
| `shell/test/test_output.c` | independent | #5 output unit (formatter / staging / colour / hexdump / TX failure) |
| `shell/test/test_integration.c` | independent | #6 end-to-end through the backend (input->execute->output / flow control / abnormal / multi-instance) |

The dummy backend is portable -- it drops straight into the on-target shell
library (#8). It calls no `tx_*` other than `cli_transport_notify_rx`, so the same
code builds on host and target.

## Dummy backend

`struct cli_dummy` (the transport's `ctx`) separates the **capture log** from the
**TX pending capacity**:

- **RX FIFO**: filled by `cli_dummy_inject()` (which, like a real ISR, only
  signals: `cli_transport_notify_rx`) and drained by `read()`. A burst past the
  FIFO is dropped and counted in both the dummy and `sh->rx_dropped` (req §9/§18 10e).
- **TX capture log**: bytes `write()` accepted, kept in order **permanently** (for
  verification; never auto-cleared).
- **TX pending capacity**: a separate counter. `write()` accepts only up to this
  much and returns `0` (full) when exhausted. It **never auto-recovers** -- only
  `cli_dummy_free_tx()` grows it (modelling a real "TX drains -> `cli_transport_notify_tx`").

| Helper | Purpose |
|---|---|
| `cli_dummy_inject(tr, data, len)` | inject input + RX notify |
| `cli_dummy_output(tr, &len)` / `_output_str(tr)` | read capture (length form / NUL-terminated) |
| `cli_dummy_set_tx_cap(tr, n)` | initial TX capacity (`0` = unlimited) |
| `cli_dummy_free_tx(tr, n)` | grant n bytes of TX space (notify_tx analogue) |
| `cli_dummy_set_tx_fail(tr, on)` | `write()` returns `-1` immediately (dead transport) |
| `cli_dummy_clear_output/clear_rx/reset_stats(tr)` | targeted resets |

## Data flow (end-to-end)

```
cli_dummy_inject ─▶ RX FIFO ─(notify_rx)─▶ cli_test_pump ─▶ read() ─▶ cli_input_byte
                                                                            │ dispatch
       capture log ◀─ write() ◀─ cli_tx_send_blocking ◀─ cli_out_flush ◀─ cli_print/echo
```

`cli_test_pump()` reproduces `cli_core.c`'s thread loop (on an RX signal, `read()`
then feed each byte to `cli_input_byte`) synchronously on the host. Output flows
from the real `cli_printf.c` through the host glue's `cli_tx_send_blocking`, which
**actually calls `tr->api->write()`** -- so the transport read/write contract and
flow control are exercised as a path, not stubbed.

## Host model of §11 flow control

The host is single-threaded, so a test cannot free space while
`cli_tx_send_blocking()` is on the stack. Instead, each time it observes "full"
(`write()==0`) the glue invokes an **`on_tx_wait` hook** (the host equivalent of
blocking for `cli_transport_notify_tx`), installed with
`cli_test_set_tx_wait_hook()`:

- **Normal backpressure**: the hook grants space via `cli_dummy_free_tx()` and the
  send completes in order (no drop).
- **Timeout drop**: no hook (NULL) -> no space arrives -> after a bounded retry
  count the remainder is dropped, `tx_dropped` is bumped and the call returns `<0`.

The glue's `cli_tx_send_blocking` mirrors `cli_core.c` (partial accept -> complete
/ full -> timeout drop / `write()<0` -> immediate fail / clamp on `n>remaining`).
`test_integration.c` pins the first three so the host model can't drift; the clamp
defensively mirrors `cli_core.c` and is not exercised (a well-behaved backend never
over-accepts).

## Coverage and non-goals

Covers the implemented surface (#2 registration / #3 parser / #4 core / #5 output)
across basic and abnormal cases.

| §18 | Item | Test |
|---|---|---|
| basic | dispatch / subcommands / argv / prompt resumes | test_core, test_integration |
| §13 | ASCII filter (high bytes dropped) / ESC-CSI swallow / CR-LF coalesce | test_core, test_integration |
| §11 | colour / staging / autoflush / hexdump / backpressure completes, timeout drop, immediate fail, non-zero promotion | test_output, test_integration |
| 10a-d,h | unknown command / bad args / line overflow BEL / Ctrl+C | test_core, test_integration |
| 10e | RX FIFO overflow drop + stat | test_integration |
| §10 | two dummy instances: no output crosstalk, independent state | test_core, test_integration |

**Non-goals (extended by later issues)**: line editing / cursor / history / Tab
completion (#9-#11) and built-in commands (#12-#14) are not implemented yet, so
out of scope here. The `cli_transport_notify_tx` contract ("backend signals when
TX space frees") is a host no-op and thus **not verified** here -- it is verified
on target by the UART backend (#7). True concurrent ThreadX scheduling is left to
the on-target demo (#8); here interleaved driving demonstrates "no output mixing".

## Running

```bash
sh shell/test/run_host_tests.sh   # => host tests passed
```

Each test uses `host_sections.ld` (the `.shell_root_cmds` boundary symbols) and
`shim/tx_api.h` (ThreadX type stubs), so it builds entirely under host gcc with no
firmware build. The dummy backend and host glue are shared by the #4-#6 builds.
