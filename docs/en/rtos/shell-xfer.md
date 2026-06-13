# Serial file transfer (`xfer` / `camera send`)

Pull a file off the board — most usefully a **captured camera frame** — over the
**same VCP serial port as the shell** (USART1, `/dev/ttyACM0`, 115200 8N1), with
no microSD removal. The board sends with a clean-room **YMODEM** sender; the PC
receives with `lrzsz` (`rz`/`rb`).

Before this (#42) the only way out was `camera save sd <path>` then physically
removing the card. Now:

```text
sh> camera capture                     # snapshot QVGA RGB565 into SDRAM
sh> camera send                        # stream it to the PC over YMODEM
ymodem: sending 'frame.raw' (153600 bytes) over the VCP
ymodem: start the receiver now -- e.g. `rb` (lrzsz YMODEM); Ctrl+C aborts
ymodem: sent 153600 bytes OK
```

## Commands

| command | source | notes |
|---|---|---|
| `camera send [name]` | the captured frame in SDRAM (no FS) | default name `frame.raw`; `size` = 153600 (QVGA RGB565) |
| `xfer send <sd\|fs> <path>` | any file on microSD (`sd`) or QSPI FS (`fs`) | generic; the YMODEM block 0 carries the basename + exact size |

Both drive the same protocol core through `xfer_send_source()`
(`shell/cmds/cmd_xfer.c`); only the byte source differs (`camera_frame_read` vs
`fx_file_read`). The camera path keeps the same **generation check** as
`camera save`: a concurrent `capture` mid-send aborts the transfer rather than
mixing two frames.

## Receiving on the PC

`lrzsz` must be installed (`rb` = receive YMODEM batch). The interactive recipe
with **picocom**:

```sh
picocom -b 115200 /dev/ttyACM0 --receive-cmd "sh -c 'rb -y -vv'"
# in picocom: run `camera capture` then `camera send`
# when the board starts sending, press Ctrl-A Ctrl-R
# at the "*** file:" prompt just press Enter (the name is ignored)
# rb writes frame.raw into picocom's working directory
```

Two gotchas this recipe handles:

- **Use `rb`, not bare `rz`** — `rz` defaults to ZMODEM and never sends the `C`
  this YMODEM sender waits for. `rb` (or `rz --ymodem`) is the YMODEM receiver.
- **`sh -c '…'` swallows picocom's appended filename** — picocom appends whatever
  you type at its `*** file:` prompt to the receive command, but `rb` takes no
  filename for receive (the name comes from block 0), so a bare
  `--receive-cmd "rb …"` would fail with "garbage on commandline". Wrapping in
  `sh -c` parks the extra argument in `$0` where it is ignored.

The sender waits up to ~30 s for the receiver's initial `C`, so you have time to
start `rb` after issuing the command. Then convert the raw frame:

```sh
python3 scripts/rgb565_to_png.py frame.raw out.png
```

For the `camera capture test` colorbar, `out.png` shows the eight known flat
bands — an end-to-end check of byte order and block integrity.

## Protocol (YMODEM-CRC)

Clean-room sender in the freestanding **`svc/ymodem.c`** layer (no shell / HAL /
ThreadX / FileX — the transport and source are injected vtables, so it is
host-tested in `shell/test/test_ymodem.c`). It is **CRC-16/CCITT** only:

- the receiver starts the batch by sending `C`; the sender replies with **block 0**
  (SOH/128) carrying `"name\0" "decimal-size\0"`, zero-padded;
- **data blocks** are STX/1024 (or SOH/128 for a final block ≤ 128 B), the short
  final block padded with `0x1A`; the exact size in block 0 lets the receiver trim
  the padding (153600 is a multiple of 1024, so the camera frame has none);
- each block is `kind | seq | ~seq | data | crc_hi | crc_lo`, ACK/NAK per block,
  `seq` mod 256;
- end with **EOT** (re-sent if NAKed once) then a final all-zero block 0.

`lrzsz` chose YMODEM (over XMODEM-1K) because block 0 carries the filename and the
exact size — the generic `xfer send` of an arbitrary file needs both. lrzsz is GPL,
so only the **PC side** reuses it; the board sender is written from the protocol
description.

## Sharing the UART with the shell

A `camera send` / `xfer send` handler runs **in the shell thread**, so while it
runs the main loop, line editor and echo are not running — the handler owns the
UART. It claims the console for the whole transfer with the new core API
([shell core](shell-core.md)):

- `cli_console_claim()` holds the output lock for the duration (so a background
  job's output cannot interleave into the byte stream) and raises a global
  `cli_xfer_active`;
- `cli_read_byte()` is a **timed raw RX read** that, unlike `cli_cancel_requested()`
  / `cli_sleep()`, does **not** consume `0x03` — those drain the RX ring hunting
  for Ctrl+C and would eat YMODEM ACK/`C`/NAK bytes;
- `cli_xfer_active` also makes `cli_tx_send_blocking` stop draining RX on a full TX
  ring, and makes the `_write` retarget **drop** printf output (which would
  otherwise reach `g_uart_console` unlocked from a non-shell thread / ISR and
  corrupt the stream);
- `cli_rx_flush()` is called before (drop type-ahead) and after (drop a trailing
  tail byte) so the prompt resumes clean.

!!! note "No on-board live progress"
    Progress can't be printed during the transfer — the same UART carries the
    binary stream. Watch `rb -vv`'s progress on the PC instead. The board prints
    only a start line and a one-line result.

!!! note "Background-job output is dropped during a transfer"
    Holding the output lock means a background job's `printf` blocks and then drops
    on its wedge deadline ([jobs](shell-jobs.md), #25) rather than corrupting the
    stream. This is intended.

!!! note "Stall timeout (`CLI_TX_TIMEOUT`)"
    If the PC/VCP stops draining, the TX path drops after `CLI_TX_TIMEOUT`
    (default 1000 ticks ≈ 1 s) and the transfer fails with a transport error.

## Abort and watchdog

- **PC abort**: `rz`'s cancel (two `CAN`) returns `YM_ERR_CANCEL`; the board prints
  `ymodem: cancelled` and returns to a clean prompt.
- **Local abort**: Ctrl+C (`0x03`) during the transfer is mapped to a YMODEM abort,
  which emits the `CAN×5` teardown so `rz` also bails.
- **Watchdog**: a ~13 s frame transfer never starves the ~3 s IWDG ([watchdog](iwdg.md))
  — the petter is a separate priority-5 thread and the (priority-16) shell thread
  suspends on event flags while waiting, so the petter keeps running.

## License

`svc/ymodem.c` and the `xfer` / `camera send` glue are MIT (clean-room). `lrzsz`
(GPL) is used only as the PC-side receiver and is not vendored.
