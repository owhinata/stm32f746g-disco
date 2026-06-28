/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nx_echo.c
 * @brief   NetX Duo TCP echo server (issue #49 P3).  See nx_echo.h.
 *
 * One dedicated thread, created once and parked when stopped (the camera /
 * guix_touch idiom -- avoids tx_thread delete/recreate).  Single connection at a
 * time: listen -> accept -> {receive -> send back} -> disconnect -> unaccept ->
 * relisten.  The received packet is re-sent directly (NetX keeps the header
 * room), which is the canonical NetX echo; on send failure it is released.
 * accept/receive use a short timeout so the loop falls out after a stop and
 * tears the socket down (no thread delete needed).  nx_echo_stop() then blocks
 * until that teardown finishes (up to ~1.5 s on a live connection -- the receive
 * timeout plus the graceful disconnect wait), so a stop/start pair never leaves
 * the old listener running on a stale port.
 */
#include "nx_api.h"

#include "nx_echo.h"
#include "nx_glue.h"

#define LOG_TAG "echo"
#include "log.h"

#define ECHO_PRIORITY     14          /* below IP(12)/DHCP(13); on-demand        */
#define ECHO_STACK_SIZE   2048u
#define ECHO_WINDOW       2048u        /* TCP receive window                      */
#define ECHO_LISTEN_QUEUE 1u
#define ECHO_POLL_TICKS   500u         /* accept/receive timeout (stop latency)   */

static TX_THREAD     echo_thread;
static UCHAR         echo_stack[ECHO_STACK_SIZE];
static NX_TCP_SOCKET echo_sock;

static volatile int      echo_run;     /* requested running (start=1, stop=0)    */
static volatile int      echo_active;  /* thread owns the socket lifecycle
                                          (set by start() BEFORE the thread runs,
                                          cleared by the thread on every exit path
                                          -- closes the start/stop startup race)  */
static volatile int      echo_listening; /* socket is actually in LISTEN -- so
                                          nx_echo_start() reports success only
                                          once the thread has really listened (no
                                          "connect immediately after start" race) */
static UINT              echo_port = NX_ECHO_DEFAULT_PORT;
static volatile unsigned echo_conns;   /* accepted connections                   */
static volatile unsigned echo_rx;      /* total bytes echoed                     */
static int               echo_created; /* the thread has been created once       */

static void echo_disconnect_cb(NX_TCP_SOCKET *s)
{
	(void)s;                            /* the receive loop detects the FIN       */
}

/* Echo one accepted connection until the peer disconnects or a stop is asked. */
static void echo_serve(void)
{
	for (;;) {
		NX_PACKET *pkt;
		UINT st = nx_tcp_socket_receive(&echo_sock, &pkt, ECHO_POLL_TICKS);

		if (st == NX_SUCCESS) {
			ULONG len = 0;

			if (!echo_run) {            /* stop asked: drop, don't block on send   */
				nx_packet_release(pkt);
				return;
			}
			nx_packet_length_get(pkt, &len);
			/* Re-send the received packet (NetX keeps header room).  On success
			   NetX owns it; on failure we must release it. */
			if (nx_tcp_socket_send(&echo_sock, pkt, NX_IP_PERIODIC_RATE) == NX_SUCCESS)
				echo_rx += (unsigned)len;
			else
				nx_packet_release(pkt);
		} else if (st == NX_NO_PACKET || st == NX_WAIT_ABORTED) {
			if (!echo_run)
				return;                 /* stop requested, still connected         */
			/* else: idle timeout, keep the connection open */
		} else {
			return;                     /* NX_NOT_CONNECTED / disconnect           */
		}
	}
}

static void echo_entry(ULONG arg)
{
	NX_IP *ip = (NX_IP *)nx_net_ip();

	(void)arg;
	for (;;) {
		/* Parked until started (short sleep -> a re-start listens promptly). */
		while (!echo_run)
			tx_thread_sleep(20);

		if (ip == NULL)
			ip = (NX_IP *)nx_net_ip();
		if (ip == NULL) { echo_run = 0; echo_active = 0; continue; }

		if (nx_tcp_socket_create(ip, &echo_sock, "net-echo", NX_IP_NORMAL,
		                         NX_FRAGMENT_OKAY, NX_IP_TIME_TO_LIVE, ECHO_WINDOW,
		                         NX_NULL, echo_disconnect_cb) != NX_SUCCESS) {
			LOG_ERR("socket create failed");
			echo_run = 0;
			echo_active = 0;
			continue;
		}
		if (nx_tcp_server_socket_listen(ip, echo_port, &echo_sock,
		                                ECHO_LISTEN_QUEUE, NX_NULL) != NX_SUCCESS) {
			LOG_ERR("listen :%u failed", (unsigned)echo_port);
			nx_tcp_socket_delete(&echo_sock);
			echo_run = 0;
			echo_active = 0;
			continue;
		}
		echo_listening = 1;             /* nx_echo_start() waits for this          */
		LOG_INF("listening on :%u", (unsigned)echo_port);

		while (echo_run) {
			if (nx_tcp_server_socket_accept(&echo_sock, ECHO_POLL_TICKS) != NX_SUCCESS)
				continue;                /* timeout -> re-accept (re-checks run)   */
			echo_conns++;
			echo_serve();
			nx_tcp_socket_disconnect(&echo_sock, NX_IP_PERIODIC_RATE);
			nx_tcp_server_socket_unaccept(&echo_sock);
			nx_tcp_server_socket_relisten(ip, echo_port, &echo_sock);
		}

		/* Stopped: tear the socket down and park. */
		echo_listening = 0;
		nx_tcp_server_socket_unaccept(&echo_sock);
		nx_tcp_server_socket_unlisten(ip, echo_port);
		nx_tcp_socket_delete(&echo_sock);
		echo_active = 0;
		LOG_INF("stopped");
	}
}

int nx_echo_start(unsigned port)
{
	if (!nx_net_is_up())
		return -1;
	if (echo_run || echo_active)
		return -2;                      /* running, or still tearing down a stop   */

	echo_port = port ? (UINT)port : NX_ECHO_DEFAULT_PORT;
	echo_conns = 0;
	echo_rx = 0;
	echo_listening = 0;
	/* Claim the lifecycle BEFORE the thread runs so a stop/start pair cannot slip
	   through the thread's startup window (echo_active is the guard, not the
	   later "listening" state). */
	echo_active = 1;
	echo_run = 1;

	if (!echo_created) {
		if (tx_thread_create(&echo_thread, "net-echo", echo_entry, 0,
		                     echo_stack, sizeof echo_stack,
		                     ECHO_PRIORITY, ECHO_PRIORITY,
		                     TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS) {
			echo_run = 0;
			echo_active = 0;
			return -3;                  /* thread create failed (distinct from -2) */
		}
		echo_created = 1;
	}

	/* Wait for the thread to ACTUALLY listen (or fail), so the caller's success
	   means the port is open -- a client connecting right after start no longer
	   races the listen.  Bounded ~1 s. */
	for (int i = 0; i < 100 && echo_active && !echo_listening; i++)
		tx_thread_sleep(10);
	if (!echo_listening) {
		echo_run = 0;                   /* listen failed/timed out -- stop it       */
		return -3;
	}
	return 0;
}

int nx_echo_stop(void)
{
	if (!echo_run && !echo_active)
		return -1;
	echo_run = 0;
	/* Wait (bounded) for the thread to finish tearing the socket down, so the
	   caller can immediately start again on a different port without racing the
	   old listener. */
	for (int i = 0; i < 30 && echo_active; i++)
		tx_thread_sleep(100);
	return echo_active ? -2 : 0;        /* -2: teardown did not complete in time   */
}

bool nx_echo_status(unsigned *port, unsigned *conns, unsigned *rx_bytes)
{
	if (port != NULL)
		*port = (unsigned)echo_port;
	if (conns != NULL)
		*conns = echo_conns;
	if (rx_bytes != NULL)
		*rx_bytes = echo_rx;
	return echo_run != 0;
}
