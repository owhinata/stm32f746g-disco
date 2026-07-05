/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nx_eth_driver.c
 * @brief   Clean-room NetX Duo network driver over the STM32 ETH MAC (#49 P2).
 *
 * See nx_eth_driver.h.  Zero-copy: RX buffers and TX payloads are NX_PACKET
 * payloads from one MPU non-cacheable pool, handed straight to the ETH DMA (the
 * HAL does no D-cache maintenance).  USE_HAL_ETH_REGISTER_CALLBACKS is 0, so the
 * RxAllocate/RxLink/TxFree hooks are strong overrides of the HAL weak symbols
 * (single ETH instance -> global is fine).  The ETH ISR only posts the NetX
 * deferred event; the drain runs in the IP helper thread (NX_LINK_DEFERRED_PROCESSING).
 *
 * Clean-room: NetX nx_api.h contract + new HAL_ETH API only.  ST's
 * nx_stm32_eth_driver.c (Azure RTOS EULA) was NOT used.
 */
#include "nx_api.h"
#include "nx_ip.h"
#include "nx_arp.h"
#include "nx_rarp.h"
#include "nx_packet.h"

#include "nx_eth_driver.h"
#include "eth_link.h"

#include "stm32f7xx_hal.h"

#define LOG_TAG "neth"
#include "log.h"

#include <string.h>

#define ETHTYPE_IP    0x0800u
#define ETHTYPE_ARP   0x0806u
#define ETHTYPE_RARP  0x8035u
#define ETH_HDR_SIZE  14u

/* Offset the RX frame 2 bytes into the (32-byte aligned) packet payload so that,
   after the 14-byte Ethernet header is stripped, the IPv4 header lands 4-byte
   aligned (data_start + 16) -- symmetric with TX, where NetX's 16-byte
   NX_PHYSICAL_HEADER headroom puts the IP header at data_start + 16.  The pool
   payload (NXG_PAYLOAD in nx_glue.c) is sized to absorb this pad. */
#define RX_FRAME_PAD  2u

/* TX coalesce scratch for the rare NX_PACKET chain with > ETH_TX_DESC_CNT
   fragments (never in P2: ARP/ICMP/DHCP are single-fragment).  Non-cacheable
   (.sdram.eth) since the ETH DMA reads it; one in-flight (P2 has no concurrent
   multi-fragment TX -- documented). */
static uint8_t tx_coalesce[1600]
	__attribute__((aligned(32), section(".sdram.eth")));

static struct {
	NX_IP             *ip;
	NX_INTERFACE      *iface;
	UINT               iface_index;
	NX_PACKET_POOL    *rx_pool;
	ETH_HandleTypeDef *heth;

	ULONG              mac_msw, mac_lsw;

	ULONG              pkt_off;        /* NX_PACKET base -> payload offset       */
	int                pkt_off_valid;

	volatile int       started;       /* stack NX_LINK_ENABLE'd                 */
	volatile int       mac_running;   /* HAL_ETH_Start_IT done                  */
	volatile int       phy_up;        /* last PHY link state (eth-link thread)  */
	int                link_known;    /* a PHY poll has been seen (first event) */
	int                phy_mbps, phy_fd;
	int                applied_mbps, applied_fd;

	struct nx_eth_stats st;
} g;

/* ---- HAL weak-callback overrides (RX alloc/link, TX free) ----------------- */

void HAL_ETH_RxAllocateCallback(uint8_t **buff)
{
	NX_PACKET *p;

	if (_nx_packet_allocate(g.rx_pool, &p, NX_RECEIVE_PACKET, NX_NO_WAIT) != NX_SUCCESS) {
		*buff = NULL;             /* back-pressure: descriptor not re-armed      */
		g.st.rx_no_buf++;
		return;
	}
	*buff = p->nx_packet_prepend_ptr + RX_FRAME_PAD;   /* 2-byte align pad (above) */
}

void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length)
{
	/* Recover the owning NX_PACKET: buff == payload_start + RX_FRAME_PAD. */
	NX_PACKET *p = (NX_PACKET *)(void *)(buff - RX_FRAME_PAD - g.pkt_off);

	if (!g.pkt_off_valid || p->nx_packet_pool_owner != g.rx_pool) {
		g.st.rx_drop++;            /* should never happen (uniform pool layout)  */
		return;
	}
	p->nx_packet_prepend_ptr = buff;            /* frame start (payload + 2)      */
	p->nx_packet_append_ptr  = buff + Length;
	p->nx_packet_length      = Length;
	p->nx_packet_next        = NX_NULL;

	if (*pStart == NX_NULL)
		*pStart = p;
	else
		((NX_PACKET *)*pEnd)->nx_packet_next = p;
	*pEnd = p;
}

