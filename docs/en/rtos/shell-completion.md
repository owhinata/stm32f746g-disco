# Tab completion (#11)

When the [line editor](shell-editing.md)'s `cli_input_byte()` receives a `Tab`
(`0x09`) it delegates to `cli_tab_complete()` (`shell/core/cli_complete.c`),
which implements **Tab completion of command / subcommand names** (req §2 / §8 /
§18.4). Clean-room: informed by the *behaviour* of bash / readline / the Zephyr
shell, reusing none of their code.

`cli_complete.c` calls no `tx_*`; it scans the input line (`sh->line`) and the
[registered command tree](shell-registration.md) **read-only** (unlike
`cli_parse.c`, which tokenizes in place it never corrupts the line), allocates
nothing, and walks the tree linearly (req §8). Output goes through the #5
buffered API, so it is [unit-tested](shell-testing.md) on the host.

## Completion algorithm

1. **Word being completed** — scan left from `cur` over non-space chars to the
   word start `ws`; `prefix = line[ws..cur)`. The completion text is inserted at
   `cur` and the tail shifted right, so it works mid-line as well as at EOL.
2. **Which command set** — walk the tokens in `line[0..ws)` **read-only**
   (length-bounded `memcmp`; never write `'\0'` into the line). The set is keyed
   on the **count of preceding tokens**, not on `ws==0`:
   - zero preceding tokens (empty line, or leading-spaces-only like `"  h"`) →
     the **root set**.
   - the first token must exactly match a root; descend into its `subcmds` if any.
     A leaf (`subcmds==NULL`) or an unknown token means we are in argument
     territory → **no candidates**.
3. **Candidate scan (one pass, no storage)** — count prefix matches over the set,
   remember the first, and compute the **longest common prefix length** (beyond
   `prefix`) by shrinking against `first`.
4. **Outcome** (req §18.4):

| Candidates | Action |
|---|---|
| 0 | BEL (`0x07`); line unchanged |
| 1 | complete the remainder; add one separator space when at end of line (not mid-line) |
| ≥2 | **bash-style two-stage**: extend to the common prefix first; when nothing more can extend, the **next consecutive Tab** lists the candidates |

## Two-stage (common prefix → list)

For multiple candidates a 1-byte `tab_list_armed` in `struct cli_instance` drives
the two stages:

- LCP extends → complete and set `tab_list_armed=1` (next Tab lists).
- nothing to extend → if `tab_list_armed==0`, BEL and arm only; if `==1`, **list**.
- `tab_list_armed` is reset to 0 at the top of `cli_input_byte()` on **any non-Tab
  byte** (only `0x09` preserves it), so editing between Tabs restarts the flow.

Example: `ve`+Tab → completes to `ver` (armed); the next Tab lists `version` and
`verbose`. At the common prefix the first Tab does not list; the **second
consecutive Tab** does.

## Candidate-list display

The list honours the [line editor](shell-editing.md)'s redraw invariants
(`old_rows` / `draw_row`). Because `cli_edit.c`'s static output helpers are not
reachable from another TU, the list is emitted under a **single** lock with the
internal staging primitives (`cli_lock` / `cli_out_putc` / `cli_out_flush`) and a
local `c_csi_n` helper (so it cannot interleave with another thread's output to
the same instance; the follow-up `cli_edit_redraw()` takes the lock again — a seam
acceptable under the shell-thread-driven model, §10):

1. Move to the bottom row of the current render (`ESC[<n>B`) then `\r\n`, so the
   list lands **below** the whole (possibly wrapped) input — like
   `cli_dispatch_line()`. The input line stays visible above it.
2. Print the matching names in **width-aware columns** (`cli_printf` has no
   `%-*s`, so spaces are padded manually): `per_row = cols/colw` per line, always
   ending on a fresh column-0 row with `\r\n`. **No allocation**; the number shown
   depends on terminal width (no fixed cap, req §8).
3. Set `old_rows=0; draw_row=0` and call `cli_edit_redraw()` (the
   `op_clear_screen` tail). `sh->cur` is untouched, so the caret is restored.

## Known limitations

- **Arguments are not completable** — `struct cli_cmd` carries no per-argument
  metadata, so completing past the command path is a BEL.
- **Quoted command paths** — the read-only token walk does not re-apply
  quote/backslash processing, so a word like `"help"` parses on submit but is not
  completable. Command names are bare C identifiers, so normal use is unaffected.
- ASCII only (req §13). Multi-instance safe: `tab_list_armed` is per-instance and
  the tree scan is read-only shared (req §10).

## Verification

Host unit test `shell/test/test_complete.c` (feeds Tab through `cli_input_byte`
and asserts the model): root unique complete / root ambiguous → LCP extend /
two-stage list (+ a non-Tab key resets the arm) / subcommand complete / no-match
BEL / argument-territory BEL / empty-line two-stage list / mid-line insert (no
double space) / leading-spaces root completion. A small-buffer build
(`CLI_CMD_BUFFER_SIZE=8`) checks the buffer-full BEL and that a **too-long LCP
extend still reaches the list on the next Tab**.

```bash
sh shell/test/run_host_tests.sh   # => host tests passed
```

On hardware, attach a terminal to the VCP (`/dev/ttyACM0`, 115200 8N1) and check
`h`+Tab → `help `, an empty line + Tab twice → an `echo`/`help` list, and the
operation response < 50 ms (req §15 / §18.4).
