# Thread Info Command (`thread`)

Issue #13 of M4 "built-in commands". After `version`/`uptime`/`reboot`
([system built-ins](shell-builtins.md)), this adds `thread`, a read-only diagnostic
command that shows every ThreadX thread in **one combined table**. It lives in
`shell/cmds/cmd_thread.c` and, like `cmd_system.c`, is linked into the **executable
only** (it never touches the host test harness command set). Use it for stack-size
tuning and deadlock investigation.

## Command

| Command | Registration | Behaviour |
|---|---|---|
| `thread` | `CLI_CMD_REGISTER(thread, NULL, ..., cmd_thread, 1, 0)` | Print every thread's state / priority / run count and stack usage in one table |

There are no subcommands (a single leaf). `thread` runs on its own; `thread <arg>`
is a usage error (too many args: mandatory=1 / optional=0). The handler touches only
the `sh` passed to it through the buffered output API, so it stays reentrant across
instances (req §10).

```text
sh> thread
name                 state  pri   runs   size  peak  use%   cpu%
cli                  susp    16     14   2048   612   29%   0.0%
led                  sleep   10   1240   1024   312   30%   0.1%
System Timer Thread  susp     0      1   1024   180   17%   0.0%
(idle)                                                      99.5%
(isr)                                                       0.4%
```

