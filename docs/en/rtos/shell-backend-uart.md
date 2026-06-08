# Shell UART(VCP) backend (USART1 IRQ-driven RX/TX)

The first real backend after the [dummy one](shell-testing.md): it implements the
`struct cli_transport_api` defined by the [core](shell-core.md) by driving the
STM32 HAL UART in **interrupt (IT) mode**. It enables on-target interaction over
the ST-Link Virtual COM Port (VCP, USART1, PA9=TX / PB7=RX, 115200 8N1).

It does **not** initialise the UART clock / GPIO / baud rate: it reuses the handle
the board already brought up (`src/bsp.c` `VCP_UART_Init` → `huart1`) and only
layers interrupt-driven RX/TX ring buffers on top. Clean-room design, no code
reused.

## transport_api mapping

| op | Implementation (`shell/backend/cli_backend_uart.c`) |
|---|---|
| `init` | reset rings/counters, cache `tr->sh`, set `g_uart_console` to self (`enabled=0`) |
| `enable` | NVIC-enable `USART1_IRQn` at priority 5 → `HAL_UART_Receive_IT(huart,&rx_byte,1)`. **Set `enabled=1` and return 0 only on HAL_OK**; on failure restore NVIC and return non-zero (the core drops that thread, §9) |
| `write` | enqueue what fits into the TX ring, return the accepted count (0..len; 0 = full), then `kick_tx()`. **Non-blocking** (the §11 blocking lives in the core) |
| `read` | non-blocking drain of the RX ring (0..cap) |
| `uninit` | `HAL_UART_Abort()` (blocking, returns READY) → `enabled=0` → NVIC-disable |

## Rings and concurrency

The ring itself (`shell/backend/cli_uart_ring.h`) is a **HAL/ThreadX-free,
lock-free helper** that is unit-tested on the host (see [testing](shell-testing.md));
the backend owns the concurrency.

- **RX**: producer = the USART1 ISR, consumer = the shell thread (`read()`) — an
  **SPSC** ring needing only `volatile` head/tail, no lock. On ring overflow the
  byte is dropped and `sh->rx_dropped` bumped (same §9/§15 behaviour as the dummy).
- **TX**: **two** producers (the shell thread `write()` and the printf retarget
  `_write`), consumer = the TxComplete ISR. The head advance and the in-flight
  start are made atomic with a short **PRIMASK critical section**.

