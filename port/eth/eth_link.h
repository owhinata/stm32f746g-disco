/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    eth_link.h
 * @brief   On-board Ethernet (ETH MAC + LAN8742A RMII) link bring-up (issue #49 P1).
 *
 * Phase 1 of the Ethernet epic (#49): bring up the STM32F746 ETH MAC over RMII,
 * register the LAN8742A PHY (eth_phy.c), and detect/report link state.  No
 * NetX/LwIP, no actual traffic -- this proves the RMII pins, the 50 MHz REF_CLK,
 * the DMA-descriptor placement and the D-cache policy before the stack lands in
 * P2.  The MAC is initialised to HAL_ETH_STATE_READY but NOT started
 * (HAL_ETH_Start is P2), so no ETH DMA runs and no ETH IRQ fires here.
 *
 * Memory: the new HAL_ETH does NO D-cache maintenance, so the TX/RX DMA
 * descriptors live in the `.sdram.eth` section (FMC bank2, 0xC0400000) which
 * bsp_init() maps Normal non-cacheable through the MPU -- coherent by
 * construction, exactly like the camera/LTDC buffers.  Because HAL_ETH_Init
 * writes those descriptors immediately, eth_init() must only run when the FMC is
 * up; src/main.c gates the call on sdram_is_up() (touching 0xC0400000 with the
 * FMC down would fault).
 *
 * Bring-up is non-blocking: eth_init() does the clock/GPIO, HAL_ETH_Init, PHY
 * reset and auto-neg enable (a few ms) and then spawns the low-priority
 * `eth-link` thread which polls the PHY (~5 Hz) and logs link up/down
 * transitions to the dmesg ring.  It never waits for the link to come up, so it
 * is safe from tx_application_define() before the scheduler / IWDG arm.
 *
 * Clean-room implementation; RM0385 §38 (ETH) / UM1907 §6.12 (RMII wiring) and
 * the ST F746G-DISCO LwIP ethernetif.c were used as a register/pin reference
 * only.
 */
#ifndef ETH_LINK_H
#define ETH_LINK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error returns (negative); 0 is success. */
#define ETH_OK          0
#define ETH_ERR_HAL    -1   /* a HAL_ETH init step failed                       */
#define ETH_ERR_STATE  -2   /* eth_init() did not succeed (driver not up)       */
#define ETH_ERR_PHY    -3   /* PHY discovery / reset failed                     */

/* NOTE: prefixed ETH_LINK_* -- the HAL ETH driver already defines ETH_SPEED_10M
   / ETH_SPEED_100M (MACCR field values) and main.c sees both headers. */
enum eth_speed  { ETH_LINK_SPEED_NONE = 0, ETH_LINK_SPEED_10M, ETH_LINK_SPEED_100M };
enum eth_duplex { ETH_LINK_DUPLEX_NONE = 0, ETH_LINK_DUPLEX_HALF, ETH_LINK_DUPLEX_FULL };

/** Snapshot returned by eth_link_get(): the static identity (MAC/PHY) plus the
 *  live link state maintained by the `eth-link` thread. */
struct eth_link_info {
	uint8_t         mac[6];      /**< the MAC address programmed into the ETH   */
	uint32_t        phy_addr;    /**< discovered PHY address (0..31)            */
	uint16_t        phy_id1;     /**< PHYIDR1                                   */
	uint16_t        phy_id2;     /**< PHYIDR2                                   */
	bool            up;          /**< current link state                       */
	enum eth_speed  speed;       /**< resolved when up                         */
	enum eth_duplex duplex;      /**< resolved when up                         */
};

/**
 * One-time bring-up: eth_lock mutex, RMII GPIO + ETH/GPIO clocks, HAL_ETH_Init
 * (RMII select + MAC reset + descriptor chain, internal to HAL), MDIO clock
 * range, LAN8742 discovery/reset/auto-neg, then the `eth-link` monitor thread.
 * The MAC is left in READY (not started -- that is P2).  REQUIRES the FMC SDRAM
 * to be up (the descriptors live at 0xC0400000); the caller must gate on
 * sdram_is_up().  Idempotent; returns ETH_OK or a negative ETH_ERR_*.
 */
int eth_init(void);

/** Nonzero once eth_init() succeeded.  This is the driver-initialised state, NOT
 *  the link state (use eth_link_get for that) -- `net info` stays useful with
 *  the cable unplugged. */
bool eth_is_initialized(void);

/**
 * Copy the current link/identity snapshot into @p out under eth_lock.  Returns
 * ETH_OK, or ETH_ERR_STATE if eth_init() never succeeded.
 */
int eth_link_get(struct eth_link_info *out);

/**
 * Re-trigger PHY auto-negotiation (for `net link`).  Returns ETH_OK, ETH_ERR_STATE
 * (not initialised) or ETH_ERR_PHY (MDIO error).
 */
int eth_link_renegotiate(void);

/* ---- P2 (#75) hooks: the NetX ETH driver layers on top of this MAC/PHY -----
 * These keep eth_link.c free of any NetX/HAL-handle leakage to its other callers
 * (cmd_net.c only uses the link API above): the NetX driver (port/netxduo) gets
 * the ETH handle as an opaque pointer, the MAC bytes, the shared HAL lock, and a
 * link-change callback -- dependency inversion (eth_link.c never includes NetX),
 * mirroring guix_register_app_builder(). */

/**
 * Link-change/poll callback registered by the NetX driver.  Invoked from the
 * `eth-link` monitor thread on EVERY successful PHY poll (~5 Hz): the driver uses
 * it to (a) reconfigure the MAC speed/duplex and start/stop the DMA on a real
 * transition, and (b) kick NetX deferred processing as a packet-pool-starvation
 * watchdog.  @p up / @p speed_mbps (0/10/100) / @p full_duplex are the PHY state.
 */
typedef void (*eth_link_cb_t)(void *arg, bool up, int speed_mbps, bool full_duplex);
void eth_link_set_callback(eth_link_cb_t cb, void *arg);

/** The ETH_HandleTypeDef* (opaque here to avoid pulling the HAL into non-HAL
 *  callers); the NetX driver casts it back.  NULL before eth_init() succeeds. */
void *eth_get_handle(void);

/** Copy the 6-byte MAC address programmed into the ETH MAC. */
void eth_get_mac(uint8_t out[6]);

/** Acquire/release the shared HAL_ETH serialization lock (eth_lock).  The NetX
 *  driver wraps every HAL_ETH state-changing call (Start/Stop/Transmit/ReadData)
 *  in these so MDIO polling, link start/stop and the data path never overlap. */
void eth_lock_acquire(void);
void eth_lock_release(void);

#ifdef __cplusplus
}
#endif

#endif /* ETH_LINK_H */
