# CoreMark command (`coremark`)

The EEMBC **CoreMark** benchmark runs as the shell's `coremark` command. It
replaces the former standalone bare-metal `coremark` image and is folded into the
firmware (`threadx`) — the first example of "apps launch from the shell".

```text
sh> coremark
Running CoreMark (auto-calibrated, ~12s)...
2K performance run parameters for coremark.
CoreMark Size    : 666
Total ticks      : 11800
Total time (secs): 11.800000
Iterations/Sec   : 932.203390
Iterations       : 11000
...
CoreMark 1.0 : 932.203390 / GCC13.3.1 ... / STATIC
```

The score is configuration-dependent but lands around **928–932 (≈ 4.3
CoreMark/MHz)** at 216 MHz with GCC 13.3 `-O3`. The trailing `/ STATIC` is the
memory method (below); the score itself does not depend on it.

## Build integration (`coremark_obj`)

Like `shell_obj`, the CoreMark sources are collected into an **OBJECT library
`coremark_obj`** and linked into the `threadx` exe (see [Shell app](shell-app.md)).

| item | value | why |
|---|---|---|
| sources | `lib/coremark/core_*.c` + `port/coremark/core_portme.c` | upstream CoreMark + the board port |
| optimisation | `-O3 -funroll-loops` | CoreMark's standard build for a canonical score (the `threadx` exe itself is `-O2`) |
| `MEM_METHOD=MEM_STATIC` | the data block (`TOTAL_DATA_SIZE=2000`) lives in static `.bss` | keep it off the shell thread stack and pull in no `malloc` |
| `main` → `coremark_main` | `-Dmain=coremark_main` on `core_main.c` only | avoid clashing with the firmware `main()` (`src/main.c`) |
| `-u _printf_float` | added to the `threadx` link | CoreMark's score line uses `%f` (float printf) |

`cmd_coremark.c` just declares and calls `coremark_main()`:

```c
int coremark_main(void);   /* core_main.c built with -Dmain=coremark_main */

static int cmd_coremark(struct cli_instance *sh, int argc, char **argv) {
    cli_info(sh, "Running CoreMark (auto-calibrated, ~12s)...\r\n");
    coremark_main();        /* prints the canonical report via printf -> VCP */
    return 0;
}
CLI_CMD_REGISTER(coremark, NULL, "run the EEMBC CoreMark benchmark (~12s)",
                 cmd_coremark, 1, 0);
```

## Run model

- **Foreground, synchronous**: `coremark_main()` runs inside the calling shell
  instance thread (priority 16). Auto-calibration takes **~12 s**, during which the
  prompt is blocked (no Ctrl+C cancel yet — planned in #16).
- **The LED keeps blinking**: the `led` heartbeat (priority 10) preempts CoreMark
  and keeps toggling LD1 every 250 ms throughout the run.
- **Stack**: with `MEM_STATIC` the large data lives in `.bss`, so the thread stack
  only carries `coremark_main`'s call frames plus `%f` formatting. As headroom the
  `threadx` target sets the shell instance stack to **4096 B**
  (`CLI_INSTANCE_STACK_SIZE=4096`, up from the 2048 default). After a run,
  [`thread`](shell-thread.md) shows the stack usage still has margin.
- **Timing**: `HAL_GetTick()` (1 ms). Under ThreadX the SysTick drives both
  `HAL_IncTick` and `_tx_timer_interrupt` ([ThreadX integration](threadx.md)), so
  it stays accurate.
- **Output**: CoreMark's `ee_printf` → `printf` → the UART backend's strong
  `_write` → the IRQ TX ring. It reaches the same USART1 as `cli_print`, but since
  the benchmark runs in this single foreground thread there is no concurrent
  output; the timed region (`start_time()`–`stop_time()`) does no I/O, so TX
  back-pressure does not perturb the score.

!!! note "Line endings (LF→CRLF)"
    CoreMark's report uses `\n` per spec, but the UART backend's `_write` translates
    a bare `\n` in printf output to `\r\n` ([UART backend](shell-backend-uart.md)), so
    it renders cleanly on a raw terminal — no `--imap lfcrlf` needed. The shell's own
    `cli_print` already emits `\r\n` and does not go through `_write`, so it is
    unaffected.

## Not cancellable (#16)

`coremark` **cannot** be interrupted with `Ctrl+c` while running. `coremark_main()`
is a single blocking call into the read-only submodule with no check-in point, and
it prints via `printf`/`_write` rather than the shell's `cli_tx_send_blocking`, so
neither the cooperative `cli_cancel_requested()` check nor the TX-blocked RX wake
applies. The run banner says `not interruptible`; a `Ctrl+c` just queues for the
next prompt.

## License

`lib/coremark` (EEMBC, Apache-2.0) and `port/coremark` (derived from the barebones
port) are reused as-is. The glue in `shell/cmds/cmd_coremark.c` is MIT (clean-room).
