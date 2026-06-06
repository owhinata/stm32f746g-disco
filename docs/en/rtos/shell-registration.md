# Shell command registration

Foundation of the ThreadX Shell (interactive CLI, an Epic). Commands are
**registered declaratively** from any source file and enumerated at startup by
walking a linker section. It is a clean-room implementation that borrows only
the *design* of Zephyr's shell — none of its code.

The public header is `shell/include/cli.h`; the configuration knobs live in
`shell/include/cli_config.h`. New shell files are MIT
(`SPDX-License-Identifier: MIT` / `Copyright (c) 2026 ThreadX Shell Project`).

!!! note "Scope of this page"
    Only command **registration** is covered here. The parser, core, output API,
    line editing and built-in commands come in later tasks. The full
    architecture overview is documented separately.

## Linker section `.shell_root_cmds`

Each root command is emitted as a `const struct cli_cmd` into the dedicated
section `.shell_root_cmds` and the table is **walked as an array** between the
boundary symbols `__cli_root_cmds_start` / `__cli_root_cmds_end`. It is added
just after `.rodata` in `ldscript/STM32F746NGHx_FLASH.ld` (a read-only table, so
it lives in FLASH).

```ld
.shell_root_cmds (READONLY) :
{
  . = ALIGN(4);
  PROVIDE_HIDDEN (__cli_root_cmds_start = .);
  KEEP (*(SORT_BY_NAME(.shell_root_cmds.*)))
  KEEP (*(.shell_root_cmds))
  PROVIDE_HIDDEN (__cli_root_cmds_end = .);
  . = ALIGN(4);
} >FLASH
```

- `KEEP` — retains entries through `-Wl,--gc-sections` (on by default in this
  repo); each entry is also tagged `used`.
- `SORT_BY_NAME` — makes the registration order deterministic (alphabetical),
  removing any dependence on link order.
- `aligned(__alignof__(struct cli_cmd))` — pins each entry's section alignment
  to the type's natural alignment so the linker inserts no padding between
  entries (which is what lets the array be walked by pointer arithmetic).
- The shared ldscript puts `.shell_root_cmds` into every app, but with no
  command registered it is an **empty section (start == end)** and harmless.

## Registration macros

```c
#include "cli.h"

static int cmd_version(struct cli_instance *sh, int argc, char **argv) { /* ... */ return 0; }

/* Subcommand tree: thread { list, stacks } */
CLI_SUBCMD_SET_CREATE(sub_thread,
    CLI_CMD_ARG(list,   NULL, "list threads",     cmd_thread_list,   1, 0),
    CLI_CMD_ARG(stacks, NULL, "show stack usage", cmd_thread_stacks, 1, 0),
    CLI_SUBCMD_SET_END
);

/* Root commands */
CLI_CMD_REGISTER(version, NULL,       "show firmware version", cmd_version, 1, 0);
CLI_CMD_REGISTER(thread,  sub_thread, "thread operations",     NULL,        2, 0);
```

| Macro | Purpose |
|---|---|
| `CLI_CMD_REGISTER(name, subcmds, help, handler, mandatory, optional)` | register a root command into `.shell_root_cmds` |
| `CLI_SUBCMD_SET_CREATE(set_name, ...)` | build a subcommand array (terminate with `CLI_SUBCMD_SET_END`) |
| `CLI_CMD_ARG(name, subcmds, help, handler, mandatory, optional)` | a set entry with explicit argument counts |
| `CLI_CMD(name, subcmds, help, handler)` | a set entry with defaults (`mandatory=1, optional=0`) |
| `CLI_SUBCMD_SET_END` | sentinel terminator for a subcommand array (`.name = NULL`) |

`name` is passed as a **bare C identifier** (stringified into `.name`, and used
to build the section suffix `.shell_root_cmds.<name>` and a unique symbol).
Hence dashed names like `foo-bar` cannot be registered this way. Every
standard-tier command (`version`/`uptime`/`reboot`/`thread`/`devmem`/`help`/
`clear`/`history`/`backends`) is a valid identifier, so this is not a limitation
in practice.

### Command descriptor `struct cli_cmd`

Root commands and subcommands share one type.

| Field | Meaning |
|---|---|
| `name` | command name (`NULL` marks a subcommand-tree sentinel) |
| `help` | one-line help |
| `subcmds` | sentinel-terminated subcommand array, or `NULL` |
| `handler` | handler, or `NULL` for a pure parent command |
| `mandatory` | required argc (command name included) |
| `optional` | number of optional arguments allowed |

The handler type is `int (*)(struct cli_instance *sh, int argc, char **argv)`;
`argv[0]` is the command name, return 0 for success and non-zero for an error.
If argc/argv validation (mandatory/optional) fails the handler is not called
(the validation itself is implemented in the parser).

## Iteration

```c
extern const struct cli_cmd __cli_root_cmds_start[];
extern const struct cli_cmd __cli_root_cmds_end[];

CLI_ROOT_CMD_FOREACH(c) {        /* c is const struct cli_cmd * */
    /* c->name, c->help, c->subcmds, ... */
}
size_t n = cli_root_cmd_count();
```

## Configuration knobs `cli_config.h` (§8)

Each knob is wrapped in `#ifndef` and can be overridden at build time (e.g.
`-DCLI_CMD_BUFFER_SIZE=512`). This foundation only *defines* the knobs plus
sanity `_Static_assert`s; the over-limit runtime behaviour of each is
implemented by later tasks.

| Knob | Default | Over-limit behaviour |
|---|---|---|
| `CLI_CMD_BUFFER_SIZE` | 256 B | ring the bell (BEL) and ignore further input |
| `CLI_MAX_ARGC` | 20 | show an error, do not execute |
| `CLI_HISTORY_BUFFER_SIZE` | 512 B | discard oldest entry first (FIFO) |
| `CLI_PRINTF_BUFFER_SIZE` | 32 B | flush when full |
| `CLI_PROMPT_BUFFER_SIZE` | 20 B | truncate |
| `CLI_INSTANCE_STACK_SIZE` | 2048 B | — |
| `CLI_MAX_INSTANCES` | 4 | fixed at compile time; exceeding is a build error |
| `CLI_MAX_SUBCMD_DEPTH` | 8 | show an error, do not execute |

The number of registered commands is bounded only by the linker section
capacity (effectively unlimited; the scan is linear). All storage is static —
no heap allocation at run time.

## Verification (host test)

`shell/test/run_host_tests.sh` is a minimal smoke test built and run with the
host gcc alone. It uses `host_sections.ld` (`INSERT AFTER .rodata`) to supply
`.shell_root_cmds` to the host linker, registers commands, walks the section,
and asserts the **count, gap-free layout, SORT order, and subcommand-tree
sentinel termination**.

```bash
bash shell/test/run_host_tests.sh   # => host smoke test passed
```