/* Undo the 14-byte Ethernet header this driver prepended for TX (issue #79).
 * _nx_packet_transmit_release() only rewinds NetX's own headers (it adds back
 * nx_packet_ip_header_length for a retained TCP packet), so the L2 header is the
 * driver's responsibility to strip.  Without this, a retransmitted TCP packet's
 * prepend_ptr stays 14 bytes (2 mod 4) low, and _nx_ip_header_add() then writes
 * the re-added IP header words at an unaligned address -> UNALIGNED UsageFault.
 * Only the head packet of a chain carries the Ethernet header. */
static void eth_tx_unprepend(NX_PACKET *p)
{
	p->nx_packet_prepend_ptr += ETH_HDR_SIZE;
	p->nx_packet_length      -= ETH_HDR_SIZE;
}

void HAL_ETH_TxFreeCallback(uint32_t *buff)
{
	/* buff == ETH_TxPacketConfig.pData == the transmitted NX_PACKET.  The DMA has
	   finished with the header bytes; strip the L2 header from the metadata before
	   release so a retained TCP packet rewinds to its TCP header for retransmit. */
	NX_PACKET *p = (NX_PACKET *)(void *)buff;

	eth_tx_unprepend(p);
	_nx_packet_transmit_release(p);
	g.st.tx_ok++;
}

/* ---- ISR: post the deferred event only ------------------------------------ */

void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *heth)
{
	(void)heth;
	if (g.ip)
		_nx_ip_driver_deferred_processing(g.ip);
}

void HAL_ETH_TxCpltCallback(ETH_HandleTypeDef *heth)
{
	(void)heth;
	if (g.ip)
		_nx_ip_driver_deferred_processing(g.ip);   /* reclaim in deferred ctx    */
}

void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *heth)
{
	(void)heth;
	g.st.dma_err++;
}

void ETH_IRQHandler(void)
{
	if (g.heth)
		HAL_ETH_IRQHandler(g.heth);
}

/* ---- RX drain + TX reclaim (NetX IP helper thread context) ---------------- */

static void eth_deferred(NX_IP *ip)
{
	NX_PACKET *p;

	for (;;) {
		HAL_StatusTypeDef st;

		eth_lock_acquire();
		st = HAL_ETH_ReadData(g.heth, (void **)&p);   /* re-arms via UpdateDescriptor */
		eth_lock_release();
		if (st != HAL_OK || p == NX_NULL)
			break;

		/* Ethernet header is at the front of the first fragment. */
		uint8_t *eh = p->nx_packet_prepend_ptr;
		UINT etype = ((UINT)eh[12] << 8) | eh[13];
		p->nx_packet_ip_interface = g.iface;

		if (etype == ETHTYPE_IP) {
			p->nx_packet_prepend_ptr += ETH_HDR_SIZE;
			p->nx_packet_length      -= ETH_HDR_SIZE;
			g.st.rx_ok++;
			_nx_ip_packet_receive(ip, p);             /* already in thread ctx   */
		} else if (etype == ETHTYPE_ARP) {
			p->nx_packet_prepend_ptr += ETH_HDR_SIZE;
			p->nx_packet_length      -= ETH_HDR_SIZE;
			g.st.rx_ok++;
			_nx_arp_packet_deferred_receive(ip, p);
		} else if (etype == ETHTYPE_RARP) {
			p->nx_packet_prepend_ptr += ETH_HDR_SIZE;
			p->nx_packet_length      -= ETH_HDR_SIZE;
			_nx_rarp_packet_deferred_receive(ip, p);
		} else {
			g.st.rx_drop++;
			_nx_packet_release(p);
		}
	}

	/* Reclaim completed TX packets (drives HAL_ETH_TxFreeCallback). */
	eth_lock_acquire();
	HAL_ETH_ReleaseTxPacket(g.heth);
	eth_lock_release();
}

/* ---- TX ------------------------------------------------------------------- */

