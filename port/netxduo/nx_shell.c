/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nx_shell.c
 * @brief   TCP network shell (telnet) over NetX Duo (issue #49 P4).  See nx_shell.h.
 *
 * One session at a time (N=1): the CLI §14 KILL/uninit lifecycle is not
 * implemented, so a static cli_instance is reused across connections rather than
 * created/destroyed per client.  The server thread owns listen/accept/relisten;
 * NetX callbacks (IP-thread context) only push to the RX ring / set flags; the
 * cli_instance thread runs the shell, resets a fresh session on CLI_EVT_CONN, and
 * does its own output via the transport write().
 *
 * Output enable (`connected`) is owned by the CLI thread (set in session_begin,
 * which the CLI thread calls on CLI_EVT_CONN AFTER resetting the editor state and
 * BEFORE the prompt) and cleared by the disconnect callback -- so a previous
 * command's late output is dropped after a reconnect and the new client's first
 * bytes are its own fresh prompt.
 */
#include "nx_api.h"

#include "nx_shell.h"
#include "nx_glue.h"          /* nx_net_ip / nx_net_pool */

#include "cli_instance.h"     /* struct cli_transport(_api), CLI_EVT_*, notify_* */
#include "cli_uart_ring.h"    /* the reusable SPSC byte ring                     */

#define LOG_TAG "nshell"
#include "log.h"

#define NX_SHELL_PORT        23u
#define NX_SHELL_WINDOW      2048u      /* TCP receive window                     */
#define NX_SHELL_TX_QUEUE    8u         /* cap TX queue depth -> protect eth_pool */
#define NX_SHELL_MSS         1400u      /* max bytes per TX packet                */
#define NX_SHELL_RX_RING     512u
#define NX_SHELL_EXTRACT     1500u      /* per-packet RX extraction buffer        */
#define NX_SHELL_PRIORITY    14
#define NX_SHELL_STACK       1536u

/* ---- transport backend (cli_transport_api over the TCP socket) ------------- */

struct cli_tcp {
	struct cli_instance *sh;            /* set by tcp_init from tr->sh           */
	struct cli_uart_ring rx_ring;       /* IP-thread producer, CLI-thread consumer */
	uint8_t              rx_buf[NX_SHELL_RX_RING];
	volatile int         connected;     /* write gate (CLI sets 1, disconnect 0) */
	uint32_t             tx_drop;       /* output packets dropped (pool/window)  */
};

static struct cli_tcp        tcp_ctx;
static NX_TCP_SOCKET         sock;
static TX_THREAD             server_thread;
static UCHAR                 server_stack[NX_SHELL_STACK];
static TX_SEMAPHORE          disc_sem;          /* posted by the disconnect cb     */
static volatile int          session_live;      /* a connection is currently accepted
                                                   (server sets 1 on accept, the
                                                   disconnect cb clears it) -- gates
                                                   session_begin so a CLI_EVT_CONN
                                                   that the CLI thread processes only
                                                   AFTER the client already vanished
                                                   does not resurrect `connected`.  */
static int                   iac_state;         /* telnet IAC strip (IP-thread dom) */
static uint8_t               extract_buf[NX_SHELL_EXTRACT];

static int tcp_init(struct cli_transport *tr)
{
	struct cli_tcp *c = (struct cli_tcp *)tr->ctx;

	cli_uart_ring_init(&c->rx_ring, c->rx_buf, sizeof c->rx_buf);
	c->sh = tr->sh;
	c->connected = 0;
	return 0;
}

static int tcp_enable(struct cli_transport *tr)
{
	(void)tr;                           /* the socket is armed by nx_shell_init    */
	return 0;
}

static int tcp_read(struct cli_transport *tr, uint8_t *data, size_t cap)
{
	struct cli_tcp *c = (struct cli_tcp *)tr->ctx;

	/* SPSC: the receive callback (IP thread) is the only producer, the CLI thread
	   the only consumer -- lock-free, like the UART backend. */
	return (int)cli_uart_ring_get_buf(&c->rx_ring, data, cap);
}

static int tcp_write(struct cli_transport *tr, const uint8_t *data, size_t len)
{
	struct cli_tcp *c = (struct cli_tcp *)tr->ctx;
	NX_PACKET_POOL *pool = (NX_PACKET_POOL *)nx_net_pool();
	NX_PACKET *pkt;
	ULONG n = (len > NX_SHELL_MSS) ? NX_SHELL_MSS : (ULONG)len;

	if (!c->connected || pool == NULL)
		return (int)len;                /* not connected: drop (req §11)           */

	if (nx_packet_allocate(pool, &pkt, NX_TCP_PACKET, NX_NO_WAIT) != NX_SUCCESS) {
		c->tx_drop++;
		return 0;                       /* pool empty -> core waits CLI_EVT_TX      */
	}
	if (nx_packet_data_append(pkt, (VOID *)data, n, pool, NX_NO_WAIT) != NX_SUCCESS) {
		nx_packet_release(pkt);
		c->tx_drop++;
		return 0;
	}
	if (nx_tcp_socket_send(&sock, pkt, NX_NO_WAIT) != NX_SUCCESS) {
		/* NX_WINDOW_OVERFLOW / NX_TX_QUEUE_DEPTH / NX_NOT_CONNECTED: must release.
		   The window/queue notify will fire cli_transport_notify_tx when space
		   frees; NX_NOT_CONNECTED (disconnect race) is harmlessly swallowed. */
		nx_packet_release(pkt);
		return 0;
	}
	return (int)n;
}

/* Telnet negotiation sent at the start of each session so a telnet client enters
   character-at-a-time mode (no local echo / no line buffering), which is what the
   CLI's interactive line editor needs.  IAC WILL ECHO (server echoes) + IAC WILL
   SUPPRESS-GO-AHEAD.  (A raw `nc` client shows these 6 bytes as harmless garbage
   before the prompt -- the session is telnet-first per the issue.) */
static const uint8_t telnet_charmode[] = {
	0xFFu, 0xFBu, 0x01u,   /* IAC WILL ECHO                  */
	0xFFu, 0xFBu, 0x03u,   /* IAC WILL SUPPRESS_GO_AHEAD     */
};

static void tcp_session_begin(struct cli_transport *tr)
{
	struct cli_tcp *c = (struct cli_tcp *)tr->ctx;
	uint8_t junk;

	/* Called by the CLI thread on CLI_EVT_CONN, after the editor state was reset
	   and before the prompt is drawn.  Drain any bytes left in the ring from a
	   previous session HERE (consumer side) so the ring stays SPSC -- only the CLI
	   thread ever gets, only the IP-thread callback ever puts -- then enable
	   output for the new session. */
	while (cli_uart_ring_get(&c->rx_ring, &junk))
		;
	/* Enable output only if the connection is still up: a client that vanished
	   before the CLI thread got here leaves session_live == 0, so the prompt is
	   dropped instead of resurrecting `connected` on a stale/next socket. */
	c->connected = session_live;
	if (c->connected)
		tcp_write(tr, telnet_charmode, sizeof telnet_charmode);
}

static const struct cli_transport_api cli_tcp_api = {
	tcp_init, tcp_enable, tcp_write, tcp_read, NULL, NULL, tcp_session_begin,
};

struct cli_transport nx_shell_transport = {
	.api = &cli_tcp_api,
	.sh  = NULL,                        /* set by cli_init()                       */
	.ctx = &tcp_ctx,
};

/* ---- NetX callbacks (IP-thread context: flag/ring/notify only) ------------ */

/* Telnet IAC strip: drop 0xFF + command [+ option] negotiation; 0xFF 0xFF -> one
   literal 0xFF.  Returns 1 if @p b is consumed (dropped), 0 if it is shell data. */
static int iac_consume(uint8_t b)
{
	switch (iac_state) {
	case 0:
		if (b == 0xFFu) { iac_state = 1; return 1; }
		return 0;
	case 1:
		if (b == 0xFFu) { iac_state = 0; return 0; }   /* IAC IAC -> literal 0xFF  */
		if (b >= 0xFBu && b <= 0xFEu) { iac_state = 2; return 1; } /* WILL/WONT/DO/DONT */
		iac_state = 0; return 1;                       /* other 2-byte command     */
	default:                                           /* option byte after WILL.. */
		iac_state = 0; return 1;
	}
}

static void shell_rx_notify(NX_TCP_SOCKET *s)
{
	NX_PACKET *pkt;

	while (nx_tcp_socket_receive(s, &pkt, NX_NO_WAIT) == NX_SUCCESS) {
		ULONG copied = 0;

		nx_packet_data_extract_offset(pkt, 0, extract_buf, sizeof extract_buf,
		                              &copied);
		nx_packet_release(pkt);
		for (ULONG i = 0; i < copied; i++) {
			uint8_t b = extract_buf[i];

			if (iac_consume(b))
				continue;
			cli_uart_ring_put(&tcp_ctx.rx_ring, b);    /* drop on full             */
		}
	}
	if (tcp_ctx.sh != NULL)
		cli_transport_notify_rx(tcp_ctx.sh);
}

static void shell_disconnect_cb(NX_TCP_SOCKET *s)
{
	(void)s;
	/* Peer FIN/RST.  Stop output, reset the telnet IAC state (same IP-thread
	   domain as iac_consume) and wake the server thread, which completes the close
	   and relistens.  NetX calls this on the ESTABLISHED FIN; disconnect_complete
	   only fires AFTER the app's own disconnect(), so this is the wake source. */
	session_live = 0;
	tcp_ctx.connected = 0;
	iac_state = 0;
	tx_semaphore_put(&disc_sem);
}

static void shell_tx_notify(NX_TCP_SOCKET *s)
{
	(void)s;                            /* window/queue space freed                */
	if (tcp_ctx.sh != NULL)
		cli_transport_notify_tx(tcp_ctx.sh);
}

/* ---- server thread: listen / accept / bind / unbind / relisten ------------ */

/* Create the socket + listen FROM THE SERVER THREAD: nx_tcp_socket_create /
   _listen are thread-only (the error-checking layer returns NX_CALLER_ERROR from
   the tx_application_define init context), so they must run after the scheduler
   starts -- same as the P3 echo server.  Returns 0 on success. */
static int shell_socket_setup(NX_IP *ip)
{
	if (nx_tcp_socket_create(ip, &sock, "net-shell", NX_IP_NORMAL, NX_FRAGMENT_OKAY,
	                         NX_IP_TIME_TO_LIVE, NX_SHELL_WINDOW, NX_NULL,
	                         shell_disconnect_cb) != NX_SUCCESS) {
		LOG_ERR("socket create failed");
		return -1;
	}
	if (nx_tcp_socket_receive_notify(&sock, shell_rx_notify) != NX_SUCCESS) {
		LOG_ERR("receive_notify failed");
		nx_tcp_socket_delete(&sock);
		return -1;
	}
	nx_tcp_socket_window_update_notify_set(&sock, shell_tx_notify);
	nx_tcp_socket_queue_depth_notify_set(&sock, shell_tx_notify);
	nx_tcp_socket_transmit_configure(&sock, NX_SHELL_TX_QUEUE,
	                                 2u * NX_IP_PERIODIC_RATE, 10, 1);
	if (nx_tcp_server_socket_listen(ip, NX_SHELL_PORT, &sock, 1, NX_NULL)
	    != NX_SUCCESS) {
		LOG_ERR("listen :%u failed", (unsigned)NX_SHELL_PORT);
		nx_tcp_socket_delete(&sock);
		return -1;
	}
	LOG_INF("listening on :%u (telnet)", (unsigned)NX_SHELL_PORT);
	return 0;
}

static void shell_server_entry(ULONG arg)
{
	NX_IP *ip = (NX_IP *)nx_net_ip();

	(void)arg;
	if (ip == NULL || shell_socket_setup(ip) != 0)
		return;                         /* telnet disabled; everything else runs    */

	for (;;) {
		/* Clear any stale disconnect signal before listening for the next client. */
		while (tx_semaphore_get(&disc_sem, TX_NO_WAIT) == TX_SUCCESS)
			;
		if (nx_tcp_server_socket_accept(&sock, NX_WAIT_FOREVER) != NX_SUCCESS)
			continue;

		/* Bind: mark the connection live, then ask the CLI thread to start a clean
		   session.  `connected` is left 0 here -- the CLI thread enables it
		   (session_begin, gated on session_live) once it reaches the CLI_EVT_CONN
		   handler and also drains the ring there (consumer side), so a
		   still-draining previous command cannot write to this new client and the
		   ring stays SPSC (issue #49 P4 D1a). */
		session_live = 1;
		if (tcp_ctx.sh != NULL)
			cli_transport_notify_conn(tcp_ctx.sh);
		LOG_INF("client connected");

		/* Wait for the peer to disconnect (FIN/RST -> shell_disconnect_cb posts). */
		tx_semaphore_get(&disc_sem, TX_WAIT_FOREVER);
		tcp_ctx.connected = 0;
		LOG_INF("client disconnected");

		/* Complete the close handshake (bounded), then re-arm the listen slot. */
		nx_tcp_socket_disconnect(&sock, NX_IP_PERIODIC_RATE);
		nx_tcp_server_socket_unaccept(&sock);
		nx_tcp_server_socket_relisten(ip, NX_SHELL_PORT, &sock);
	}
}

int nx_shell_init(void)
{
	/* Only ThreadX object creation here (safe in tx_application_define): the
	   socket create + listen run in the server thread (thread-only NetX APIs). */
	if (nx_net_ip() == NULL)
		return -1;
	if (tx_semaphore_create(&disc_sem, "nshd", 0) != TX_SUCCESS)
		return -1;
	if (tx_thread_create(&server_thread, "net-shell", shell_server_entry, 0,
	                     server_stack, sizeof server_stack,
	                     NX_SHELL_PRIORITY, NX_SHELL_PRIORITY,
	                     TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS) {
		LOG_ERR("server thread create failed");
		return -1;
	}
	return 0;
}
