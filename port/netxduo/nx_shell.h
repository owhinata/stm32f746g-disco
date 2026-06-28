/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nx_shell.h
 * @brief   TCP network shell (telnet) over NetX Duo (issue #49 P4).
 *
 * Bridges the clean-room CLI shell's transport abstraction to a NetX TCP server
 * socket: a single network shell session on port 23 (telnet/nc), bound to one
 * cli_instance.  This file owns the socket lifecycle + the cli_transport vtable
 * implementation (it needs both nx_api.h and the shell headers, so it lives in
 * the NetX glue layer rather than shell/backend, keeping the shell library free
 * of any NetX dependency).
 *
 * Wiring: src/main.c defines a cli_instance bound to @ref nx_shell_transport and
 * adds it to shells[], then calls nx_shell_init() once the instances have
 * started.  The instance's own thread runs the shell; this module only feeds its
 * RX ring from the socket, sends its output, and signals (re)connect/disconnect.
 */
#ifndef NX_SHELL_H
#define NX_SHELL_H

#ifdef __cplusplus
extern "C" {
#endif

struct cli_transport;

/** The transport a cli_instance binds to (CLI_INSTANCE_DEFINE in src/main.c). */
extern struct cli_transport nx_shell_transport;

/** Create the server socket, listen on port 23, spawn the server thread.  Call
 *  after nx_net_init() and after the shell instances have started.  Returns 0,
 *  or <0 (IP not up / socket / listen / thread create failed). */
int nx_shell_init(void);

#ifdef __cplusplus
}
#endif

#endif /* NX_SHELL_H */
