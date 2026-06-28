/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    eth_phy.h
 * @brief   Clean-room LAN8742A Ethernet PHY link driver (issue #49 P1).
 *
 * A minimal IEEE-802.3 clause-22 PHY driver for the board's LAN8742A: address
 * discovery, soft reset, auto-negotiation, and link/speed/duplex read.  It is
 * freestanding -- it touches NO HAL and NO ThreadX.  All MDIO register access is
 * injected as a read/write vtable (the glue in eth_link.c wires it to
 * HAL_ETH_{Read,Write}PHYRegister over the ETH MAC's SMI/MDIO), so this module is
 * host-unit-testable and independent of the MAC driver, in the @ref frame_os /
 * @ref cli_transport idiom.
 *
 * Only the few registers needed for link bring-up are used: BCR(0) for reset and
 * auto-neg control, BSR(1) for the (latched-low) link bit, PHYIDR1/2(2,3) for the
 * 22-bit OUI + model id, and the LAN8742-specific Special Control/Status
 * register (0x1F) whose HCDSPEED field gives the resolved speed/duplex after
 * auto-negotiation.  We deliberately do NOT vendor ST's lan8742.c: it adds an
 * unconditional 2-second busy wait in its init (LAN8742_INIT_TO) that would stall
 * boot, and this project keeps PHY/peripheral glue clean-room (the ST BSP
 * lan8742.h was a register-map reference only).
 */
#ifndef ETH_PHY_H
#define ETH_PHY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error returns (negative); 0 is success. */
#define ETH_PHY_OK         0
#define ETH_PHY_ERR_IO    -1   /* an MDIO read/write returned an error          */
#define ETH_PHY_ERR_NODEV -2   /* no PHY answered on any of the 32 addresses     */
#define ETH_PHY_ERR_RESET -3   /* BCR soft-reset did not self-clear in time      */

enum eth_phy_speed  { ETH_PHY_SPEED_NONE = 0, ETH_PHY_SPEED_10M, ETH_PHY_SPEED_100M };
enum eth_phy_duplex { ETH_PHY_DUPLEX_NONE = 0, ETH_PHY_DUPLEX_HALF, ETH_PHY_DUPLEX_FULL };

/** Resolved link state (filled by eth_phy_get_link). */
struct eth_phy_link {
	bool                up;      /**< BSR link-status bit (current, not latched) */
	enum eth_phy_speed  speed;   /**< resolved only when up                      */
	enum eth_phy_duplex duplex;  /**< resolved only when up                      */
};

/**
 * Injected MDIO bus.  @p addr is the 5-bit PHY address, @p reg the clause-22
 * register (0..31).  read/write return 0 on success, <0 on a bus error.  The
 * glue wires these to the ETH MAC's MDIO; a host test wires them to a mock.
 */
struct eth_phy {
	void *ctx;
	int (*read)(void *ctx, uint32_t addr, uint32_t reg, uint16_t *val);
	int (*write)(void *ctx, uint32_t addr, uint32_t reg, uint16_t val);

	/* Filled by eth_phy_init(). */
	uint32_t addr;   /**< discovered PHY address (0..31)        */
	uint16_t id1;    /**< PHYIDR1: OUI bits 3..18               */
	uint16_t id2;    /**< PHYIDR2: OUI 19..24 + model + revision */
};

/**
 * Discover the PHY address (scan 0..31 for a valid PHYIDR1), soft-reset it
 * (BCR.RESET, polled until self-clear), and enable + restart auto-negotiation.
 * Does NOT wait for the link to come up (auto-neg can take seconds; poll later
 * with eth_phy_get_link).  @p phy->{ctx,read,write} must be set by the caller.
 * Returns ETH_PHY_OK, or a negative ETH_PHY_ERR_* (phy->addr/id* are valid only
 * on success).
 */
int eth_phy_init(struct eth_phy *phy);

/** Re-trigger auto-negotiation (BCR auto-neg enable + restart). */
int eth_phy_restart_autoneg(struct eth_phy *phy);

/**
 * Read the current link state.  Reads BSR twice (the link bit latches low, so
 * the second read reflects the present state) and, when up, decodes the
 * LAN8742 PHYSCSR HCDSPEED field into speed/duplex.  Returns ETH_PHY_OK or
 * ETH_PHY_ERR_IO.
 */
int eth_phy_get_link(struct eth_phy *phy, struct eth_phy_link *out);

#ifdef __cplusplus
}
#endif

#endif /* ETH_PHY_H */
