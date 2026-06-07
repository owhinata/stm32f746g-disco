# Shell core (instance / ThreadX / state machine / dispatch)

The core that ties the [registration foundation](shell-registration.md) and the
[parser](shell-parser.md) together at run time. One transport = one **shell
instance**: on its own ThreadX thread it assembles received bytes into a line,
calls the parser on Enter, and runs the handler. Clean-room design borrowing
only Zephyr shell's *design*, no code reused.

## Layering (the ThreadX-free seam)

| File | ThreadX | Role |
|---|---|---|
| `shell/include/cli.h` | independent | public command-registration API (`struct cli_instance` forward-declared only) |
| `shell/include/cli_instance.h` | depends (`tx_api.h`) | `struct cli_instance` / transport types / `CLI_INSTANCE_DEFINE` / lifecycle API |
| `shell/core/cli_core.c` | depends | `cli_init` / `cli_start` / thread loop / ISR notify (the only core file calling `tx_*`) |
| `shell/core/cli_edit.c` | **calls none** | ASCII filter / RX + escape state machine / line editing / redraw ([line editing](shell-editing.md), #9) |
| `shell/core/cli_session.c` | **calls none** | line dispatch (parser call / error mapping / prompt return) |
| `shell/core/cli_history.c` | **calls none** | history hooks (no-op stubs in #9; the fixed ring lands in #10) |

Because `cli_edit.c` / `cli_session.c` never call a `tx_*` function, they compile
on the host against a type shim (`shell/test/shim/tx_api.h`), so the **state
machine, line editing and dispatch are unit-tested without hardware**. `cli.h`
stays ThreadX-independent, so command source files and the host parser test
build with no ThreadX headers.

## Transport abstraction

The core never touches hardware directly; it talks to the backend through
`struct cli_transport_api`.

```c
struct cli_transport_api {
    int  (*init)(struct cli_transport *tr);                          /* required */
    int  (*enable)(struct cli_transport *tr);                        /* required: start RX */
    int  (*write)(struct cli_transport *tr, const uint8_t *d, size_t n);/* required: non-blocking, returns accepted 0..n (finalised in #5) */
    int  (*read)(struct cli_transport *tr, uint8_t *d, size_t cap);  /* required: non-blocking, 0..cap */
    void (*uninit)(struct cli_transport *tr);                        /* optional (NULL ok) */
    void (*update)(struct cli_transport *tr);                        /* optional (NULL ok) */
};
```

- Backend-to-core wake-up is `cli_transport_notify_rx(sh)` (ISR-safe: it only
  sets an event flag, taking no lock and no suspend). `cli_transport_notify_tx`
  is reserved for #5.
- The [dummy backend](shell-testing.md) (#6) and the [USART1 VCP backend](shell-backend-uart.md) (#7) implement this.

## ThreadX primitives (per instance)

| Abstraction | ThreadX | Use |
|---|---|---|
| instance thread | `tx_thread` (`CLI_INSTANCE_PRIORITY`, default 16) | RX handling + dispatch |
| RX / TX / KILL signals | `tx_event_flags` (`CLI_EVT_RX/TX/KILL`) | wake-up from ISR/backend |
| TX mutual exclusion | `tx_mutex` (`TX_INHERIT`) | created only in #4; locked output is #5 |

Thread loop: wake on `tx_event_flags_get(RX|KILL)` -> drain with `read()` without
dropping bytes -> feed each byte to the state machine (`cli_input_byte`) -> return
to the prompt. `KILL` exits (full stop/uninit is future work). At start the thread
calls `cli_edit_session_start()` instead of a bare `cli_prompt`, so it fires the
terminal-width probe (CPR) and draws the first prompt.

## RX state machine and line editing

`cli_input_byte()` (`cli_edit.c`) processes one byte at a time. The ASCII filter
(non-ASCII `0x80–0xFF` dropped, §13), the `\r`/`\n` dispatch (`prev_cr` makes
`\r\n` dispatch **exactly once**), and Ctrl+C line cancel are the baseline.
Printable `0x20–0x7E` input plus cursor motion, in-line insert/delete, meta keys,
VT100 escapes, terminal-width wrapping and colour are the **#9 line editor** — see
[Line editing / VT100 / meta keys / colour](shell-editing.md).

## Dispatch and error mapping

`cli_dispatch_line()` maps the parser's `enum cli_parse_status` to a message.

| Status | Behaviour |
|---|---|
| `OK` | run the handler with `pr.argv` / `pr.argc` (leaf-relative, `argv[0]` = leaf name); store the return in `last_result` |
| `EMPTY` | do nothing (blank line) |
| `NOT_FOUND` | `<cmd>: command not found` |
| `NO_HANDLER` | `<cmd>: missing or unknown subcommand` |
| `WRONG_ARGS` | `<cmd>: invalid number of arguments` |
| `TOO_MANY_TOKENS` / `NESTING_TOO_DEEP` / `UNTERMINATED_QUOTE` | the respective error text |

A parse error (everything except `OK` and `EMPTY`) sets
`last_result = CLI_DISPATCH_ERR (-1)`; `EMPTY` leaves it unchanged. **Every path
returns to the prompt**, and neither a bad command nor a non-zero handler return
stops the shell (fail-safe, req §9).

## Multi-instance state separation

The line buffer, history, terminal state, parser scratch and prompt all live
inside `struct cli_instance`; the **core keeps no mutable global state**. The
command tree is read-only data in the linker section. Several instances
therefore run at once without their output or state mixing (req §10).

```c
/* e.g. wire to a backend and start it (#6/#8) */
CLI_INSTANCE_DEFINE(vcp_shell, &vcp_transport, "uart:~$ ");
cli_init(&vcp_shell);    /* backend init -> event flags -> mutex */
cli_start(&vcp_shell);   /* tx_thread_create (auto-start) */
```

`cli_init` runs backend init -> `tx_event_flags_create` -> `tx_mutex_create`. A
failure disables only that instance and leaves the rest running; the ThreadX
objects are not deleted on an init-time failure, because `tx_*_delete` is not
callable during system initialisation.

## Scope (#4) and what follows

#4 is the skeleton. The output API / buffering / colour (#5), the dummy backend
and end-to-end auto tests (#6), the USART1 VCP backend (#7) and the shell app +
`flash-shell` (#8) are done. [Line editing / VT100 / meta keys / colour](shell-editing.md)
(#9) is implemented too; history (#10) and Tab completion (#11) come next.

## Verification (host unit test)

`shell/test/test_core.c` is compiled together with `cli_session.c`, `cli_edit.c`,
`cli_history.c` and `cli_parse.c` (with `shim/tx_api.h` first on the include path
and small `CLI_CMD_BUFFER_SIZE`/`CLI_MAX_ARGC` overrides to exercise the
full-buffer and too-many-tokens paths). Output and the `tx_*` glue go through the shared
[dummy backend + host glue](shell-testing.md) (bytes are injected at the session
level via `cli_input_byte`). The test asserts the ASCII filter, the state
machine, CR/LF/CR-LF coalescing, the dispatch mapping, fail-safe, and
**non-mixing output of two instances**.

```bash
sh shell/test/run_host_tests.sh   # => host tests passed
```
