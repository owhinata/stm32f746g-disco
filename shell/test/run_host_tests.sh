#!/usr/bin/env sh
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
#
# Host smoke/unit tests for the Shell core, built and run with the host gcc.
# Each test links shell/test/host_sections.ld, which supplies the
# .shell_root_cmds section + boundary symbols that the target ldscript provides
# on hardware.  No firmware build is involved.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
inc="$here/../include"
core="$here/../core"
backend="$here/../backend"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

# Flags mirror the target link so the tests exercise the real retention path:
#   -ffunction-sections -fdata-sections + -Wl,--gc-sections : same GC as the
#       firmware; proves `used` + linker KEEP keep the (otherwise unreferenced)
#       command entries from being garbage-collected.
#   -no-pie : resolve the const command/pointer table absolutely at link time so
#       it stays read-only without runtime text relocations (target firmware is
#       linked absolute/static, so this only matters on the host).
CFLAGS="-std=c11 -Wall -Wextra -ffunction-sections -fdata-sections -no-pie"
LDFLAGS="-Wl,--gc-sections -Wl,-T,$here/host_sections.ld"

# #2 -- command registration foundation.
gcc $CFLAGS -I "$inc" \
    "$here/test_registration.c" \
    $LDFLAGS -o "$out/test_registration"
"$out/test_registration"

# #3 -- command-line parser.  cli_parse.c and the test share one compile so the
# small CLI_MAX_ARGC / CLI_MAX_SUBCMD_DEPTH overrides (used to exercise the
# token-limit and nesting-limit paths with a compact tree) apply consistently.
gcc $CFLAGS -DCLI_MAX_ARGC=8 -DCLI_MAX_SUBCMD_DEPTH=2 \
    -I "$inc" -I "$core" \
    "$here/test_parse.c" "$core/cli_parse.c" \
    $LDFLAGS -o "$out/test_parse"
"$out/test_parse"

# Shared host pieces for the core / output / integration tests (#4-#6): the
# dummy (loopback) backend and the ThreadX-free glue (no-op lock/notify, a
# faithful cli_tx_send_blocking over tr->api->write, and the RX pump).  Found via
# the test dir (-I "$here", host_glue.h) and the backend dir (cli_backend_dummy.h).
glue="$backend/cli_backend_dummy.c $here/host_glue.c"
glue_inc="-I $here -I $backend"

# #4 -- shell core: ASCII filter, RX state machine, dispatch, fail-safe.
# cli_session.c is ThreadX-free (the tx_* glue lives in cli_core.c, firmware
# only), so it builds on the host against the tx_api.h shim in test/shim, placed
# first on the include path.  Output + tx_* glue now route through the shared
# dummy backend.  Compiled with cli_parse.c and small CLI_* limits so the
# buffer-full (CLI_CMD_BUFFER_SIZE) and too-many-tokens (CLI_MAX_ARGC) paths fit
# a compact input line.
gcc $CFLAGS -DCLI_CMD_BUFFER_SIZE=16 -DCLI_MAX_ARGC=4 -DCLI_MAX_SUBCMD_DEPTH=2 \
    -DCLI_USE_COLOR=0 \
    $glue_inc -I "$here/shim" -I "$inc" -I "$core" \
    "$here/test_core.c" "$core/cli_session.c" "$core/cli_printf.c" "$core/cli_parse.c" \
    $glue \
    $LDFLAGS -o "$out/test_core"
"$out/test_core"

# #5 -- output API: minimal formatter, 32 B staging + autoflush, VT100 colour,
# hexdump, TX-failure drop/return.  cli_printf.c is ThreadX-free (the tx_* flow
# control lives in cli_core.c), so it builds against the shim and routes staged
# output through the shared dummy backend + glue.  Colour ON (default) and the
# real 32 B CLI_PRINTF_BUFFER_SIZE so the SGR escapes and autoflush are exercised.
gcc $CFLAGS \
    $glue_inc -I "$here/shim" -I "$inc" -I "$core" \
    "$here/test_output.c" "$core/cli_printf.c" \
    $glue \
    $LDFLAGS -o "$out/test_output"
"$out/test_output"

# #6 -- dummy backend end-to-end: input -> execute -> output driven THROUGH the
# transport (cli_dummy_inject -> read() -> state machine -> write() -> capture),
# §11 flow control (backpressure completes / timeout drops / immediate fail),
# §9/§18 abnormal cases and §10 multi-instance isolation.  Small CLI_* limits +
# colour OFF as for #4 so overflow / too-many-tokens fit and output compares plain.
gcc $CFLAGS -DCLI_CMD_BUFFER_SIZE=16 -DCLI_MAX_ARGC=4 -DCLI_MAX_SUBCMD_DEPTH=2 \
    -DCLI_USE_COLOR=0 \
    $glue_inc -I "$here/shim" -I "$inc" -I "$core" \
    "$here/test_integration.c" "$core/cli_session.c" "$core/cli_printf.c" "$core/cli_parse.c" \
    $glue \
    $LDFLAGS -o "$out/test_integration"
"$out/test_integration"

# #7 -- UART backend byte ring: the pure, lock-free FIFO helpers (cli_uart_ring.h)
# that the USART1 IRQ backend layers RX/TX on.  HAL/ThreadX-free, so it builds with
# the host gcc and needs no shim -- only the backend include dir for the header.
gcc $CFLAGS -I "$backend" \
    "$here/test_uart_ring.c" \
    $LDFLAGS -o "$out/test_uart_ring"
"$out/test_uart_ring"

echo "host tests passed"
