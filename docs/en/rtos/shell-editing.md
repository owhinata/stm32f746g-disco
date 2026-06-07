# Line editing / VT100 / meta keys / colour (#9)

The layer that grows the [Shell core](shell-core.md)'s `cli_input_byte()`
(`shell/core/cli_edit.c`) from append-only input into an **interactive line
editor**. The cursor is split from the line length so text can be inserted,
overwritten and deleted anywhere; it adds cursor motion, word operations, line
clear, **redraw with wrapping at the terminal width**, the VT100 escapes (arrows
/ Home / End / Del / Insert), Emacs-style meta keys and colour output (req §2).
Clean-room: informed by the *concept* of GNU readline / linenoise / the Zephyr
shell, reusing none of their code.

`cli_edit.c` calls no `tx_*`; output goes through the #5 buffered output API
(`cli_lock` / `cli_out_putc` / `cli_out_flush`), so it is
[unit-tested](shell-testing.md) on the host.

## Cursor model

`struct cli_instance` holds `cur` (cursor index, 0..len) split from `len`. Each
edit updates `(line, len, cur)` and then redraws. The same struct also holds
`overwrite` (insert/overwrite), `bs_swap` (backspace mode), `term_width`,
`old_rows` / `draw_row` (redraw invariants), `probing_cpr` (CPR pending) and
`esc_p[2]` / `esc_np` (CSI parameters).

## Key bindings

