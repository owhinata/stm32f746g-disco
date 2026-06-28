/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nx_glue.h
 * @brief   NetX Duo IPv4 bring-up + diagnostics facade (issue #49 P2).
 *
 * Owns the single NetX IP instance over the STM32 ETH MAC: the shared
 * non-cacheable packet pool, the IP thread, ARP/ICMP/UDP/TCP, and the DHCP
 * client (boot default).  Exposes a thin facade so the shell (cmd_net.c) never
 * includes nx_api.h directly -- it stays in the shell layer of HAL<-svc<-port<-
 * ui<-shell.  All addresses are host byte order (a.b.c.d -> (a<<24)|...).
 */
#ifndef NX_GLUE_H
#define NX_GLUE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXG_OK         0
#define NXG_ERR_STATE -1   /* NetX not up                                       */
#define NXG_ERR       -2   /* a NetX call failed                                */
#define NXG_TIMEOUT   -3   /* ping: no reply                                    */

/**
 * One-time bring-up from tx_application_define() (after eth_init()): NetX system
 * init, the shared pool (.sdram.eth), the IP instance + IP thread, ARP/ICMP/UDP/
 * TCP, and the DHCP client (created; started by the link-up callback).  Only
 * creates ThreadX objects (no blocking), so it is safe before the scheduler --
 * DHCP negotiation runs later on the IP thread.  Returns NXG_OK or a negative.
 */
int  nx_net_init(void);

/** Nonzero once nx_net_init() created the IP instance. */
bool nx_net_is_up(void);

/** Current address / mode snapshot for `net info`. */
struct nx_net_info {
	bool        ip_valid;     /**< a non-zero address is assigned               */
	bool        dhcp_mode;    /**< DHCP client owns the address (vs static)     */
	uint32_t    ip, mask, gw; /**< host byte order                              */
};
int  nx_net_info_get(struct nx_net_info *out);

/** Switch to a static address (stops DHCP). */
int  nx_net_set_static(uint32_t ip, uint32_t mask, uint32_t gw);

/** (Re)acquire an address via DHCP (leaves static mode). */
int  nx_net_dhcp_renew(void);

/**
 * ICMP echo @p ip once, up to @p timeout_ms.  On a reply returns NXG_OK and sets
 * @p rtt_ms; NXG_TIMEOUT on no reply; NXG_ERR/NXG_ERR_STATE otherwise.
 */
int  nx_net_ping(uint32_t ip, unsigned timeout_ms, unsigned *rtt_ms);

/** The NetX IP instance (NX_IP*), opaque to non-NetX callers -- the TCP echo
 *  server (P3) / future sockets cast it back.  NULL before nx_net_init(). */
void *nx_net_ip(void);

/** The shared non-cacheable packet pool (NX_PACKET_POOL*), for backends that
 *  allocate TX packets (the network shell, P4).  NULL before nx_net_init(). */
void *nx_net_pool(void);

#ifdef __cplusplus
}
#endif

#endif /* NX_GLUE_H */
