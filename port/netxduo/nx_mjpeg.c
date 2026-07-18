/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nx_mjpeg.c
 * @brief   MJPEG-over-HTTP camera streaming server (issue #49 P5).  See nx_mjpeg.h.
 *
 * The camera frame pipeline's "eth_sink": a SYNCHRONOUS copy push sink.  In the
 * producer thread, consume() memcpy's the JPEG frame into a private SDRAM buffer
 * and puts the pin immediately (in-flight is always 0, exactly like the GUIX
 * sink), so the camera's async teardown stays correct.  The HTTP server thread
 * then sends mjpeg_buf out as one multipart/x-mixed-replace part, decoupled from
 * the pipeline.  A single buf_busy flag (set by consume after the copy, cleared
 * by the HTTP thread on every send-exit path) gates the one-deep handoff.
 *
 * Lifecycle: one thread created once and parked when stopped; the socket create
 * + listen run in the thread (thread-only NetX APIs).
 */
#include <stdio.h>           /* snprintf */
#include <string.h>          /* memcpy / memset */

#include "nx_api.h"

#include "nx_mjpeg.h"
#include "nx_glue.h"         /* nx_net_ip / nx_net_pool / nx_net_is_up */
#include "camera.h"          /* camera_subscribe_oneshot / camera_subscribed / camera_frame_put */
#include "frame_pipeline.h"  /* struct frame_sink / frame_desc / FRAME_POLICY_* */

#define LOG_TAG "mjpeg"
#include "log.h"

#define NX_MJPEG_PORT        80u
#define NX_MJPEG_WINDOW      2048u       /* TCP receive window (we barely read)   */
#define NX_MJPEG_TX_QUEUE    8u          /* cap unacked TX packets -> protect pool */
#define MJPEG_MSS_CAP        1400u       /* max bytes per TX packet               */
#define MJPEG_BUF_BYTES      262144u     /* = JPEG frame budget (65535*4): a frame
                                            never exceeds it, so no oversized drop */
#define MJPEG_PRIORITY       14          /* below IP(12)/DHCP(13), with net-shell */
#define MJPEG_STACK          2048u
#define MJPEG_ACCEPT_TICKS   500u        /* accept timeout -> stop latency         */
#define MJPEG_POLL_TICKS     200u        /* frame wait -> disconnect latency       */
#define MJPEG_ALLOC_TICKS    100u        /* packet alloc wait                      */
#define MJPEG_SEND_TICKS     NX_IP_PERIODIC_RATE  /* per-chunk send wait (1 s)     */
#define MJPEG_BOUNDARY       "mjpegstream"

static const char http_header[] =
	"HTTP/1.0 200 OK\r\n"
	"Connection: close\r\n"
	"Cache-Control: no-cache\r\n"
	"Pragma: no-cache\r\n"
	"Content-Type: multipart/x-mixed-replace; boundary=" MJPEG_BOUNDARY "\r\n"
	"\r\n";

/* The private copy buffer lives with the NetX traffic in FMC bank2/3 (.sdram.eth),
   away from the camera DMA arena (bank1) and the LTDC surface (bank0) -- #65. */
static uint8_t mjpeg_buf[MJPEG_BUF_BYTES]
	__attribute__((aligned(32), section(".sdram.eth")));

static TX_THREAD     mjpeg_thread;
static UCHAR         mjpeg_stack[MJPEG_STACK];
static NX_TCP_SOCKET sock;
static TX_SEMAPHORE  frame_sem;          /* consume posts; the server sends        */

static volatile int  mjpeg_run;          /* requested running (start=1, stop=0)    */
static volatile int  mjpeg_active;       /* thread owns the socket lifecycle       */
static volatile int  mjpeg_listening;    /* socket actually in LISTEN              */
static volatile int  client_connected;   /* a browser is connected (output gate)   */
static volatile int  producer_dead;      /* camera producer torn down (close cb)   */
static volatile int  buf_busy;           /* mjpeg_buf holds a frame to send         */
static volatile uint32_t buf_len;        /* valid bytes in mjpeg_buf               */
static int           mjpeg_created;      /* the thread has been created once       */
static uint8_t       cur_res;            /* enum camera_res of the active stream    */

static struct {
	uint32_t conns;
	uint32_t sent_frames;
	uint32_t sent_bytes;
	uint32_t drop_busy;
	uint32_t drop_oversized;
	uint32_t send_err;
	uint32_t pool_fail;
} mstats;

/* ---- the camera frame-pipeline sink (synchronous copy) -------------------- */

static int  eth_open(void *ctx, enum frame_format fmt, uint16_t w, uint16_t h);
static int  eth_consume(void *ctx, const struct frame_desc *f);
static void eth_close(void *ctx);

static struct frame_sink eth_sink = {
	.name    = "mjpeg",
	.policy  = FRAME_POLICY_DROP,
	.open    = eth_open,
	.consume = eth_consume,
	.close   = eth_close,
};

static int eth_open(void *ctx, enum frame_format fmt, uint16_t w, uint16_t h)
{
	(void)ctx; (void)w; (void)h;

	if (fmt != FRAME_FMT_JPEG)
		return -1;                  /* MJPEG only carries JPEG                  */
	/* Fresh session reset on attach -- the single reset point.  A normal stop's
	   detach() also calls close() (sets producer_dead), so clearing it here on the
	   next attach prevents a stale producer_dead from instantly ending the new
	   session.  Also clears the handoff flags and drains the frame signal. */
	producer_dead    = 0;
	client_connected = 0;
	buf_busy         = 0;
	buf_len          = 0;
	memset(&mstats, 0, sizeof mstats);
	while (tx_semaphore_get(&frame_sem, TX_NO_WAIT) == TX_SUCCESS)
		;
	return 0;
}

static int eth_consume(void *ctx, const struct frame_desc *f)
{
	(void)ctx;

	/* Producer-thread context, lock-free, slot pre-pinned.  Always put exactly
	   once (synchronous): copy when we can, otherwise drop -- never hold the pin
	   across the network send, so in-flight stays 0 for the camera teardown. */
	if (!mjpeg_run || !client_connected) {
		camera_frame_put(&eth_sink, f);          /* no client -> drop           */
		return 0;
	}
	if (buf_busy) {
		mstats.drop_busy++;                      /* HTTP still sending          */
		camera_frame_put(&eth_sink, f);
		return 0;
	}
	if (f->bytes > MJPEG_BUF_BYTES) {
		mstats.drop_oversized++;                 /* should not happen (= budget) */
		camera_frame_put(&eth_sink, f);
		return 0;
	}
	memcpy(mjpeg_buf, f->data, f->bytes);
	buf_len = f->bytes;
	camera_frame_put(&eth_sink, f);              /* release the pin immediately  */
	buf_busy = 1;                                /* hand off to the HTTP thread  */
	(void)tx_semaphore_put(&frame_sem);
	return 0;
}

static void eth_close(void *ctx)
{
	(void)ctx;
	producer_dead = 1;              /* producer async teardown (e.g. DCMI overrun) */
}

/* ---- TCP send helpers (HTTP-thread context) ------------------------------- */

/* Send @p len bytes in MSS-sized packets (NetX fragments a >MSS payload itself,
   double-spending the pool -- so we chunk).  Returns 0, or -1 on pool/send fail. */
static int mjpeg_send(const uint8_t *data, ULONG len)
{
	NX_PACKET_POOL *pool = (NX_PACKET_POOL *)nx_net_pool();
	ULONG mss = 0, off = 0;

	if (pool == NULL)
		return -1;
	if (nx_tcp_socket_mss_get(&sock, &mss) != NX_SUCCESS || mss == 0
	    || mss > MJPEG_MSS_CAP)
		mss = MJPEG_MSS_CAP;

	while (off < len) {
		NX_PACKET *pkt;
		ULONG n = len - off;

		/* Abort mid-frame on stop/disconnect so a slow peer cannot hold the frame
		   loop past nx_mjpeg_stop()'s bounded teardown wait (a full frame is up to
		   ~180 MSS chunks).  The partial frame breaks this multipart part, but the
		   connection is being torn down anyway. */
		if (!mjpeg_run || !client_connected)
			return -1;
		if (n > mss)
			n = mss;
		if (nx_packet_allocate(pool, &pkt, NX_TCP_PACKET, MJPEG_ALLOC_TICKS)
		    != NX_SUCCESS) {
			mstats.pool_fail++;
			return -1;
		}
		if (nx_packet_data_append(pkt, (VOID *)(data + off), n, pool,
		                          MJPEG_ALLOC_TICKS) != NX_SUCCESS) {
			nx_packet_release(pkt);
			mstats.pool_fail++;
			return -1;
		}
		if (nx_tcp_socket_send(&sock, pkt, MJPEG_SEND_TICKS) != NX_SUCCESS) {
			nx_packet_release(pkt);
			mstats.send_err++;
			return -1;
		}
		off += n;
	}
	return 0;
}

/* Send one multipart part from mjpeg_buf.  Returns 0, or -1 (disconnect). */
static int mjpeg_send_frame(void)
{
	char hdr[96];
	ULONG n = buf_len;                 /* stable while buf_busy (consume waits)   */
	int hl = snprintf(hdr, sizeof hdr,
	                  "--" MJPEG_BOUNDARY "\r\n"
	                  "Content-Type: image/jpeg\r\n"
	                  "Content-Length: %lu\r\n\r\n", (unsigned long)n);

	if (hl < 0 || (size_t)hl >= sizeof hdr)
		return -1;
	if (mjpeg_send((const uint8_t *)hdr, (ULONG)hl) != 0)
		return -1;
	if (mjpeg_send(mjpeg_buf, n) != 0)
		return -1;
	if (mjpeg_send((const uint8_t *)"\r\n", 2u) != 0)
		return -1;
	mstats.sent_frames++;
	mstats.sent_bytes += n;
	return 0;
}

/* ---- HTTP server thread --------------------------------------------------- */

static void mjpeg_disconnect_cb(NX_TCP_SOCKET *s)
{
	(void)s;
	client_connected = 0;          /* serve loop ends; must NOT touch buf_busy   */
}

/* Serve one accepted client: skip its GET, send the multipart header, then push
   frames until disconnect / stop / producer death. */
static void mjpeg_serve(void)
{
	NX_PACKET *req;

	/* Skip the HTTP request (one packet, best-effort -- path is ignored). */
	if (nx_tcp_socket_receive(&sock, &req, MJPEG_POLL_TICKS) == NX_SUCCESS)
		nx_packet_release(req);

	/* Open the output gate BEFORE sending: the per-chunk send abort (mjpeg_send)
	   keys off client_connected, and the producer's consume() begins staging
	   frames into mjpeg_buf from here.  Start with an empty handoff + no stale
	   frame signal. */
	buf_busy = 0;
	while (tx_semaphore_get(&frame_sem, TX_NO_WAIT) == TX_SUCCESS)
		;
	client_connected = 1;

	if (mjpeg_send((const uint8_t *)http_header, sizeof http_header - 1u) != 0) {
		client_connected = 0;
		return;
	}

	for (;;) {
		(void)tx_semaphore_get(&frame_sem, MJPEG_POLL_TICKS);
		if (!mjpeg_run || !client_connected || producer_dead) {
			buf_busy = 0;          /* clear the unsent handoff before leaving     */
			break;
		}
		if (buf_busy) {
			int r = mjpeg_send_frame();

			buf_busy = 0;          /* HTTP thread is the SOLE clearer (all exits) */
			if (r != 0)
				break;             /* send error -> disconnect                    */
		}
	}
	client_connected = 0;
}

static int mjpeg_socket_setup(NX_IP *ip)
{
	if (nx_tcp_socket_create(ip, &sock, "net-mjpeg", NX_IP_NORMAL, NX_FRAGMENT_OKAY,
	                         NX_IP_TIME_TO_LIVE, NX_MJPEG_WINDOW, NX_NULL,
	                         mjpeg_disconnect_cb) != NX_SUCCESS) {
		LOG_ERR("socket create failed");
		return -1;
	}
	/* Cap the TX queue so a fast stream cannot drain eth_pool away from RX/shell;
	   2 s retransmit, 10 retries, x2 backoff. */
	nx_tcp_socket_transmit_configure(&sock, NX_MJPEG_TX_QUEUE,
	                                 2u * NX_IP_PERIODIC_RATE, 10, 1);
	if (nx_tcp_server_socket_listen(ip, NX_MJPEG_PORT, &sock, 1, NX_NULL)
	    != NX_SUCCESS) {
		LOG_ERR("listen :%u failed", (unsigned)NX_MJPEG_PORT);
		nx_tcp_socket_delete(&sock);
		return -1;
	}
	return 0;
}

static void mjpeg_entry(ULONG arg)
{
	NX_IP *ip = (NX_IP *)nx_net_ip();

	(void)arg;
	for (;;) {
		/* Parked until started (short sleep -> a re-start listens promptly). */
		while (!mjpeg_run)
			tx_thread_sleep(20);

		if (ip == NULL)
			ip = (NX_IP *)nx_net_ip();
		if (ip == NULL || mjpeg_socket_setup(ip) != 0) {
			mjpeg_run = 0;
			mjpeg_active = 0;
			continue;              /* start's !listening path unsubscribes (#101) */
		}
		mjpeg_listening = 1;       /* nx_mjpeg_start() waits for this              */
		LOG_INF("listening on :%u (http mjpeg)", (unsigned)NX_MJPEG_PORT);

		/* Serve until an explicit `net mjpeg stop` (mjpeg_run=0) or a non-recover
		   base teardown.  A transient DCMI overrun (base auto-recovering, #100)
		   pauses serving and resumes without dropping the listen socket;
		   camera_subscribed() is the single source of truth that tells "released
		   for good" from "paused for recovery" (#101, avoids the stale-close race).
		   A relisten failure breaks out to a full socket re-setup. */
		while (mjpeg_run) {
			/* Accept + serve while the base is delivering (producer_dead=0). */
			while (mjpeg_run && !producer_dead) {
				if (nx_tcp_server_socket_accept(&sock, MJPEG_ACCEPT_TICKS)
				    != NX_SUCCESS)
					continue;      /* timeout -> re-accept (re-checks run)    */
				mstats.conns++;
				LOG_INF("client connected");
				mjpeg_serve();
				LOG_INF("client disconnected (%lu frames)",
				        (unsigned long)mstats.sent_frames);
				nx_tcp_socket_disconnect(&sock, NX_IP_PERIODIC_RATE);
				nx_tcp_server_socket_unaccept(&sock);
				if (nx_tcp_server_socket_relisten(ip, NX_MJPEG_PORT, &sock)
				    != NX_SUCCESS) {
					LOG_ERR("relisten failed");
					break;     /* -> re-setup the socket (below)         */
				}
			}
			if (!mjpeg_run)
				break;             /* explicit stop                          */
			if (!producer_dead)
				break;             /* relisten failed -> full socket re-setup */

			/* producer_dead: the base tore this sink down.  Released (a
			   non-recover base stop) -> fully stop; still enabled (an overrun
			   recovery is in flight) -> pause for the re-open (eth_open clears
			   producer_dead) or a recovery giveup (camera_subscribed -> 0). */
			if (!camera_subscribed(&eth_sink)) {
				LOG_WRN("camera base stopped -- auto-stopping mjpeg");
				mjpeg_run = 0;
				break;
			}
			LOG_INF("camera base overrun -- mjpeg paused for recovery");
			while (mjpeg_run && producer_dead
			       && camera_subscribed(&eth_sink))
				tx_thread_sleep(20);
			if (mjpeg_run && !producer_dead)
				LOG_INF("camera base recovered -- mjpeg resumed");
			/* else: stop (mjpeg_run=0) or giveup (camera_subscribed=0,
			   producer_dead still 1) -> the outer while re-checks and stops. */
		}

		/* Stopped: tear the socket down and park. */
		mjpeg_listening = 0;
		nx_tcp_server_socket_unaccept(&sock);
		nx_tcp_server_socket_unlisten(ip, NX_MJPEG_PORT);
		nx_tcp_socket_delete(&sock);
		mjpeg_active = 0;
		LOG_INF("stopped");
	}
}

/* ---- public API ----------------------------------------------------------- */

int nx_mjpeg_start(void)
{
	struct camera_mode m;
	int rc;

	if (!nx_net_is_up())
		return -1;
	if (mjpeg_run || mjpeg_active)
		return -2;                  /* running, or still tearing down a stop       */

	/* #101: MJPEG is a JPEG-class subscriber -- it no longer owns/starts the base.
	   The base must already be streaming JPEG (`camera format jpeg` + `camera stream
	   start`).  Report the precise reason so `net mjpeg start` never silently opens a
	   port that serves no frames (the #97 class of bug). */
	if (!camera_streaming())
		return NX_MJPEG_NO_CAPTURE;
	if (camera_get_mode(&m) != 0 || !m.is_jpeg)
		return NX_MJPEG_FMT_CLASH;  /* base is raster: mjpeg needs JPEG            */

	/* Create the ThreadX objects exactly once (idempotent across start/stop).  The
	   thread parks on !mjpeg_run, so creating it before the sink is attached is
	   harmless and keeps a failed start from re-creating the same semaphore. */
	if (!mjpeg_created) {
		if (tx_semaphore_create(&frame_sem, "mjpeg", 0) != TX_SUCCESS)
			return -3;
		if (tx_thread_create(&mjpeg_thread, "net-mjpeg", mjpeg_entry, 0,
		                     mjpeg_stack, sizeof mjpeg_stack,
		                     MJPEG_PRIORITY, MJPEG_PRIORITY,
		                     TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS) {
			tx_semaphore_delete(&frame_sem);
			return -3;
		}
		mjpeg_created = 1;
	}

	/* Attach as a non-persistent JPEG subscriber to the running base.  The pre-check
	   above is just for a precise error message: camera_subscribe_oneshot() is STRICT
	   (attaches to a live JPEG base under one cam_lock, or fails), so if the base
	   stopped since the pre-check this returns an error rather than a ghost idle
	   registration (#101).  A successful attach calls eth_open(), which resets the
	   session state + stats and clears producer_dead.  Only then do we claim the
	   lifecycle (mjpeg_run wakes the parked thread). */
	rc = camera_subscribe_oneshot(&eth_sink, CAM_FMT_JPEG);
	if (rc != 0)
		return rc;

	cur_res = m.res;
	mjpeg_listening = 0;
	mjpeg_active = 1;
	mjpeg_run = 1;

	/* Wait (bounded ~1 s) for the thread to actually listen, so success means the
	   port is open and the sink is attached. */
	for (int i = 0; i < 100 && mjpeg_active && !mjpeg_listening; i++)
		tx_thread_sleep(10);
	if (!mjpeg_listening) {
		mjpeg_run = 0;
		camera_unsubscribe(&eth_sink);
		return -3;
	}
	/* #101: the base could have been torn down (cascade released this oneshot sink)
	   while the thread was coming up -- don't report success for a stream that has
	   already stopped.  camera_subscribed() is the single source of truth. */
	if (!camera_subscribed(&eth_sink)) {
		mjpeg_run = 0;
		for (int i = 0; i < 30 && mjpeg_active; i++)
			tx_thread_sleep(10);
		camera_unsubscribe(&eth_sink);
		return -3;
	}
	return 0;
}

int nx_mjpeg_stop(void)
{
	if (!mjpeg_run && !mjpeg_active)
		return -1;
	/* Order (D3a): stop output gating, then stop, then detach the sink.  #101:
	   unsubscribe DETACHES ONLY (the base keeps running for other subscribers); a
	   base cascade stop is a separate `camera stream stop`.  Idempotent if the base
	   already released this oneshot sink (in-flight is always 0). */
	client_connected = 0;
	mjpeg_run = 0;
	(void)tx_semaphore_put(&frame_sem);   /* wake the serve loop if waiting        */
	camera_unsubscribe(&eth_sink);        /* detach the sink; base keeps running   */

	/* Wait (bounded) for the thread to tear the socket down and park. */
	for (int i = 0; i < 30 && mjpeg_active; i++)
		tx_thread_sleep(100);
	return mjpeg_active ? -2 : 0;
}

bool nx_mjpeg_stats_get(struct nx_mjpeg_stats *out)
{
	if (out != NULL) {
		out->running        = (mjpeg_run != 0);
		out->client         = (client_connected != 0);
		out->res            = cur_res;
		out->conns          = mstats.conns;
		out->sent_frames    = mstats.sent_frames;
		out->sent_bytes     = mstats.sent_bytes;
		out->drop_busy      = mstats.drop_busy;
		out->drop_oversized = mstats.drop_oversized;
		out->send_err       = mstats.send_err;
		out->pool_fail      = mstats.pool_fail;
	}
	return mjpeg_run != 0;
}