| Input | Action |
|---|---|
| printable `0x20–0x7E` | insert at the cursor (replace in overwrite mode) |
| `Ctrl+a` / `Ctrl+e` | start / end of line |
| `Ctrl+b` / `Ctrl+f`, `←` / `→` | cursor left / right |
| `Alt+b` / `Alt+f` (`ESC b` / `ESC f`) | move left / right by word |
| `Home` / `End` (`ESC[H` `ESC[F` `ESC[1~` `ESC[4~`) | start / end of line |
| `Backspace` (`0x08`) | delete the char before the cursor |
| `Del` (`0x7F`, default) / `Ctrl+d` / `Delete` (`ESC[3~`) | `0x7F` erases backward by default; `Ctrl+d` and `ESC[3~` delete forward (under the cursor) |
| `Ctrl+k` | kill to end of line |
| `Ctrl+u` | kill from start to cursor (whole line when at end) |
| `Ctrl+w` | delete the previous word (whitespace boundary) |
| `Ctrl+l` | clear screen + redraw prompt/line |
| `Insert` (`ESC[2~`) | toggle insert / overwrite |
| `Ctrl+c` | cancel the input line (recovers from a half-read escape too, §9) |
| `Enter` (`\r` / `\n`) | dispatch the line (`\r\n` once) |
| `↑` / `↓`, `Ctrl+p` / `Ctrl+n` | history (no-op stubs in #9; the fixed ring lands in #10) |

`Tab` (`0x09`) is ignored in #9 (completion is #11). Non-ASCII (`0x80–0xFF`) and
unsupported / malformed escapes are ignored, returning to the normal state
(req §13).

## Escape state machine

`enum cli_rx_state` is extended to `NORMAL / ESC / CSI / SS3`. After `ESC`
(`0x1B`) the next byte selects:

- `[` -> **CSI**: accumulate numeric parameters and `;`, finalise on a final byte
  (`0x40–0x7E`). `A/B/C/D` = ↑↓→←, `H`/`F` = Home/End, the `~` family is
  `1/7`=Home `2`=Insert `3`=Del `4/8`=End, and `R` = the CPR reply. Unknown
  finals are ignored (§13).
- `O` -> **SS3**: application-mode arrows such as `ESC O A` (no parameters).
- `b` / `f` -> Alt word move. Anything else is ignored.

## Redraw model (unified refresh + fast paths)

Edits normally repaint the whole line and reposition the cursor with a single
`cli_edit_refresh()`. It never trusts the terminal's last-column auto-wrap; it
fixes the physical cursor position itself.

With `cols=term_width`, `pend=prompt_len+len`, `pcur=prompt_len+cur`:

1. Erase the previous render (`old_rows` rows) bottom-up with `ESC[2K`.
2. Paint prompt + line. When `pend>0 && pend%cols==0` (exact width boundary)
   **force `\r\n`** so the cursor is unambiguously at the next row, column 0.
3. Reposition to `pcur`'s row/column with `ESC[<n>A` + `\r` + `ESC[<n>C`.
4. Update the invariants `old_rows = pend/cols + 1`, `draw_row = pcur/cols`.

Numbers are emitted digit-by-digit with a tiny fixed helper — no `snprintf`, no
large local arrays — to keep the instance-thread stack flat.

**Fast paths** (cheap for the common cases, byte-identical to #4):

- append at end of line (`cur==len`, still on the same physical row) -> echo the
  one byte;
- Backspace at end of line (`cur==len>0`, no row crossing) -> emit `\b \b`;
- cursor-only moves (arrows / Home / End / word) -> reposition without repainting.

Everything else (mid-line edits, wrap crossings) takes a full refresh.

## Terminal-width auto-detection (CPR)

Wrapping needs the terminal width. At start `cli_edit_session_start()` sends a
**CPR probe** (`ESC[999C` to jump far right, then `ESC[6n` to request the cursor
position) and **immediately follows it with a normal refresh**, so the visible
cursor is always restored whether or not a reply comes. The terminal answers
`ESC[<r>;<c>R`; the CSI parser consumes it and sets `term_width=c`.

The width is applied only when **`probing_cpr` is set, exactly two parameters
were seen, and `20<=c<=255`**. Any other `R` (no probe pending, wrong parameter
count, out of range, or pasted) is treated as an unknown final and ignored — it
never leaks into the line. A terminal that never answers keeps `CLI_TERM_WIDTH`
(default 80); the path is non-blocking and never stalls the shell. Mid-session
resizes are not tracked (reconnect to re-probe).

## insert / overwrite and backspace mode

- `Insert` toggles `overwrite`; in overwrite mode a printable replaces the char
  under the cursor. The mode persists across commands.
- `CLI_BACKSPACE_MODE` (default 0) seeds `bs_swap`. `0` = both `0x08` and `0x7F`
  erase backward; `1` = `0x7F` (DEL) deletes forward (for terminals whose
  Backspace key sends `0x08`). Flip at run time with `cli_set_backspace_mode()`.

## Colour

The `cli_error`/`warn`/`info` SGR colours (red/yellow/green) are from #5;
`CLI_USE_COLOR=0` makes them empty. The cursor / erase / CPR control escapes
(`cli_vt100.h`) are a **separate group that is always emitted** — they carry
editing semantics, not decoration, so they are not gated by `CLI_USE_COLOR`.

## Configuration

| Parameter | Default | Meaning |
|---|---|---|
| `CLI_TERM_WIDTH` | 80 | terminal width for wrapping before/without a CPR reply |
| `CLI_BACKSPACE_MODE` | 0 | `bs_swap` seed (0 = both erase backward / 1 = DEL deletes forward) |

## History seam (#10)

`↑`/`↓` and `Ctrl+p`/`Ctrl+n` call `cli_history_prev/next()`, and dispatch calls
`cli_history_add()`, but in #9 these are **no-op stubs** in
`shell/core/cli_history.c`. #10 only has to replace that file's body with the
fixed ring (§8: 512 B, FIFO, consecutive-duplicate suppression).

## Verification

Host unit test `shell/test/test_edit.c` (drives `cli_input_byte` directly and
asserts the model): cursor moves, insert/overwrite, deletes (BS, Ctrl+d, Del,
Ctrl+k/u/w), word moves, invalid-escape ignore, the CPR guard, wrap row counts,
clear screen, backspace mode and the post-dispatch cursor reset.

```bash
sh shell/test/run_host_tests.sh   # => host tests passed
```

On hardware, attach a terminal to the VCP (`/dev/ttyACM0`, 115200 8N1) and check
cursor motion, in-line editing, meta keys, wrap redraw, colour, and the response
times (operation < 50 ms, echo < 5 ms; req §15/§18). A concurrent `printf` from
another thread may garble the edited line on screen, but it corrupts no state and
`Ctrl+l` redraws cleanly.
