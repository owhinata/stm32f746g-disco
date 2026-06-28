/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nx_echo.h
 * @brief   NetX Duo TCP echo server (issue #49 P3).
 *
 * A minimal single-connection TCP echo service on top of the P2 IP stack, used
 * to validate the NetX TCP socket path (listen -> accept -> receive -> send ->
 * disconnect -> relisten).  Its socket lifecycle is the template the P4 network
 * shell (cli_backend_tcp, #77) builds on.  Start/stop from the shell; connect
 * with `nc <board-ip> <port>` (default port 7) and every line is echoed back.
 */
#ifndef NX_ECHO_H
#define NX_ECHO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NX_ECHO_DEFAULT_PORT 7u

/** Start the echo server on @p port (0 = NX_ECHO_DEFAULT_PORT).  Returns 0,
 *  or <0 (IP not up / already running / thread create failed). */
int  nx_echo_start(unsigned port);

/** Stop the echo server (tears down the socket, parks the thread).  Returns 0
 *  or <0 if it was not running. */
int  nx_echo_stop(void);

/** True if running; fills @p port / @p conns (accepted connections) /
 *  @p rx_bytes (total echoed) when non-NULL. */
bool nx_echo_status(unsigned *port, unsigned *conns, unsigned *rx_bytes);

#ifdef __cplusplus
}
#endif

#endif /* NX_ECHO_H */