`kick_tx()`: if no transfer is in flight, send the **contiguous run from tail to
the buffer end** via `HAL_UART_Transmit_IT`, and **commit `tx_in_flight=1` /
`tx_chunk=run` only on HAL_OK** (`tx_chunk` is `uint16_t`, matching HAL's `Size`).
On HAL_BUSY/ERROR the bytes stay queued and the next call retries; a wrapped
remainder is sent by TxComplete as the next chunk. `write()`, `_write` and
TxComplete all funnel through the one `kick_tx()`, so the HAL return is handled in
a single place.

## ISR / HAL callbacks

HAL callbacks carry only the handle, so they are resolved to the context through
`g_uart_console` (the single USART1 console); a mismatched `huart` is ignored.

| Function | Role |
|---|---|
| `USART1_IRQHandler` | NULL-guard `g_uart_console`/`huart`, then `HAL_UART_IRQHandler(huart)` |
| `HAL_UART_RxCpltCallback` | put `rx_byte` into the ring (full → drop+count) → re-arm 1-byte RX (failure → `rx_rearm_fail++`) → `cli_transport_notify_rx(sh)` |
| `HAL_UART_TxCpltCallback` | advance tail by `tx_chunk` → `tx_in_flight=0` → `kick_tx()` for the next chunk → `cli_transport_notify_tx(sh)` (space freed) |
| `HAL_UART_ErrorCallback` | ORE → `rx_overrun++`. If `RxState==READY` (ORE/RTO stopped RX) re-arm RX; FE/NE/PE leave RX running, so do not re-arm |

`USART1_IRQHandler` lives inside the backend module. The `threadx` target excludes
`src/stm32f7xx_it.c` (to avoid a PendSV clash), so keeping the ISR in the backend
means the vector is defined only in builds that link the shell — existing demos
are unaffected.

## Interrupt priority and ThreadX

This repo's ThreadX Cortex-M7 port guards critical sections with **PRIMASK**
(`TX_PORT_USE_BASEPRI` is not defined; `port/threadx/tx_glue.c`). An ISR that
calls `cli_transport_notify_rx/tx` (`tx_event_flags_set`) is therefore **safe at
any NVIC priority** — it can never preempt a ThreadX critical section. The USART1
priority **5** (above SysTick=14 / PendSV=15) is an **echo-latency / overrun
tuning** choice, not a ThreadX-safety constraint, and the backend's own PRIMASK
section nests safely inside ThreadX's `TX_DISABLE/RESTORE`.

## printf / `_write` coexistence (single TX owner)

`src/bsp.c`'s `_write` (blocking polling) is made **weak**, and this backend
supplies a **strong** `_write` that overrides it (only in builds linking the shell).

- Console enabled: route printf through the **same TX ring** as the shell
  (bounded spin when full; the TX ISR drains in the background), giving each USART
  a single TX owner.
- Before enable (early boot logs, `g_uart_console==NULL` / `!enabled`): poll
  `huart1` with `HAL_UART_Transmit`. No IT TX is armed yet, so there is no clash.
- **LF→CRLF**: once enabled, `_write` translates a bare `\n` in printf output to
  `\r\n` (no double CR if a `\r` already precedes it), so the `coremark` report's
  `\n`-terminated lines render cleanly without a terminal-side map (`--imap lfcrlf`).
  The shell's `cli_print` goes through `write()`, not `_write`, so it is unaffected.

### Routing printf to the calling terminal (#18)

`_write` resolves the **shell instance that owns the running thread** via
`cli_current_instance()` (the thread→instance registry in
`shell/core/cli_core.c`) and writes to that instance's UART TX ring. This makes
printf output — notably the `coremark` report (`ee_printf` == `printf`, which
runs synchronously in the calling shell thread) — **follow the terminal you are
typing on**. When there is no owning instance — in an **ISR** (detected via
IPSR≠0), **before the scheduler starts** (the boot banner), from a **non-shell
thread**, or for a **non-UART transport** — it returns `NULL` and `_write` falls
back to `g_uart_console` (the last backend to init), exactly as before. The
registry is populated by `cli_start()` **before** it creates the thread (so an
auto-started thread is never seen unregistered) and cleared on every
`cli_thread_entry` exit. Its size is `CLI_THREAD_MAP_MAX` (default =
`CLI_MAX_INSTANCES`).

!!! note "Single USART1 today"
    The backend's `uart_enable`/`USART1_IRQHandler`/HAL callbacks are still tied
    to `g_uart_console`, so the TxComplete ISR cannot advance another instance's
    ring — **no second UART instance is wired up yet** (adding one requires
    per-instance IRQ/callback dispatch and a duplicate-`huart` bind guard). This
    routing mechanism and the `_write` retarget are the groundwork for future
    multi-UART and background jobs (#25); on the single USART1 the behaviour is
    byte-for-byte unchanged.

!!! warning "printf line atomicity"
    `_write` does **not** take the per-instance TX mutex (`tx_lock`) — it must be
    callable from an ISR, before the kernel starts, and from any thread. So a
    printf and a cross-thread `cli_print` to the **same** instance may interleave
    at sub-line granularity (the TX ring itself stays intact under PRIMASK). This
    does not occur for `coremark` (printf and shell are the same thread, serial)
    or on a single instance.

## Known limitation

- **HW overrun (ORE)**: `HAL_UART_Receive_IT(1)` disables RXNEIE/EIE on each byte
  and calls the callback, leaving an RXNE gap until the callback re-arms. At
  115200 8N1 (10 bit/frame) that is **~86.8 µs/byte**. The priority-5 ISR re-arms
  well within that, but a burst with long interrupt-masked windows can still
  trigger ORE. The DoD's "no drops" is about the **software ring capacity**; HW ORE
  is counted via `ErrorCallback` as `rx_overrun`. If sustained high-rate reception
  becomes a requirement, switch to **reading RDR directly in the RXNE ISR** (no
  1-byte re-arm gap).

## Configuration (overridable alongside `shell/include/cli_config.h`)

| Macro | Default | Meaning |
|---|---|---|
| `CLI_UART_RX_BUFFER_SIZE` | 256 | RX ring depth (effective size-1 bytes) |
| `CLI_UART_TX_BUFFER_SIZE` | 512 | TX ring depth (effective size-1 bytes) |

## Usage (wired up in #8)

```c
CLI_BACKEND_UART_DEFINE(vcp_tr, &huart1);          /* transport + ctx */
CLI_INSTANCE_DEFINE(vcp_sh, &vcp_tr, "uart:~$ ");  /* instance + stack */
cli_init(&vcp_sh);   /* backend init → event flags → mutex */
cli_start(&vcp_sh);  /* tx_thread_create (auto-start) */
```

The library-ised shell app, the `flash` target (then named `flash-shell`)
and the dummy 2nd instance are issue #8.

## Verification

- **Host unit test**: `sh shell/test/run_host_tests.sh` asserts the ring helper's
  fill/drain/wrap/overflow/contig (no HAL/ThreadX needed).
- **On-target verify** (#7 uses temporary wiring, not committed): temporarily wire
  the backend into `app_threadx.c`, `flash`, then over
  `picocom -b 115200 /dev/ttyACM0` confirm the prompt, command echo, no drops
  within the ring on long / pasted lines, and clean boot-log output.
