# Shell command-line parser

The core-internal parser that interprets one input line, layered on top of the
[command registration foundation](shell-registration.md). It does (1) tokenizing
(quotes/escapes), (2) static subcommand-tree search, and (3) argc/argv
validation (including RAW).

The parser is a **pure function**: it does not depend on a `struct cli_instance`
and never invokes a handler. It resolves the command plus the handler-relative
argc/argv and reports a status; the actual dispatch and message printing belong
to the core. Internal API in `shell/core/cli_internal.h`, implementation in
`shell/core/cli_parse.c`. Clean-room design borrowing only Zephyr shell's
*design* (no code reused).

## Tokenizer rules (ASCII, in-place destructive split)

The line buffer is rewritten in place and each `argv[i]` points into it as a
NUL-terminated string. Quotes/escapes are compacted within each token's span, so
the still-unread tail stays pristine (RAW relies on this).

| Input | Rule |
|---|---|
| space / tab | token separators; leading/trailing ignored, runs collapse |
| `"…"` (double) | literal until closed (spaces included); inside, `\` escapes the next char |
| `'…'` (single) | fully literal (`\` is literal; only `'` closes) |
| `\c` (unquoted) | next char literal (`\ ` does not split) |
| empty quote `""` `''` | kept as a single empty-string argument |
| trailing `\` (nothing after) | literal `\` (not an error) |
| unterminated quote | **error** (`UNTERMINATED_QUOTE`), command not run |

- Escapes are "make the next char literal" only (`\x##`/`\0###` hex/octal are
  not supported — a future extension).
- More than `CLI_MAX_ARGC` (=20) tokens yields **`TOO_MANY_TOKENS`** (§8; no
  silent truncation).

## Command separator `;` (sequential execution)

A single input line is split on `;` into multiple commands run **left to right**
(e.g. `thread ; coremark ; thread`). The split is done before dispatch by
`cli_next_segment()` (`cli_parse.c`), which scans with the **same quote/escape
state machine** as the tokenizer (each segment is kept verbatim — only the
separating `;` is overwritten with `NUL`).

- **Quotes respected**: a `;` inside quotes or escaped is not a separator
  (`echo "a;b"` / `echo 'a;b'` / `echo a\;b` stay one command). A remainder with
  an unterminated quote stays one segment and `cli_parse` reports
  `UNTERMINATED_QUOTE` (same as for a single line).
- **Continue on error**: a not-found / arg error / non-zero handler in one
  segment does not stop the rest (bash `;` semantics).
- **Ctrl+C aborts**: if a running command observes the cooperative cancel (#16),
  the remaining segments do **not** run (one `^C`).
- **Empty segments**: leading/trailing/doubled `;` (`;a`, `a;`, `a;;b`) whose
  segment is blank are skipped (an `EMPTY` no-op).
- **One history entry per line**: the whole `;` line is recorded as one entry; ↑
  recalls it and re-runs the sequence after a re-parse.
- **Prompt once at end of line**; each command's output is identical to running
  it alone (no extra newline between segments).
- **Scope**: only sequential `;`. `&&` / `||` / pipe `|` / redirection are not
  supported (future work). Tab-completion of the word after `;` is also
  unsupported (the completer treats `;` as an ordinary char). For RAW commands a
  space before `;` stays in the preceding segment (consistent with the verbatim
  tail contract).

The segment loop itself lives in `cli_session.c`: `cli_dispatch_line()` (scans
`;`) → `cli_dispatch_one()` (parses + runs one command).

## Subcommand-tree search

- Token 0 is matched against root commands (`.shell_root_cmds`). Duplicate root
  names resolve to the first match in SORT order (no detection).
- Subsequent tokens greedily descend the current node's `subcmds`
  (sentinel-terminated array) while they match. Descent stops on a non-matching
  token, a node without `subcmds`, or the depth limit.
- **Depth limit** `CLI_MAX_SUBCMD_DEPTH` (=8) counts subcommand steps below root:
  root plus up to 8 levels; beyond that is `NESTING_TOO_DEEP`.
- **The parent path is stripped**: the handler's `argv[0]` is the leaf command
  name (for `thread list`, the handler sees `argv[0]="list"`).
- Stopping at a pure parent (`subcmds` set, `handler==NULL`), or passing an
  unknown subcommand to a parent that has no handler, yields `NO_HANDLER`. If the
  parent has a handler, the non-matching token becomes its argument.

## argc/argv validation and RAW

- Accept condition: `mandatory <= handler_argc <= mandatory + optional`
  (`mandatory` includes the leaf name; `mandatory=1` means no arguments).
  Out of range is `WRONG_ARGS`.
- **RAW** (`optional == CLI_ARG_RAW`, value `0xFF`; for leaf commands): the upper
  bound is dropped; after the `mandatory` tokens are tokenized normally, the rest
  of the line is passed as one verbatim raw string in the handler's
  `argv[mandatory]` (no quote/escape processing, leading whitespace trimmed). An
  empty remainder adds no extra argument.

## Return statuses

| Status | Meaning | `out` contents |
|---|---|---|
| `OK` | handler may run | cmd / handler argv·argc / cmd_level |
| `EMPTY` | blank line | zeroed |
| `NOT_FOUND` | unknown root | `argv[0]`=unknown token, argc=1 |
| `NO_HANDLER` | pure parent (no handler) | cmd / handler argv·argc |
| `WRONG_ARGS` | arg count out of range | cmd / handler argv·argc |
| `TOO_MANY_TOKENS` | exceeded `CLI_MAX_ARGC` | zeroed |
| `NESTING_TOO_DEEP` | exceeded `CLI_MAX_SUBCMD_DEPTH` | zeroed |
| `UNTERMINATED_QUOTE` | quote left open | zeroed |

`out` is zero-initialized up front and `argc` is the length contract; a NULL
sentinel is also written at `argv[argc]` whenever a command is resolved (the
caller's argv array has capacity `CLI_ARGV_CAP = CLI_MAX_ARGC + 2`).

## Verification (host unit test)

`shell/test/test_parse.c` is compiled together with `shell/core/cli_parse.c`
(with small `CLI_MAX_ARGC` / `CLI_MAX_SUBCMD_DEPTH` overrides so the limit paths
can be exercised with a compact tree). It asserts splitting
(quotes/escapes/empty quote/trailing `\`/unterminated/NULL sentinel), search
(multi-level/parent-path stripping/pure parent/non-match-becomes-arg/depth),
validation (in/out of range/too many), RAW (handler-relative index for a
multi-level leaf/verbatim tail/empty tail/too few), and `;` splitting
(`cli_next_segment`: no split inside quotes/escapes, empty/leading/trailing
segments, unterminated quote stays one segment, escape boundaries). The
end-to-end `;` sequencing (continue-on-error, Ctrl+C abort, one prompt at the end
of the line) is in `shell/test/test_integration.c`.

```bash
bash shell/test/run_host_tests.sh   # => host tests passed
```