static void eth_tx(NX_IP_DRIVER *req)
{
	NX_PACKET *pkt = req->nx_ip_driver_packet;
	UINT cmd = req->nx_ip_driver_command;
	USHORT etype = (cmd == NX_LINK_ARP_SEND || cmd == NX_LINK_ARP_RESPONSE_SEND)
	                   ? ETHTYPE_ARP
	             : (cmd == NX_LINK_RARP_SEND) ? ETHTYPE_RARP
	                                          : ETHTYPE_IP;
	ETH_BufferTypeDef txb[ETH_TX_DESC_CNT];
	ETH_TxPacketConfigTypeDef cfg;
	HAL_StatusTypeDef st;
	ULONG dmsw, dlsw;
	uint8_t *eh;
	int frags = 0;

	if (!g.mac_running) {            /* link down / not started -> drop          */
		g.st.tx_drop++;
		_nx_packet_transmit_release(pkt);
		return;
	}

	/* Prepend the 14-byte Ethernet header (NX_PHYSICAL_HEADER reserved the room). */
	pkt->nx_packet_prepend_ptr -= ETH_HDR_SIZE;
	pkt->nx_packet_length      += ETH_HDR_SIZE;
	eh   = pkt->nx_packet_prepend_ptr;
	dmsw = req->nx_ip_driver_physical_address_msw;
	dlsw = req->nx_ip_driver_physical_address_lsw;
	eh[0] = (uint8_t)(dmsw >> 8);  eh[1] = (uint8_t)dmsw;
	eh[2] = (uint8_t)(dlsw >> 24); eh[3] = (uint8_t)(dlsw >> 16);
	eh[4] = (uint8_t)(dlsw >> 8);  eh[5] = (uint8_t)dlsw;
	eh[6] = (uint8_t)(g.mac_msw >> 8);  eh[7] = (uint8_t)g.mac_msw;
	eh[8] = (uint8_t)(g.mac_lsw >> 24); eh[9] = (uint8_t)(g.mac_lsw >> 16);
	eh[10] = (uint8_t)(g.mac_lsw >> 8); eh[11] = (uint8_t)g.mac_lsw;
	eh[12] = (uint8_t)(etype >> 8); eh[13] = (uint8_t)etype;

	for (NX_PACKET *q = pkt; q != NX_NULL; q = q->nx_packet_next)
		frags++;

	memset(&cfg, 0, sizeof cfg);
	if (frags <= ETH_TX_DESC_CNT) {
		int n = 0;
		for (NX_PACKET *f = pkt; f != NX_NULL; f = f->nx_packet_next, n++) {
			txb[n].buffer = f->nx_packet_prepend_ptr;
			txb[n].len    = (uint32_t)(f->nx_packet_append_ptr - f->nx_packet_prepend_ptr);
			txb[n].next   = (f->nx_packet_next != NX_NULL) ? &txb[n + 1] : NULL;
		}
		cfg.TxBuffer = &txb[0];
	} else {
		/* Coalesce an over-long chain into the scratch buffer (rare). */
		uint8_t *d = tx_coalesce;
		if (pkt->nx_packet_length > sizeof tx_coalesce) {
			g.st.tx_drop++;
			eth_tx_unprepend(pkt);          /* #79: restore before release */
			_nx_packet_transmit_release(pkt);
			return;
		}
		for (NX_PACKET *f = pkt; f != NX_NULL; f = f->nx_packet_next) {
			ULONG l = (ULONG)(f->nx_packet_append_ptr - f->nx_packet_prepend_ptr);
			memcpy(d, f->nx_packet_prepend_ptr, l);
			d += l;
		}
		txb[0].buffer = tx_coalesce;
		txb[0].len    = pkt->nx_packet_length;
		txb[0].next   = NULL;
		cfg.TxBuffer  = &txb[0];
	}
	cfg.Length     = pkt->nx_packet_length;
	cfg.Attributes = ETH_TX_PACKETS_FEATURES_CRCPAD;
	cfg.CRCPadCtrl = ETH_CRC_PAD_INSERT;

	/* HW TX checksum insertion (issue #98).  NetX flags, per packet, which
	   checksums it offloaded (leaving those header fields 0); map that to the
	   TDES0 CIC field.  Always set CSUM so ChecksumCtrl is written on every
	   transmit -- the descriptors are init'd with CIC=FULL and DMA write-back
	   does not restore it, so leaving CIC untouched would be non-deterministic.
	   ETH_CHECKSUM_DISABLE (0) then means "HW must not touch the SW-computed
	   field" for ARP/RARP and any non-offloaded protocol. */
	cfg.Attributes |= ETH_TX_PACKETS_FEATURES_CSUM;
	cfg.ChecksumCtrl = ETH_CHECKSUM_DISABLE;
#ifdef NX_ENABLE_INTERFACE_CAPABILITY
	{
		ULONG capf = pkt->nx_packet_interface_capability_flag;

		if (capf & (NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM |
		            NX_INTERFACE_CAPABILITY_UDP_TX_CHECKSUM |
		            NX_INTERFACE_CAPABILITY_ICMPV4_TX_CHECKSUM))
			/* IP header + payload + pseudo-header all in HW (CIC=11).  Payload
			   offload always covers the IP header too, so NetX also zeroed it. */
			cfg.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
		else if (capf & NX_INTERFACE_CAPABILITY_IPV4_TX_CHECKSUM)
			/* IP header only (CIC=01): e.g. an IP fragment, whose payload
			   checksum the MAC would bypass anyway. */
			cfg.ChecksumCtrl = ETH_CHECKSUM_IPHDR_INSERT;
	}
#endif
	cfg.pData      = pkt;            /* TxFree releases this NX_PACKET            */

	eth_lock_acquire();
	st = HAL_ETH_Transmit_IT(g.heth, &cfg);
	eth_lock_release();

	if (st != HAL_OK) {
		g.st.tx_drop++;
		eth_tx_unprepend(pkt);              /* #79: restore before release */
		_nx_packet_transmit_release(pkt);   /* TxFree won't run on a failed send */
	}
	/* success: the packet is released later by HAL_ETH_TxFreeCallback. */
}

