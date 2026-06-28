/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    eth_link.c
 * @brief   ETH MAC + LAN8742A RMII link bring-up (issue #49 P1).  See eth_link.h.
 *
 * Clean-room glue: the RMII MspInit (GPIO/clock) is done inline before
 * HAL_ETH_Init (the project's port idiom -- the HAL weak MspInit stays a no-op),
 * the PHY is driven through the freestanding eth_phy.c over the MAC's MDIO, and a
 * single low-priority thread polls the link and logs transitions.  No
 * HAL_ETH_Start here (P2): the MAC stays in READY, so no DMA runs and no ETH IRQ
 * fires, and the TX/RX descriptors in `.sdram.eth` are only touched by
 * HAL_ETH_Init (CPU writes) -- never by DMA -- in P1.
 */
#include "eth_link.h"

#include "eth_phy.h"

#include "stm32f7xx_hal.h"

#define LOG_TAG "eth"
#include "log.h"

#include "tx_api.h"

/* `eth-link` monitor thread: below CLI(16), above nothing it must preempt; it
   sleeps ~200 ms between PHY polls so its duty is negligible.  Stack 1 KB (it
   only reads a handful of PHY registers and logs). */
#define ETH_LINK_PRIORITY    15
#define ETH_LINK_STACK_SIZE  1024
#define ETH_LINK_POLL_MS     200          /* tx tick = 1 ms (TX_TIMER_TICKS=1000) */

/* RX buffer length handed to HAL (one Ethernet frame, 32 B aligned).  The buffer
   pool itself is P2 -- P1 never starts the MAC, so no RX buffers are allocated;
   HAL_ETH_Init only records this length. */
#define ETH_RX_BUF_LEN       1536u

/* ETH TX/RX DMA descriptors.  The new HAL_ETH does no D-cache maintenance, so
   they live in `.sdram.eth` (FMC bank2, MPU Normal non-cacheable -- coherent by
   construction), 32 B aligned (one Cortex-M7 cache line). */
static ETH_DMADescTypeDef g_tx_desc[ETH_TX_DESC_CNT]
	__attribute__((aligned(32), section(".sdram.eth")));
static ETH_DMADescTypeDef g_rx_desc[ETH_RX_DESC_CNT]
	__attribute__((aligned(32), section(".sdram.eth")));

static ETH_HandleTypeDef heth;
static struct eth_phy    phy;
static uint8_t           g_mac[6];

static TX_MUTEX  eth_lock;            /* serializes HAL_ETH/MDIO access          */
static bool      eth_up;              /* eth_init() succeeded                    */
static struct eth_link_info shared;   /* identity (init) + live link (thread)    */

static TX_THREAD eth_link_thread;
static UCHAR     eth_link_stack[ETH_LINK_STACK_SIZE];

/* ---- MDIO vtable wired to the MAC's SMI (injected into eth_phy) ----------- */

static int eth_mdio_read(void *ctx, uint32_t addr, uint32_t reg, uint16_t *val)
{
	uint32_t v = 0;
	if (HAL_ETH_ReadPHYRegister((ETH_HandleTypeDef *)ctx, addr, reg, &v) != HAL_OK)
		return -1;
	*val = (uint16_t)v;
	return 0;
}

static int eth_mdio_write(void *ctx, uint32_t addr, uint32_t reg, uint16_t val)
{
	if (HAL_ETH_WritePHYRegister((ETH_HandleTypeDef *)ctx, addr, reg, val) != HAL_OK)
		return -1;
	return 0;
}

/* ---- speed/duplex mapping (PHY enum -> public enum) ----------------------- */

static enum eth_speed map_speed(enum eth_phy_speed s)
{
	switch (s) {
	case ETH_PHY_SPEED_100M: return ETH_LINK_SPEED_100M;
	case ETH_PHY_SPEED_10M:  return ETH_LINK_SPEED_10M;
	default:                 return ETH_LINK_SPEED_NONE;
	}
}

static enum eth_duplex map_duplex(enum eth_phy_duplex d)
{
	switch (d) {
	case ETH_PHY_DUPLEX_FULL: return ETH_LINK_DUPLEX_FULL;
	case ETH_PHY_DUPLEX_HALF: return ETH_LINK_DUPLEX_HALF;
	default:                  return ETH_LINK_DUPLEX_NONE;
	}
}

static const char *speed_str(enum eth_phy_speed s)
{
	return (s == ETH_PHY_SPEED_100M) ? "100M" : (s == ETH_PHY_SPEED_10M) ? "10M" : "?";
}

static const char *duplex_str(enum eth_phy_duplex d)
{
	return (d == ETH_PHY_DUPLEX_FULL) ? "full" : (d == ETH_PHY_DUPLEX_HALF) ? "half" : "?";
}

/* ---- RMII GPIO + clocks (inline MspInit, before HAL_ETH_Init) ------------- */

static void eth_gpio_clock_init(void)
{
	GPIO_InitTypeDef g = {0};

	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOC_CLK_ENABLE();
	__HAL_RCC_GPIOG_CLK_ENABLE();

	/* All 10 RMII signals: AF11_ETH, push-pull, no pull, very-high speed
	   (RM0385 §38.3 Table 277; ST F746G-DISCO ethernetif.c HAL_ETH_MspInit). */
	g.Mode      = GPIO_MODE_AF_PP;
	g.Pull      = GPIO_NOPULL;
	g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
	g.Alternate = GPIO_AF11_ETH;

	g.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7;     /* REF_CLK, MDIO, CRS_DV   */
	HAL_GPIO_Init(GPIOA, &g);
	g.Pin = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;     /* MDC, RXD0, RXD1         */
	HAL_GPIO_Init(GPIOC, &g);
	g.Pin = GPIO_PIN_2 | GPIO_PIN_11 | GPIO_PIN_13 | GPIO_PIN_14; /* RXER,TXEN,TXD0,TXD1 */
	HAL_GPIO_Init(GPIOG, &g);

	/* MAC/TX/RX clocks (RMII select + SYSCFG clock are done inside HAL_ETH_Init). */
	__HAL_RCC_ETH_CLK_ENABLE();
}

/* ---- link monitor thread -------------------------------------------------- */

static void eth_link_entry(ULONG arg)
{
	bool last_up = false;

	(void)arg;
	for (;;) {
		struct eth_phy_link l;
		int rc;

		tx_thread_sleep(ETH_LINK_POLL_MS);

		tx_mutex_get(&eth_lock, TX_WAIT_FOREVER);
		rc = eth_phy_get_link(&phy, &l);
		if (rc == ETH_PHY_OK) {
			shared.up     = l.up;
			shared.speed  = map_speed(l.speed);
			shared.duplex = map_duplex(l.duplex);
		}
		tx_mutex_put(&eth_lock);

		if (rc == ETH_PHY_OK && l.up != last_up) {
			if (l.up)
				LOG_INF("link up %s %s", speed_str(l.speed), duplex_str(l.duplex));
			else
				LOG_INF("link down");
			last_up = l.up;
		}
	}
}

/* ---- public API ----------------------------------------------------------- */

int eth_init(void)
{
	uint32_t uid;

	if (eth_up)
		return ETH_OK;     /* idempotent */

	if (tx_mutex_create(&eth_lock, "eth", TX_INHERIT) != TX_SUCCESS)
		return ETH_ERR_HAL;

	eth_gpio_clock_init();

	/* Locally-administered, unicast MAC (first byte 0x02: bit1 = local admin,
	   bit0 = 0 unicast), uniquified from the full STM32 96-bit UID (all three
	   words folded, UID_BASE = 0x1FF0F420) so two boards on one LAN do not
	   collide.  Link-only P1 does not transmit; this is in place for P2. */
	uid = HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2();
	g_mac[0] = 0x02; g_mac[1] = 0x00; g_mac[2] = 0x00;
	g_mac[3] = (uint8_t)(uid >> 16);
	g_mac[4] = (uint8_t)(uid >> 8);
	g_mac[5] = (uint8_t)uid;

	heth.Instance            = ETH;
	heth.Init.MACAddr        = g_mac;
	heth.Init.MediaInterface = HAL_ETH_RMII_MODE;
	heth.Init.TxDesc         = g_tx_desc;
	heth.Init.RxDesc         = g_rx_desc;
	heth.Init.RxBuffLen      = ETH_RX_BUF_LEN;
	if (HAL_ETH_Init(&heth) != HAL_OK) {
		LOG_ERR("HAL_ETH_Init failed");
		tx_mutex_delete(&eth_lock);
		return ETH_ERR_HAL;
	}

	HAL_ETH_SetMDIOClockRange(&heth);

	phy.ctx   = &heth;
	phy.read  = eth_mdio_read;
	phy.write = eth_mdio_write;
	if (eth_phy_init(&phy) != ETH_PHY_OK) {
		LOG_ERR("PHY init failed");
		HAL_ETH_DeInit(&heth);
		tx_mutex_delete(&eth_lock);
		return ETH_ERR_PHY;
	}

	for (int i = 0; i < 6; i++)
		shared.mac[i] = g_mac[i];
	shared.phy_addr = phy.addr;
	shared.phy_id1  = phy.id1;
	shared.phy_id2  = phy.id2;
	shared.up       = false;
	shared.speed    = ETH_LINK_SPEED_NONE;
	shared.duplex   = ETH_LINK_DUPLEX_NONE;

	if (tx_thread_create(&eth_link_thread, "eth-link", eth_link_entry, 0,
	                     eth_link_stack, sizeof eth_link_stack,
	                     ETH_LINK_PRIORITY, ETH_LINK_PRIORITY,
	                     TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS) {
		LOG_ERR("eth-link thread create failed");
		HAL_ETH_DeInit(&heth);
		tx_mutex_delete(&eth_lock);
		return ETH_ERR_HAL;
	}

	eth_up = true;
	LOG_INF("ETH up (RMII); PHY@%lu %04x:%04x; MAC %02x:%02x:%02x:%02x:%02x:%02x",
	        (unsigned long)phy.addr, phy.id1, phy.id2,
	        g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
	return ETH_OK;
}

bool eth_is_initialized(void)
{
	return eth_up;
}

int eth_link_get(struct eth_link_info *out)
{
	if (out == NULL)
		return ETH_ERR_STATE;
	if (!eth_up)
		return ETH_ERR_STATE;

	tx_mutex_get(&eth_lock, TX_WAIT_FOREVER);
	*out = shared;
	tx_mutex_put(&eth_lock);
	return ETH_OK;
}

int eth_link_renegotiate(void)
{
	int rc;

	if (!eth_up)
		return ETH_ERR_STATE;

	tx_mutex_get(&eth_lock, TX_WAIT_FOREVER);
	rc = eth_phy_restart_autoneg(&phy);
	tx_mutex_put(&eth_lock);
	return (rc == ETH_PHY_OK) ? ETH_OK : ETH_ERR_PHY;
}
