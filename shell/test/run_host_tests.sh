#!/usr/bin/env sh
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
#
# Host smoke test for the Shell command-registration foundation (issue #2).
# Builds test_registration.c with the host gcc, linking the augmenting script
# that supplies the .shell_root_cmds section + boundary symbols, then runs it.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

# Flags chosen to mirror the target link so the test actually exercises the
# retention path the feature relies on:
#   -ffunction-sections -fdata-sections + -Wl,--gc-sections  : same GC as the
#       firmware build; proves `used` + linker KEEP keep the (otherwise
#       unreferenced) command entries from being garbage-collected.
#   -no-pie : resolve the const command/pointer table absolutely at link time so
#       it stays read-only without runtime text relocations (the target firmware
#       is linked absolute/static, so this only matters on the host).
gcc -std=c11 -Wall -Wextra \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -no-pie \
    -I "$here/../include" \
    "$here/test_registration.c" \
    -Wl,-T,"$here/host_sections.ld" \
    -o "$out/test_registration"

"$out/test_registration"
echo "host smoke test passed"
