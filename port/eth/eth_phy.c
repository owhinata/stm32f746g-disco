/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    eth_phy.c
 * @brief   Clean-room LAN8742A PHY link driver (issue #49 P1).  See eth_phy.h.
 *
 * Freestanding: no HAL, no ThreadX -- all MDIO access goes through the injected
 * read/write vtable.  The LAN8742 register numbers/bits below are clause-22
 * standard (BCR/BSR/PHYIDR) plus the one vendor register PHYSCSR(0x1F); they were
 * taken from the LAN8742A datasheet and the ST BSP lan8742.h as a map reference
 * only (no code reuse).
 */
#include "eth_phy.h"

/* IEEE 802.3 clause-22 registers. */
#define PHY_BCR      0x00u   /* Basic Control Register   */
#define PHY_BSR      0x01u   /* Basic Status Register    */
#define PHY_IDR1     0x02u   /* PHY Identifier 1         */
#define PHY_IDR2     0x03u   /* PHY Identifier 2         */

/* BCR bits. */
#define PHY_BCR_RESET            0x8000u
#define PHY_BCR_AUTONEG_EN       0x1000u
#define PHY_BCR_AUTONEG_RESTART  0x0200u

/* BSR bits. */
#define PHY_BSR_AUTONEG_DONE     0x0020u
#define PHY_BSR_LINK_UP          0x0004u

/* LAN8742 PHY Special Control/Status Register (0x1F): HCDSPEED[4:2] gives the
   auto-negotiation-resolved speed/duplex, valid only once AUTONEGO_DONE is set. */
#define PHY_SCSR                 0x1Fu
#define PHY_SCSR_AUTONEG_DONE    0x1000u
#define PHY_SCSR_HCDSPEED_MASK   0x001Cu
#define PHY_SCSR_10M_HALF        0x0004u
#define PHY_SCSR_10M_FULL        0x0014u
#define PHY_SCSR_100M_HALF       0x0008u
#define PHY_SCSR_100M_FULL       0x0018u

/* BCR.RESET self-clears once the PHY finishes its reset.  Each poll is an MDIO
   read (itself bounded by the MAC's MACMIIAR busy timeout), so a bounded retry
   count is also a bounded wall-clock wait (a LAN8742 reset completes well within
   a few ms). */
#define PHY_RESET_POLL_MAX  1000u

static int phy_rd(struct eth_phy *phy, uint32_t reg, uint16_t *val)
{
	return phy->read(phy->ctx, phy->addr, reg, val);
}

static int phy_wr(struct eth_phy *phy, uint32_t reg, uint16_t val)
{
	return phy->write(phy->ctx, phy->addr, reg, val);
}

int eth_phy_init(struct eth_phy *phy)
{
	uint16_t v;
	int rc;

	/* Discover the PHY: scan all 32 addresses for a valid PHYIDR1 (a populated
	   PHY answers with its OUI; an empty address floats to 0x0000 or 0xFFFF).
	   Track whether *any* MDIO read succeeded so a dead bus (every read errors)
	   is reported as ETH_PHY_ERR_IO, distinct from "no PHY answered". */
	phy->addr = 0;
	phy->id1 = 0;
	phy->id2 = 0;
	bool found = false;
	bool any_io = false;
	for (uint32_t a = 0; a < 32u; a++) {
		uint16_t id1;
		if (phy->read(phy->ctx, a, PHY_IDR1, &id1) != 0)
			continue;
		any_io = true;
		if (id1 != 0x0000u && id1 != 0xFFFFu) {
			phy->addr = a;
			phy->id1 = id1;
			if (phy->read(phy->ctx, a, PHY_IDR2, &phy->id2) != 0)
				return ETH_PHY_ERR_IO;
			found = true;
			break;
		}
	}
	if (!found)
		return any_io ? ETH_PHY_ERR_NODEV : ETH_PHY_ERR_IO;

	/* Soft reset, then poll BCR.RESET until the PHY clears it. */
	rc = phy_wr(phy, PHY_BCR, PHY_BCR_RESET);
	if (rc != 0)
		return ETH_PHY_ERR_IO;
	for (uint32_t i = 0; i < PHY_RESET_POLL_MAX; i++) {
		rc = phy_rd(phy, PHY_BCR, &v);
		if (rc != 0)
			return ETH_PHY_ERR_IO;
		if ((v & PHY_BCR_RESET) == 0)
			goto reset_done;
	}
	return ETH_PHY_ERR_RESET;

reset_done:
	/* Enable + restart auto-negotiation.  Do not block on completion. */
	rc = phy_wr(phy, PHY_BCR, PHY_BCR_AUTONEG_EN | PHY_BCR_AUTONEG_RESTART);
	if (rc != 0)
		return ETH_PHY_ERR_IO;
	return ETH_PHY_OK;
}

int eth_phy_restart_autoneg(struct eth_phy *phy)
{
	if (phy_wr(phy, PHY_BCR, PHY_BCR_AUTONEG_EN | PHY_BCR_AUTONEG_RESTART) != 0)
		return ETH_PHY_ERR_IO;
	return ETH_PHY_OK;
}

int eth_phy_get_link(struct eth_phy *phy, struct eth_phy_link *out)
{
	uint16_t bsr, scsr;

	out->up = false;
	out->speed = ETH_PHY_SPEED_NONE;
	out->duplex = ETH_PHY_DUPLEX_NONE;

	/* BSR link bit latches low: the first read returns the latched value, the
	   second the present state.  Use the second. */
	if (phy_rd(phy, PHY_BSR, &bsr) != 0)
		return ETH_PHY_ERR_IO;
	if (phy_rd(phy, PHY_BSR, &bsr) != 0)
		return ETH_PHY_ERR_IO;
	if ((bsr & PHY_BSR_LINK_UP) == 0)
		return ETH_PHY_OK;   /* link down -- speed/duplex stay NONE */

	out->up = true;

	/* Resolve speed/duplex from the LAN8742 PHYSCSR HCDSPEED field -- but only
	   once auto-negotiation has finished.  While it is still running the link
	   reads up but the speed/duplex are not yet valid, so leave them NONE. */
	if (phy_rd(phy, PHY_SCSR, &scsr) != 0)
		return ETH_PHY_ERR_IO;
	if ((scsr & PHY_SCSR_AUTONEG_DONE) == 0)
		return ETH_PHY_OK;
	switch (scsr & PHY_SCSR_HCDSPEED_MASK) {
	case PHY_SCSR_100M_FULL:
		out->speed = ETH_PHY_SPEED_100M; out->duplex = ETH_PHY_DUPLEX_FULL; break;
	case PHY_SCSR_100M_HALF:
		out->speed = ETH_PHY_SPEED_100M; out->duplex = ETH_PHY_DUPLEX_HALF; break;
	case PHY_SCSR_10M_FULL:
		out->speed = ETH_PHY_SPEED_10M;  out->duplex = ETH_PHY_DUPLEX_FULL; break;
	case PHY_SCSR_10M_HALF:
		out->speed = ETH_PHY_SPEED_10M;  out->duplex = ETH_PHY_DUPLEX_HALF; break;
	default:
		/* Up but auto-neg not resolved yet: leave speed/duplex NONE. */
		break;
	}
	return ETH_PHY_OK;
}
