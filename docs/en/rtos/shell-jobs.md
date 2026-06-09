# Background execution and job control (`&` / `jobs` / `kill`)

Issue #25. A trailing `&` on a command line runs the command in the **background**,
with minimal job control via `jobs` / `kill %N`. This is distinct from #23's `;`
(synchronous, syntactic segment sequencing): `&` requires **concurrent** execution.
It builds on #18 (route printf to the calling terminal) and #16 (cooperative cancel).

## Usage

```
sh> sleep 30 &        # prints "[1] sleep 30" and the prompt returns immediately
sh> jobs              # [1] Running    sleep 30
sh> kill %1           # [1] kill requested  -> next Enter prints [1] Done sleep 30
sh> coremark &        # heavy commands run in the background too (prompt stays live)
sh> sleep 20 & ; sleep 20 &   # two jobs run concurrently under distinct ids
```

- **`cmd &`**: a lone trailing `&` (outside quotes, not backslash-escaped) runs the
  command in a worker thread and the dispatcher returns to the prompt at once. The
  `&` in `echo "a&"` / `echo a\&` is a literal. `&&` (logical-and) is not supported
  and does not background. Only a segment-trailing `&` backgrounds (no mid-line `&`).
- **`jobs`**: list running jobs as `[id] Running cmd`; finished jobs are reaped and
  announced here.
- **`kill %N`**: request a **cooperative** stop of job id `N` (a leading `%` is optional).

## Execution model

One job = a dedicated `struct cli_instance` worker thread from a fixed pool
(`CLI_MAX_BG_JOBS`, default 2). The handler signature is identical to the interactive
shell, so **every existing command runs in the background unchanged**. Each worker:

- **Output**: shares the launching foreground (fg) instance's `tx_lock`, and aliases
  its `tr` to `fg->tr`. A background `cli_print` *and* `printf` (`_write`) both
  serialise through the fg output lock and reach the fg terminal (#18/#25).
- **Cancellation**: never touches the fg RX ring (a strict single-consumer SPSC pipe).
  `kill %N` latches the worker's `cancel_req` and wakes it via the worker's own event
  group. Cancellation is cooperative: commands that poll `cli_cancel_requested()` or
  use `cli_sleep()` (`sleep`/`watch`, ...) stop promptly, but a **non-cooperative one
  (`coremark`/`usleep`) ignores it and runs to completion** (same limit as #16).
- **Priority**: `CLI_BG_JOB_PRIORITY = CLI_INSTANCE_PRIORITY + 1` (default 17), one
  step below the fg (16), so even a CPU-bound `coremark &` is always **preempted by
  interactive input** and the prompt stays responsive.
- **Lifecycle**: a worker returns after its handler runs -> `TX_COMPLETED`. A thread
  cannot delete itself, so the **foreground thread reaps lazily** (`cli_jobs_reap`:
  read `tx_thread_state`, `tx_thread_delete` only COMPLETED workers, print `[id] Done
  cmd` from fg context, free the slot). Reaping runs on every Enter and in `jobs`/`kill`.
  **Job ids are monotonic**, and `kill %N` only acts on `{matching id AND running}`,
  so a reused slot can never be killed by a stale `kill %N` for an older job.

## How output looks (known limitations)

When a background job emits output, it breaks to a fresh line under the fg `tx_lock`,
prints as a **block**, and marks the fg input-line render dirty. On the next keystroke
the existing line editor repaints the prompt + in-progress line **below** that output.

- The input line is not erased first, so a half-typed line stays above the bg output
  and is redrawn below it (**double echo**). The TX ring is byte-safe, so output is
  never **corrupted**; the display may just be briefly untidy (fixed on the next key).
  This is the intended trade-off of the line-block policy (erase/redraw integration is
  a non-goal).
- The completion notice `[id] Done` is printed by the **fg thread at Enter time**, not
  by the worker, so it serialises naturally with the prompt.

## Configuration (`cli_config.h`)

| Knob | Default | Meaning |
|---|---|---|
| `CLI_MAX_BG_JOBS` | 2 | concurrent bg jobs (worker pool); over the limit a launch is rejected |
| `CLI_BG_JOB_STACK_SIZE` | =`CLI_INSTANCE_STACK_SIZE` (exe: 4096) | worker stack; large enough for `coremark &` |
| `CLI_BG_JOB_PRIORITY` | `CLI_INSTANCE_PRIORITY+1` (17) | worker priority (below fg so it cannot starve the prompt) |
| `CLI_BG_TX_POLL_TICKS` | 2 | TX-full kill-wait slice (overall deadline is `CLI_TX_TIMEOUT`) |
| `CLI_THREAD_MAP_MAX` | `CLI_MAX_INSTANCES+CLI_MAX_BG_JOBS` | thread->instance registry (printf routing) |

## Non-goals

Pipes `|`, `&&`/`||`, redirection, multiple `&` per line, a full `fg`/`bg`/`wait`,
`Ctrl+Z` suspend, and erase/redraw integration.

## See also

- Output routing / printf terminal following: #18 ([output API](shell-output.md) / [UART backend](shell-backend-uart.md))
- Cooperative cancel `cli_cancel_requested` / `cli_sleep`: #16 ([line editing](shell-editing.md))
- `;` sequencing and quote-aware `&` detection: #23 ([parser](shell-parser.md))
- Backgrounding long-running commands: `watch ... &` ([delay/watch](shell-sleep-watch.md))
