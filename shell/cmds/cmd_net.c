/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_net.c
 * @brief   `net` shell command: on-board Ethernet status (issue #49 P1).
 *
 *   net info     MAC address, PHY address/id, link state (up/down, speed, duplex)
 *   net link     re-run auto-negotiation and report the resulting link state
 *
 * P1 brings up the ETH MAC + LAN8742A RMII PHY and detects link only (no
 * NetX/IP yet -- that is P2).  `net info` works whether or not the cable is
 * plugged in (it reports link down), so it guards on eth_is_initialized() rather
 * than link state.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "eth_link.h"

static const char *speed_name(enum eth_speed s)
{
	return (s == ETH_LINK_SPEED_100M) ? "100M" : (s == ETH_LINK_SPEED_10M) ? "10M" : "?";
}

static const char *duplex_name(enum eth_duplex d)
{
	return (d == ETH_LINK_DUPLEX_FULL) ? "full" : (d == ETH_LINK_DUPLEX_HALF) ? "half" : "?";
}

/* Shared guard: the driver must have initialised (link may still be down). */
static int net_ready(struct cli_instance *sh)
{
	if (!eth_is_initialized()) {
		cli_error(sh, "net: not initialized\r\n");
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

	/* Poll the eth-link thread's snapshot for up to ~5 s, cancellable. */
	for (int i = 0; i < 50; i++) {
		if (cli_sleep(sh, 100) != 0)      /* 100 ticks = 100 ms; nonzero = Ctrl+C */
			return 1;
		if (eth_link_get(&li) == ETH_OK && li.up) {
			net_print_link(sh, &li);
			return 0;
		}
	}

	rc = eth_link_get(&li);
	if (rc == ETH_OK)
		net_print_link(sh, &li);     /* still down */
	cli_warn(sh, "net: link not up after 5 s (cable? peer?)\r\n");
	return 0;
}

CLI_SUBCMD_SET_CREATE(net_subcmds,
	CLI_CMD(info, NULL, "MAC / PHY id / link state (up, speed, duplex)",
	        cmd_net_info),
	CLI_CMD(link, NULL, "restart auto-negotiation and report link state",
	        cmd_net_link),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(net, net_subcmds,
                 "on-board Ethernet (ETH MAC + LAN8742 RMII)", NULL, 1, 0);
