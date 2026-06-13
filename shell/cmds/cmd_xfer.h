/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_xfer.h
 * @brief   Shared YMODEM-over-VCP send entry point (issue #50).
 *
 * cmd_xfer.c owns the console hand-over + YMODEM driving for an injected byte
 * source.  `xfer send <sd|fs> <path>` builds a FileX source; `camera send`
 * (cmd_camera.c) builds a RAM-frame source -- both call xfer_send_source() so
 * the console claim / RX flush / progress messaging / result mapping live in one
 * place.
 */
#ifndef CMD_XFER_H
#define CMD_XFER_H

#include "ymodem.h"

#ifdef __cplusplus
extern "C" {
#endif

struct cli_instance;

/**
 * Send @p src to the PC over the shell's VCP using YMODEM: claim the console
 * (hold the output lock, enter raw mode), flush RX, run ymodem_send(), then
 * restore the console and print a one-line result.  The caller owns @p src and
 * any underlying resource (open file / frame validity) for the duration.
 * Returns 0 on a completed transfer, 1 on any failure / cancel.
 */
int xfer_send_source(struct cli_instance *sh, const struct ym_source *src);

#ifdef __cplusplus
}
#endif

#endif /* CMD_XFER_H */
