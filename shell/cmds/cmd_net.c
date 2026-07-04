/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_net.c
 * @brief   `net` shell command: on-board Ethernet + NetX Duo IPv4 (issues #49 P1/P2).
 *
 *   net info               MAC / PHY id / link state + IP / mask / gateway
 *   net link               re-run auto-negotiation and report link state
 *   net ping <a.b.c.d> [n]  ICMP echo n times (default 4) with RTT
 *   net ip <a.b.c.d/mask> [gw]  set a static address (stops DHCP)
 *   net dhcp               (re)acquire an address via DHCP
 *
 * The link half (info/link) talks to port/eth (eth_link.h); the IP half talks to
 * port/netxduo (nx_glue.h) -- the shell never includes nx_api.h directly.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "eth_link.h"
#include "nx_glue.h"
#include "nx_echo.h"
#include "nx_mjpeg.h"

#include <stdint.h>

static const char *speed_name(enum eth_speed s)
{
	return (s == ETH_LINK_SPEED_100M) ? "100M" : (s == ETH_LINK_SPEED_10M) ? "10M" : "?";
}

static const char *duplex_name(enum eth_duplex d)
{
	return (d == ETH_LINK_DUPLEX_FULL) ? "full" : (d == ETH_LINK_DUPLEX_HALF) ? "half" : "?";
}

/* Parse a base-10 unsigned (for the ping count). */
static int parse_uint(const char *s, uint32_t *out)
{
	uint32_t v = 0;

	if (s == NULL || *s == '\0')
		return -1;
	for (const char *p = s; *p != '\0'; p++) {
		if (*p < '0' || *p > '9')
			return -1;
		v = v * 10u + (uint32_t)(*p - '0');
	}
	*out = v;
	return 0;
}

/* Parse "a.b.c.d" into a host-order u32. */
static int parse_ipv4(const char *s, uint32_t *out)
{
	uint32_t b[4];
	int i = 0;
	const char *p = s;

	for (;;) {
		uint32_t v = 0;
		int digits = 0;

		while (*p >= '0' && *p <= '9') {
			v = v * 10u + (uint32_t)(*p - '0');
			if (v > 255u)
				return -1;
			p++;
			digits++;
		}
		if (digits == 0 || i > 3)
			return -1;
		b[i++] = v;
		if (*p == '\0')
			break;
		if (*p != '.')
			return -1;
		p++;
	}
	if (i != 4)
		return -1;
	*out = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
	return 0;
}

/* Parse "a.b.c.d/mask" into address + netmask (host order). */
static int parse_ipv4_cidr(const char *s, uint32_t *ip, uint32_t *mask)
{
	char buf[20];
	const char *slash = NULL;
	uint32_t bits;
	size_t n = 0;

	for (const char *p = s; *p != '\0'; p++) {
		if (*p == '/') { slash = p + 1; break; }
		if (n >= sizeof buf - 1)
			return -1;
		buf[n++] = *p;
	}
	buf[n] = '\0';
	if (slash == NULL || parse_ipv4(buf, ip) != 0)
		return -1;
	if (parse_uint(slash, &bits) != 0 || bits > 32u)
		return -1;
	*mask = (bits == 0) ? 0u : (0xFFFFFFFFu << (32u - bits));
	return 0;
}

static void print_octets(struct cli_instance *sh, const char *label, uint32_t a)
{
	cli_print(sh, "%s%lu.%lu.%lu.%lu", label, (unsigned long)((a >> 24) & 0xFF),
	          (unsigned long)((a >> 16) & 0xFF), (unsigned long)((a >> 8) & 0xFF),
	          (unsigned long)(a & 0xFF));
}

static unsigned mask_bits(uint32_t mask)
{
	unsigned n = 0;

	while (mask & 0x80000000u) { n++; mask <<= 1; }
	return n;
}

/* Guard: the link driver (P1) must be initialised. */
static int net_ready(struct cli_instance *sh)
{
	if (!eth_is_initialized()) {
		cli_error(sh, "net: not initialized\r\n");
		return 0;
	}
	return 1;
}

/* Guard: the NetX IP stack (P2) must be up. */
static int net_ip_ready(struct cli_instance *sh)
{
	if (!net_ready(sh))
		return 0;
	if (!nx_net_is_up()) {
		cli_error(sh, "net: IP stack not up\r\n");
		return 0;
	}
	return 1;
}

static void net_print_link(struct cli_instance *sh, const struct eth_link_info *li)
{
	if (li->up)
		cli_print(sh, "link:  up (%s %s)\r\n",
		          speed_name(li->speed), duplex_name(li->duplex));
	else
		cli_print(sh, "link:  down\r\n");
}

static void net_print_ip(struct cli_instance *sh, const struct nx_net_info *ni)
{
	if (ni->ip_valid) {
		print_octets(sh, "ip:    ", ni->ip);
		cli_print(sh, "/%u ", mask_bits(ni->mask));
		print_octets(sh, "gw ", ni->gw);
		cli_print(sh, " (%s)\r\n", ni->dhcp_mode ? "dhcp" : "static");
	} else {
		cli_print(sh, "ip:    none (%s)\r\n",
		          ni->dhcp_mode ? "dhcp pending" : "unset");
	}
}

static int cmd_net_info(struct cli_instance *sh, int argc, char **argv)
{
	struct eth_link_info li;

	(void)argc;
	(void)argv;
	if (!net_ready(sh))
		return 1;
	if (eth_link_get(&li) != ETH_OK) {
		cli_error(sh, "net: link query failed\r\n");
		return 1;
	}

	cli_print(sh, "mac:   %02x:%02x:%02x:%02x:%02x:%02x\r\n",
	          li.mac[0], li.mac[1], li.mac[2], li.mac[3], li.mac[4], li.mac[5]);
	cli_print(sh, "phy:   addr %lu, id %04x:%04x (LAN8742)\r\n",
	          (unsigned long)li.phy_addr, li.phy_id1, li.phy_id2);
	net_print_link(sh, &li);

	if (nx_net_is_up()) {
		struct nx_net_info ni;
		struct nx_mjpeg_stats ms;
		unsigned ep, ec, eb;

		if (nx_net_info_get(&ni) == NXG_OK)
			net_print_ip(sh, &ni);
		if (nx_echo_status(&ep, &ec, &eb))
			cli_print(sh, "echo:  listening on :%u (%u conns, %u bytes)\r\n",
			          ep, ec, eb);
		if (nx_mjpeg_stats_get(&ms))
			cli_print(sh, "mjpeg: http://board:80 (%s, %lu frames)\r\n",
			          ms.client ? "streaming" : "idle",
			          (unsigned long)ms.sent_frames);
	}
	return 0;
}

static int cmd_net_link(struct cli_instance *sh, int argc, char **argv)
{
	struct eth_link_info li;
	int rc;

	(void)argc;
	(void)argv;
	if (!net_ready(sh))
		return 1;

	if (eth_link_renegotiate() != ETH_OK) {
		cli_error(sh, "net: auto-negotiation restart failed\r\n");
		return 1;
	}
	cli_print(sh, "net: auto-negotiating (up to 5 s, Ctrl+C to stop)...\r\n");

	for (int i = 0; i < 50; i++) {
		if (cli_sleep(sh, 100) != 0)
			return 1;
		if (eth_link_get(&li) == ETH_OK && li.up) {
			net_print_link(sh, &li);
			return 0;
		}
	}
	rc = eth_link_get(&li);
	if (rc == ETH_OK)
		net_print_link(sh, &li);
	cli_warn(sh, "net: link not up after 5 s (cable? peer?)\r\n");
	return 0;
}

static int cmd_net_ping(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t ip, c;
	int count = 4, ok = 0;
	unsigned rmin = 0xFFFFFFFFu, rmax = 0, rsum = 0;

	if (!net_ip_ready(sh))
		return 1;
	if (parse_ipv4(argv[1], &ip) != 0) {
		cli_error(sh, "net: bad address '%s'\r\n", argv[1]);
		return 1;
	}
	if (argc >= 3 && parse_uint(argv[2], &c) == 0 && c > 0 && c <= 100)
		count = (int)c;

	cli_print(sh, "PING %s, %d probes:\r\n", argv[1], count);
	for (int i = 0; i < count; i++) {
		unsigned rtt = 0;
		int rc;

		if (cli_cancel_requested(sh))
			break;
		rc = nx_net_ping(ip, 1000, &rtt);
		if (rc == NXG_OK) {
			cli_print(sh, "  reply %d: %u ms\r\n", i + 1, rtt);
			ok++;
			rsum += rtt;
			if (rtt < rmin) rmin = rtt;
			if (rtt > rmax) rmax = rtt;
		} else if (rc == NXG_TIMEOUT) {
			cli_print(sh, "  probe %d: timeout\r\n", i + 1);
		} else {
			cli_error(sh, "  probe %d: error\r\n", i + 1);
		}
		if (i + 1 < count && cli_sleep(sh, 1000) != 0)
			break;        /* Ctrl+C between probes */
	}
	cli_print(sh, "%d/%d received", ok, count);
	if (ok > 0)
		cli_print(sh, ", rtt min/avg/max %u/%u/%u ms",
		          rmin, rsum / (unsigned)ok, rmax);
	cli_print(sh, "\r\n");
	return 0;
}

static int cmd_net_ip(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t ip, mask, gw = 0;

	if (!net_ip_ready(sh))
		return 1;
	if (parse_ipv4_cidr(argv[1], &ip, &mask) != 0) {
		cli_error(sh, "net: bad address '%s' (use a.b.c.d/mask)\r\n", argv[1]);
		return 1;
	}
	if (argc >= 3 && parse_ipv4(argv[2], &gw) != 0) {
		cli_error(sh, "net: bad gateway '%s'\r\n", argv[2]);
		return 1;
	}
	if (nx_net_set_static(ip, mask, gw) != NXG_OK) {
		cli_error(sh, "net: set static failed\r\n");
		return 1;
	}
	cli_print(sh, "net: static address set\r\n");
	return 0;
}

static int cmd_net_dhcp(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	if (!net_ip_ready(sh))
		return 1;
	if (nx_net_dhcp_renew() != NXG_OK) {
		cli_error(sh, "net: DHCP start failed\r\n");
		return 1;
	}
	cli_print(sh, "net: acquiring via DHCP (up to 10 s, Ctrl+C to stop)...\r\n");
	for (int i = 0; i < 100; i++) {
		struct nx_net_info ni;

		if (cli_sleep(sh, 100) != 0)
			return 1;
		if (nx_net_info_get(&ni) == NXG_OK && ni.ip_valid) {
			net_print_ip(sh, &ni);
			return 0;
		}
	}
	cli_warn(sh, "net: no DHCP lease after 10 s\r\n");
	return 0;
}

static int cmd_net_echo_start(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t port = 0;
	int rc;

	if (!net_ip_ready(sh))
		return 1;
	if (argc >= 2 && (parse_uint(argv[1], &port) != 0 || port == 0 || port > 65535)) {
		cli_error(sh, "net: bad port '%s'\r\n", argv[1]);
		return 1;
	}
	rc = nx_echo_start((unsigned)port);
	if (rc == -2) {
		cli_error(sh, "net: echo already running\r\n");
		return 1;
	}
	if (rc != 0) {
		cli_error(sh, "net: echo start failed\r\n");
		return 1;
	}
	cli_print(sh, "net: echo listening on :%u\r\n",
	          port ? (unsigned)port : NX_ECHO_DEFAULT_PORT);
	return 0;
}

static int cmd_net_echo_stop(struct cli_instance *sh, int argc, char **argv)
{
	int rc;

	(void)argc;
	(void)argv;
	if (!net_ready(sh))
		return 1;
	rc = nx_echo_stop();
	if (rc == -1) {
		cli_error(sh, "net: echo not running\r\n");
		return 1;
	}
	if (rc != 0) {
		cli_error(sh, "net: echo stop timed out\r\n");
		return 1;
	}
	cli_print(sh, "net: echo stopped\r\n");
	return 0;
}

CLI_SUBCMD_SET_CREATE(net_echo_subcmds,
	CLI_CMD_ARG(start, NULL, "start the TCP echo server [port] (default 7)",
	            cmd_net_echo_start, 1, 1),
	CLI_CMD(stop, NULL, "stop the TCP echo server", cmd_net_echo_stop),
	CLI_SUBCMD_SET_END);

/* ---- MJPEG-over-HTTP camera streaming (#49 P5) --------------------------- */

static const struct {
	const char     *name;
	enum camera_res res;
} mjpeg_res_names[] = {
	{ "qqvga", CAM_RES_QQVGA }, { "qvga", CAM_RES_QVGA },
	{ "vga", CAM_RES_VGA },
};

static int cmd_net_mjpeg_start(struct cli_instance *sh, int argc, char **argv)
{
	enum camera_res res = CAM_RES_QVGA;     /* default */
	int rc;

	if (!net_ip_ready(sh))
		return 1;
	if (argc >= 2) {
		size_t i;

		for (i = 0; i < sizeof mjpeg_res_names / sizeof mjpeg_res_names[0]; i++) {
			if (strcmp(argv[1], mjpeg_res_names[i].name) == 0) {
				res = mjpeg_res_names[i].res;
				break;
			}
		}
		if (i == sizeof mjpeg_res_names / sizeof mjpeg_res_names[0]) {
			cli_error(sh, "net: bad resolution '%s' "
			          "(qqvga|qvga|vga)\r\n", argv[1]);
			return 1;
		}
	}
	rc = nx_mjpeg_start(res);
	if (rc == -2) {
		cli_error(sh, "net: mjpeg already running\r\n");
		return 1;
	}
	if (rc == CAM_ERR_BUSY) {
		cli_error(sh, "net: camera busy (owned by gui preview or stream); "
		          "stop it first\r\n");
		return 1;
	}
	if (rc != 0) {
		cli_error(sh, "net: mjpeg start failed (%d)\r\n", rc);
		return 1;
	}
	cli_print(sh, "net: mjpeg streaming at http://<board-ip>:80/ (%s)\r\n",
	          argc >= 2 ? argv[1] : "qvga");
	return 0;
}

static int cmd_net_mjpeg_stop(struct cli_instance *sh, int argc, char **argv)
{
	int rc;

	(void)argc;
	(void)argv;
	if (!net_ready(sh))
		return 1;
	rc = nx_mjpeg_stop();
	if (rc == -1) {
		cli_error(sh, "net: mjpeg not running\r\n");
		return 1;
	}
	if (rc != 0) {
		cli_error(sh, "net: mjpeg stop timed out\r\n");
		return 1;
	}
	cli_print(sh, "net: mjpeg stopped\r\n");
	return 0;
}

static int cmd_net_mjpeg_stats(struct cli_instance *sh, int argc, char **argv)
{
	struct nx_mjpeg_stats st;

	(void)argc;
	(void)argv;
	if (!net_ready(sh))
		return 1;
	if (!nx_mjpeg_stats_get(&st)) {
		cli_print(sh, "mjpeg: not running\r\n");
		return 0;
	}
	cli_print(sh, "state:     %s\r\n", st.client ? "streaming" : "idle (no client)");
	cli_print(sh, "conns:     %lu\r\n", (unsigned long)st.conns);
	cli_print(sh, "sent:      %lu frames, %lu bytes\r\n",
	          (unsigned long)st.sent_frames, (unsigned long)st.sent_bytes);
	cli_print(sh, "dropped:   busy %lu, oversized %lu\r\n",
	          (unsigned long)st.drop_busy, (unsigned long)st.drop_oversized);
	cli_print(sh, "errors:    send %lu, pool %lu\r\n",
	          (unsigned long)st.send_err, (unsigned long)st.pool_fail);
	return 0;
}

CLI_SUBCMD_SET_CREATE(net_mjpeg_subcmds,
	CLI_CMD_ARG(start, NULL,
	            "start MJPEG-over-HTTP on :80 [res] (qqvga|qvga|vga, "
	            "default qvga)", cmd_net_mjpeg_start, 1, 1),
	CLI_CMD(stop, NULL, "stop MJPEG streaming and release the camera",
	        cmd_net_mjpeg_stop),
	CLI_CMD(stats, NULL, "MJPEG client / frame / drop counters",
	        cmd_net_mjpeg_stats),
	CLI_SUBCMD_SET_END);

CLI_SUBCMD_SET_CREATE(net_subcmds,
	CLI_CMD(info, NULL, "MAC / PHY id / link + IP / mask / gateway", cmd_net_info),
	CLI_CMD(link, NULL, "restart auto-negotiation and report link state",
	        cmd_net_link),
	CLI_CMD_ARG(ping, NULL, "ICMP echo <a.b.c.d> [count]", cmd_net_ping, 2, 1),
	CLI_CMD_ARG(ip, NULL, "set static <a.b.c.d/mask> [gw]", cmd_net_ip, 2, 1),
	CLI_CMD(dhcp, NULL, "(re)acquire an address via DHCP", cmd_net_dhcp),
	CLI_CMD(echo, net_echo_subcmds, "TCP echo server (start/stop)", NULL),
	CLI_CMD(mjpeg, net_mjpeg_subcmds,
	        "MJPEG-over-HTTP camera stream (start/stop/stats)", NULL),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(net, net_subcmds,
                 "on-board Ethernet (ETH MAC + LAN8742 RMII) + NetX Duo IPv4",
                 NULL, 1, 0);