/* ---- link-driven MAC start/stop (eth-link thread context) ----------------- */

static void apply_mac_state(void)        /* call under eth_lock */
{
	int want = g.started && g.phy_up;

	if (want && (!g.mac_running ||
	             g.applied_mbps != g.phy_mbps || g.applied_fd != g.phy_fd)) {
		ETH_MACConfigTypeDef mc;

		if (g.mac_running) {         /* reconfigure: must be READY for SetMACConfig */
			HAL_NVIC_DisableIRQ(ETH_IRQn);
			HAL_ETH_Stop_IT(g.heth);
			g.mac_running = 0;
		}
		if (HAL_ETH_GetMACConfig(g.heth, &mc) == HAL_OK) {
			mc.Speed      = (g.phy_mbps == 100) ? ETH_SPEED_100M : ETH_SPEED_10M;
			mc.DuplexMode = g.phy_fd ? ETH_FULLDUPLEX_MODE : ETH_HALFDUPLEX_MODE;
			HAL_ETH_SetMACConfig(g.heth, &mc);
		}
		if (HAL_ETH_Start_IT(g.heth) == HAL_OK) {
			HAL_NVIC_EnableIRQ(ETH_IRQn);
			g.mac_running  = 1;
			g.applied_mbps = g.phy_mbps;
			g.applied_fd   = g.phy_fd;
		}
	} else if (!want && g.mac_running) {
		HAL_NVIC_DisableIRQ(ETH_IRQn);
		HAL_ETH_Stop_IT(g.heth);
		g.mac_running = 0;
	}
}

static void nx_eth_on_link(void *arg, bool up, int mbps, bool full_duplex)
{
	int changed;

	(void)arg;
	eth_lock_acquire();
	/* Force an event on the very first poll so NetX learns the real link state
	   (its IP thread init optimistically set nx_interface_link_up = TRUE). */
	changed = !g.link_known || (up != g.phy_up) ||
	          (mbps != g.phy_mbps) || (full_duplex != g.phy_fd);
	g.link_known = 1;
	g.phy_up   = up;
	g.phy_mbps = mbps;
	g.phy_fd   = full_duplex;
	apply_mac_state();
	eth_lock_release();

	if (g.ip != NX_NULL) {
		if (changed)
			_nx_ip_driver_link_status_event(g.ip, g.iface_index);
		/* Every poll: kick deferred processing as a packet-pool-starvation
		   watchdog (HAL_ETH_ReadData re-arms starved descriptors). */
		_nx_ip_driver_deferred_processing(g.ip);
	}
}

/* ---- NetX driver entry ---------------------------------------------------- */

