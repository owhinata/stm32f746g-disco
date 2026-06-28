/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nx_glue.c
 * @brief   NetX Duo IPv4 bring-up + diagnostics facade (issue #49 P2).  See nx_glue.h.
 *
 * The single packet pool lives in `.sdram.eth` (FMC bank2, MPU non-cacheable):
 * the new HAL_ETH does no D-cache maintenance, so every DMA-visible payload --
 * RX buffers, TX payloads, and (via nx_dhcp_packet_pool_set) the DHCP datagrams
 * -- must be non-cacheable.  The control blocks (NX_IP/NX_PACKET_POOL/NX_DHCP)
 * and the IP thread stack / ARP cache are CPU-only, so they stay in regular SRAM.
 */
#include "nx_api.h"
#include "nxd_dhcp_client.h"

#include "nx_glue.h"
#include "nx_eth_driver.h"

#define LOG_TAG "net"
#include "log.h"

/* Pool: one Ethernet frame (1514) + the driver's 2-byte RX align pad + headroom,
   ~38 packets.  In non-cacheable SDRAM (zero-copy: payloads go straight to the
   ETH DMA). */
#define NXG_PAYLOAD       1600u
#define NXG_POOL_BYTES    (64u * 1024u)
#define NXG_IP_PRIORITY   12          /* below camera(10), above touch/GUIX/cli  */
#define NXG_IP_STACK      2048u
#define NXG_ARP_CACHE     1040u       /* ~20 entries                             */

static UCHAR eth_pool_mem[NXG_POOL_BYTES]
	__attribute__((aligned(32), section(".sdram.eth")));

static NX_PACKET_POOL eth_pool;       /* control block: CPU-only -> regular SRAM */
static NX_IP          eth_ip;
static NX_DHCP        eth_dhcp;        /* user pool set -> no embedded DMA buffer */
static ULONG          ip_stack[NXG_IP_STACK / sizeof(ULONG)];
static ULONG          arp_cache[NXG_ARP_CACHE / sizeof(ULONG)];

static bool nx_up;
static bool dhcp_created;
static bool dhcp_started;
static bool static_mode;

extern VOID nx_eth_driver(NX_IP_DRIVER *driver_req_ptr);

/* NetX link-status callback (IP helper thread context, under nx_ip_protection):
   update the interface link flag and kick DHCP on the first link-up. */
static void nx_link_status_cb(NX_IP *ip, UINT iface_index, UINT link_up)
{
	ip->nx_ip_interface[iface_index].nx_interface_link_up = (UCHAR)link_up;

	if (link_up && dhcp_created && !static_mode && !dhcp_started) {
		if (nx_dhcp_start(&eth_dhcp) == NX_SUCCESS)
			dhcp_started = true;
	}
}

int nx_net_init(void)
{
	UINT s;

	nx_system_initialize();

	s = nx_packet_pool_create(&eth_pool, "eth", NXG_PAYLOAD,
	                          eth_pool_mem, sizeof eth_pool_mem);
	if (s != NX_SUCCESS) {
		LOG_ERR("packet pool create failed (0x%02x)", s);
		return NXG_ERR;
	}

	/* Bind the RX pool to the driver BEFORE nx_ip_create (which runs the driver
	   INITIALIZE on the IP thread) -- it measures the packet<->payload offset. */
	nx_eth_driver_set_pool(&eth_pool);

	s = nx_ip_create(&eth_ip, "eth", 0, 0xFFFFFF00UL, &eth_pool, nx_eth_driver,
	                 (VOID *)ip_stack, sizeof ip_stack, NXG_IP_PRIORITY);
	if (s != NX_SUCCESS) {
		LOG_ERR("ip create failed (0x%02x)", s);
		return NXG_ERR;
	}

	nx_arp_enable(&eth_ip, (VOID *)arp_cache, sizeof arp_cache);
	nx_icmp_enable(&eth_ip);
	nx_udp_enable(&eth_ip);              /* DHCP needs UDP                        */
	nx_tcp_enable(&eth_ip);              /* P3-ready (no sockets yet)             */

	nx_ip_link_status_change_notify_set(&eth_ip, nx_link_status_cb);

	/* DHCP is the boot default (user choice).  Created here, started by the
	   link-up callback; reuse the shared non-cacheable pool. */
	if (nx_dhcp_create(&eth_dhcp, &eth_ip, "eth") == NX_SUCCESS) {
		nx_dhcp_packet_pool_set(&eth_dhcp, &eth_pool);
		dhcp_created = true;
	} else {
		LOG_WRN("dhcp create failed; use 'net ip' for a static address");
	}

	nx_up = true;
	LOG_INF("NetX Duo up (IPv4); pool %u B, IP thread prio %u",
	        (unsigned)NXG_POOL_BYTES, NXG_IP_PRIORITY);
	return NXG_OK;
}

bool nx_net_is_up(void)
{
	return nx_up;
}

int nx_net_info_get(struct nx_net_info *out)
{
	ULONG ip = 0, mask = 0, gw = 0;

	if (!nx_up)
		return NXG_ERR_STATE;

	nx_ip_address_get(&eth_ip, &ip, &mask);
	nx_ip_gateway_address_get(&eth_ip, &gw);
	out->ip = (uint32_t)ip;
	out->mask = (uint32_t)mask;
	out->gw = (uint32_t)gw;
	out->ip_valid = (ip != 0);
	out->dhcp_mode = dhcp_created && !static_mode;
	return NXG_OK;
}

int nx_net_set_static(uint32_t ip, uint32_t mask, uint32_t gw)
{
	if (!nx_up)
		return NXG_ERR_STATE;

	if (dhcp_started) {
		nx_dhcp_stop(&eth_dhcp);
		dhcp_started = false;
	}
	static_mode = true;
	if (nx_ip_address_set(&eth_ip, ip, mask) != NX_SUCCESS)
		return NXG_ERR;
	nx_ip_gateway_address_set(&eth_ip, gw);
	LOG_INF("static %lu.%lu.%lu.%lu", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
	        (ip >> 8) & 0xFF, ip & 0xFF);
	return NXG_OK;
}

int nx_net_dhcp_renew(void)
{
	if (!nx_up)
		return NXG_ERR_STATE;
	if (!dhcp_created)
		return NXG_ERR;

	static_mode = false;
	if (!dhcp_started) {
		if (nx_dhcp_start(&eth_dhcp) != NX_SUCCESS)
			return NXG_ERR;
		dhcp_started = true;
	} else {
		if (nx_dhcp_reinitialize(&eth_dhcp) != NX_SUCCESS)
			return NXG_ERR;
		nx_dhcp_start(&eth_dhcp);
	}
	return NXG_OK;
}

int nx_net_ping(uint32_t ip, unsigned timeout_ms, unsigned *rtt_ms)
{
	NX_PACKET *resp = NX_NULL;
	ULONG t0;
	UINT rc;

	if (!nx_up)
		return NXG_ERR_STATE;

	t0 = tx_time_get();
	rc = nx_icmp_ping(&eth_ip, (ULONG)ip, "nx_glue_ping", 12, &resp,
	                  (ULONG)timeout_ms);    /* NX_IP_PERIODIC_RATE=1000 -> ms    */
	if (rc == NX_SUCCESS) {
		if (rtt_ms != NULL)
			*rtt_ms = (unsigned)(tx_time_get() - t0);
		nx_packet_release(resp);
		return NXG_OK;
	}
	if (resp != NX_NULL)
		nx_packet_release(resp);
	return (rc == NX_NO_RESPONSE) ? NXG_TIMEOUT : NXG_ERR;
}