| Column | Meaning | Source |
|---|---|---|
| name | thread name | `tx_thread_name` |
| state | execution state (table below) | `tx_thread_state` |
| pri | current priority (0–31) | `tx_thread_priority` |
| runs | times dispatched onto the CPU (cumulative) | `tx_thread_run_count` |
| size | total stack (bytes) | `tx_thread_stack_size` |
| peak | **high-water** stack usage (bytes) | 0xEF scan (below) |
| use% | `peak / size` as a percentage | same |
| cpu% | CPU share over the window since the previous `thread` (top-style) | Execution Profile Kit (below, issue #19) |

The trailing `(idle)` / `(isr)` are pseudo-rows: idle headroom and ISR processing, so
the cpu% column sums to ~100%. The `free` column (`size − peak`) was dropped as
redundant, but is still computed internally to derive peak (below).

On the real shell build you will see `cli` (the VCP shell instance,
prio=`CLI_INSTANCE_PRIORITY`), `led` (prio 10) and `System Timer Thread` (ThreadX's
software-timer thread). The last one exists because the shell build does not define
`TX_TIMER_PROCESS_IN_ISR`, so it is created and enumerated.

## CPU usage (cpu%)

The `cpu%` column is each thread's share of CPU time over the window **since the
previous `thread` run** (top-style). Together with the trailing `(idle)` (idle
headroom) and `(isr)` (interrupt processing) pseudo-rows, the column sums to ~100%.

### Mechanism: ThreadX Execution Profile Kit + TIM2

- `port/threadx/tx_user.h` defines `TX_EXECUTION_PROFILE_ENABLE` / `TX_CORTEX_M_EPK`,
  enabling the bundled Execution Profile Kit
  (`lib/threadx/utility/execution_profile_kit/`). It accumulates each thread's busy
  time (64-bit) on every context switch — the port asm hooks are wired automatically.
- **Time source is `TIM2->CNT`** (APB1, 32-bit, TIM2CLK = 2×PCLK1 = 108 MHz; started in
  `src/bsp.c` `exec_timebase_init()`). The kit default DWT_CYCCNT freezes when the core
  clock is gated during WFI sleep, whereas TIM2 keeps counting in Sleep (TIM2LPEN reset
  = 1). So idle/cpu% stay correct even once WFI power saving
  ([#20](https://github.com/owhinata/stm32f746g-disco/issues/20)) is enabled.
- **ISR accounting**: the real ISRs (`SysTick_Handler` / `USART1_IRQHandler`) are plain
  HAL C handlers that do not go through ThreadX's context save/restore asm, so the kit's
  `_tx_execution_isr_enter/exit` are **explicitly wrapped around both handlers**.
  `TX_CORTEX_M_EPK` is required (this port's `TX_THREAD_GET_SYSTEM_STATE()` ORs in the
  IPSR, so the non-EPK guard would drop all ISR time). The enter/exit calls are
  PRIMASK-protected so the kit's bookkeeping (nest counter / 64-bit RMW) survives nesting
  (USART1 prio 5 preempting SysTick prio 14).
- Measurement is armed at the top of the first application thread `led`
  (`tx_glue_profile_enable()`) — a point guaranteed to be after
  `_tx_execution_initialize()`, avoiding reads before TIM2 is live or calls before the
  kit is initialised.

### cpu% computation (differential window)

`cmd_thread.c` keeps a static snapshot of each thread's cumulative time plus the global
thread/isr/idle totals from the previous `thread`, and computes the difference:

```text
cpu%(thread) = Δthread / window ,   window = Δ(all threads) + Δisr + Δidle
```

- The **first** run has no prior, so every row shows `--`; later runs are non-blocking
  (the window is the gap between two `thread` invocations).
- 64-bit values are non-atomic, so each per-thread total is read under a brief
  `TX_DISABLE`, and the global totals are snapshotted in the same critical section as
  head/count. Δ<0 clamps to 0, >100.0% clamps to 100.0%, window=0 prints `--`.
- This is distinct from `runs` (dispatch count); cpu% is the real CPU-time share.

## Stack usage (0xEF high-water scan)

At `tx_thread_create()` ThreadX fills the whole stack with `TX_STACK_FILL`
(`0xEFEFEFEF`) **unless** `TX_DISABLE_STACK_FILLING` is defined
(`lib/threadx/common/src/tx_thread_create.c`). The shell target does not define it
(only the `thread_metric` benchmark does), so every stack is `0xEF`-initialised at
creation.

Cortex-M stacks **grow downward**. ThreadX represents a stack as:

- `tx_thread_stack_start` = buffer base = **lowest** address
- `tx_thread_stack_end` = `start + size − 1` = **highest** address

The stack grows from `end` (high) toward `start` (low), so the untouched tail keeps
its `0xEF` fill at the low end. Therefore **free = the leading run of `0xEF` bytes
from `stack_start`** and **peak = `size − free`**. This is the same approach as
ThreadX's own `tx_thread_stack_analyze()`, but it runs only when `thread` is invoked
(zero steady-state overhead). `TX_ENABLE_STACK_CHECKING` is not required.

!!! note "High-water, not instantaneous / best-effort"
    `peak` is the **maximum-ever** usage, not the value at command time (a running
    thread's live SP is the value saved at the last context switch and may be stale,
    so it is not used). Also, if a used boundary word happens to equal `0xEF` the
    figure can read a few bytes low (the same best-effort limitation as ThreadX's
    analyze). The command relies on the default stack fill, so **do not build with
    `TX_DISABLE_STACK_FILLING`**.

## Thread enumeration

ThreadX's created list (a **circular doubly-linked list**) is walked via the head
`_tx_thread_created_ptr` and count `_tx_thread_created_count` (declared `extern`),
following `tx_thread_created_next` **count times** (the list is circular, not
NULL-terminated). The internal `tx_thread.h` is not included — only the two globals
are declared locally; every field read lives in the public `TX_THREAD` typedef.

This firmware creates every thread once in `tx_application_define()` and never deletes
one, so the created list is static. The head+count are snapshotted briefly under
`TX_DISABLE`/`TX_RESTORE`, then the list is walked with interrupts on (`cli_print`
waits on a mutex and must not run inside a critical section).

## State name mapping

`tx_thread_state` (`tx_api.h`, 0..14) is mapped to a short label:

| Value | define | Label | Value | define | Label |
|---|---|---|---|---|---|
| 0 | TX_READY | `ready` | 8 | TX_BLOCK_MEMORY | `block` |
| 1 | TX_COMPLETED | `compl` | 9 | TX_BYTE_MEMORY | `byte` |
| 2 | TX_TERMINATED | `term` | 10 | TX_IO_DRIVER | `io` |
| 3 | TX_SUSPENDED | `susp` | 11 | TX_FILE | `file` |
| 4 | TX_SLEEP | `sleep` | 12 | TX_TCP_IP | `tcpip` |
| 5 | TX_QUEUE_SUSP | `queue` | 13 | TX_MUTEX_SUSP | `mutex` |
| 6 | TX_SEMAPHORE_SUSP | `sem` | 14 | TX_PRIORITY_CHANGE | `pchg` |
| 7 | TX_EVENT_FLAG | `event` | — | out of range | `?` |

## Verification

- **Build**: `cmake --build build` (the `shell` target links; existing demo intact).
- **On hardware** (`/dev/ttyACM0` @115200 8N1):
  - `help` lists `thread`.
  - `thread` shows `cli` / `led` / `System Timer Thread` with state/pri/runs/size/peak/use%/cpu%.
    A running thread's `runs` increases over time; `peak < size`; `led` matches `size=1024`.
  - Run `thread` twice: the first cpu% is `--`, the second shows realistic values over the
    differential window. When idle, `(idle)` dominates (~99%), thread cpu% is small, and the
    cpu% column (threads + `(idle)` + `(isr)`) sums to ~100%.
  - Tab completion: `thr` → `thread`. `thread x` is a usage/arg error.