VOID nx_eth_driver(NX_IP_DRIVER *req)
{
	NX_IP        *ip    = req->nx_ip_driver_ptr;
	NX_INTERFACE *iface = req->nx_ip_driver_interface;

	req->nx_ip_driver_status = NX_SUCCESS;

	switch (req->nx_ip_driver_command) {
	case NX_LINK_INTERFACE_ATTACH:
		g.iface = iface;
		break;

	case NX_LINK_INITIALIZE: {
		uint8_t mac[6];

		g.ip          = ip;
		g.iface       = iface;
		g.iface_index = iface->nx_interface_index;
		g.heth        = (ETH_HandleTypeDef *)eth_get_handle();
		if (g.heth == NULL) {
			req->nx_ip_driver_status = NX_NOT_ENABLED;
			break;
		}
		eth_get_mac(mac);
		g.mac_msw = ((ULONG)mac[0] << 8) | mac[1];
		g.mac_lsw = ((ULONG)mac[2] << 24) | ((ULONG)mac[3] << 16) |
		            ((ULONG)mac[4] << 8) | mac[5];

		nx_ip_interface_mtu_set(ip, g.iface_index, 1500);
		nx_ip_interface_physical_address_set(ip, g.iface_index,
		                                     g.mac_msw, g.mac_lsw, NX_FALSE);
		nx_ip_interface_address_mapping_configure(ip, g.iface_index, NX_TRUE);

		/* Report the MAC's TX checksum insertion (issue #98).  Set the flag
		   directly here rather than via nx_ip_interface_capability_set() from the
		   glue: NX_LINK_INITIALIZE runs on the IP thread *after* it clears the
		   capability flag to 0 (nx_ip_thread_entry.c), so a glue-side set would
		   race that clear.  IPv4-header + TCP only (see eth_tx CIC mapping); no
		   UDP/ICMP payload (UDP zero-checksum special case unverified) and no RX. */
#ifdef NX_ENABLE_INTERFACE_CAPABILITY
		iface->nx_interface_capability_flag =
		        NX_INTERFACE_CAPABILITY_IPV4_TX_CHECKSUM |
		        NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM;
#endif
		/* NetX optimistically set link_up = TRUE; start from the real (down)
		   state until the first PHY poll reports up (see nx_eth_on_link). */
		iface->nx_interface_link_up = NX_FALSE;

		HAL_NVIC_SetPriority(ETH_IRQn, 7, 0);   /* enabled when the MAC starts   */
		eth_link_set_callback(nx_eth_on_link, ip);
		break;
	}

	case NX_LINK_ENABLE:
		eth_lock_acquire();
		g.started = 1;
		apply_mac_state();
		eth_lock_release();
		break;

	case NX_LINK_DISABLE:
		eth_lock_acquire();
		g.started = 0;
		apply_mac_state();
		eth_lock_release();
		break;

	case NX_LINK_PACKET_SEND:
	case NX_LINK_PACKET_BROADCAST:
	case NX_LINK_ARP_SEND:
	case NX_LINK_ARP_RESPONSE_SEND:
	case NX_LINK_RARP_SEND:
		eth_tx(req);
		break;

	case NX_LINK_DEFERRED_PROCESSING:
		eth_deferred(ip);
		break;

	case NX_LINK_GET_STATUS:
		*req->nx_ip_driver_return_ptr = (ULONG)(g.phy_up ? NX_TRUE : NX_FALSE);
		break;
	case NX_LINK_GET_SPEED:
		*req->nx_ip_driver_return_ptr = (ULONG)g.phy_mbps;
		break;
	case NX_LINK_GET_DUPLEX_TYPE:
		*req->nx_ip_driver_return_ptr = (ULONG)(g.phy_fd ? NX_TRUE : NX_FALSE);
		break;

	case NX_LINK_MULTICAST_JOIN:
	case NX_LINK_MULTICAST_LEAVE:
		/* P2 needs only unicast + broadcast (ARP/ICMP/DHCP); the MAC accepts
		   those by default.  Hash-filter multicast is deferred to P5 if needed. */
		break;

	case NX_LINK_UNINITIALIZE:
		eth_lock_acquire();
		g.started = 0;
		apply_mac_state();
		eth_lock_release();
		break;

	default:
		req->nx_ip_driver_status = NX_UNHANDLED_COMMAND;
		break;
	}
}

/* ---- glue helpers --------------------------------------------------------- */

VOID nx_eth_driver_set_pool(NX_PACKET_POOL *pool)
{
	NX_PACKET *probe;

	g.rx_pool = pool;
	/* Measure the (constant) NX_PACKET base -> payload offset once, for
	   zero-copy buffer<->packet recovery in HAL_ETH_RxLinkCallback. */
	if (_nx_packet_allocate(pool, &probe, NX_RECEIVE_PACKET, NX_NO_WAIT) == NX_SUCCESS) {
		g.pkt_off = (ULONG)((uint8_t *)probe->nx_packet_prepend_ptr - (uint8_t *)probe);
		g.pkt_off_valid = 1;
		_nx_packet_release(probe);
	}
}

VOID nx_eth_driver_get_stats(struct nx_eth_stats *out)
{
	*out = g.st;
}
