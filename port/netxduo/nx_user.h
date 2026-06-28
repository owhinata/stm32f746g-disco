/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nx_user.h
 * @brief   NetX Duo build configuration for this project (issue #49 P2).
 *
 * Pulled in by every NetX Duo translation unit via the Cortex-M7/GNU port
 * (ports/cortex_m7/gnu/inc/nx_port.h includes this when NX_INCLUDE_USER_DEFINE_FILE
 * is defined -- the same mechanism as ThreadX tx_user.h / FileX fx_user.h / GUIX
 * gx_user.h).  nx_port.h includes us BEFORE applying its own #ifndef defaults, so
 * every value here wins.
 *
 * DHCP-specific knobs (NX_DHCP_THREAD_PRIORITY, NX_DHCP_CLIENT_USER_CREATE_PACKET_POOL)
 * are passed as -D on the build target instead (they only affect the addon).
 */
#ifndef NX_USER_H
#define NX_USER_H

/*
 * ★ CRITICAL: the Cortex-M7/GNU nx_port.h hard-defaults NX_IP_PERIODIC_RATE to
 * 100, but this project's ThreadX runs at 1000 Hz (port/threadx/tx_user.h,
 * TX_TIMER_TICKS_PER_SECOND = 1000, 1 tick = 1 ms).  NetX derives ALL of its time
 * bases (ARP retransmit/aging, TCP timeouts, ICMP, DHCP lease/renew) from this
 * rate, so leaving it at 100 makes every NetX timer run 10x slow.  It MUST match
 * the ThreadX tick rate.  After this override, wait_option values are in ms.
 */
#define NX_IP_PERIODIC_RATE          1000

/* IPv4-only.  Neutralises every *ipv6* / *icmpv6* / *_nd_* source file (they are
 * wrapped in #ifdef FEATURE_NX_IPV6, which nx_api.h only sets when this is unset),
 * so they compile to empty objects and --gc-sections drops them. */
#define NX_DISABLE_IPV6

/* Ethernet L2 framing room reserved in each packet: 14-byte MAC header padded to
 * 16 (keeps the IP header 32-bit aligned), 4-byte trailer (FCS). */
#define NX_PHYSICAL_HEADER           16
#define NX_PHYSICAL_TRAILER          4

/* Align packet header + payload to a Cortex-M7 cache line.  The shared pool lives
 * in MPU non-cacheable SDRAM and its payloads are handed straight to the ETH MAC
 * DMA (zero-copy driver), so 32-byte alignment keeps DMA buffers cleanly placed. */
#define NX_PACKET_ALIGNMENT          32

/* The ETH ISR only sets a deferred event; the NetX IP helper thread runs the
 * driver bottom-half (RX drain / TX reclaim) via NX_LINK_DEFERRED_PROCESSING. */
#define NX_DRIVER_DEFERRED_PROCESSING

#endif /* NX_USER_H */
