/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    sdram.c
 * @brief   FMC SDRAM bring-up for the on-board MT48LC4M32B2 (issue #40).
 *
 * See sdram.h for the API contract.  Hardware setup:
 *
 *   - FMC SDRAM bank 1 at 0xC0000000.  The chip is 4M x 32 (128 Mbit) but the
 *     board wires only DQ0-DQ15 (UM1907 §6.13), so the controller is set to a
 *     16-bit data width: 4 banks x 4096 rows x 256 columns x 2 B = 8 MB.
 *   - SDCLK = HCLK/2 = 108 MHz (FMC_SDRAM_CLOCK_PERIOD_2), CAS latency 3.
 *     The ST BSP uses CAS2, but its comment assumes a 100 MHz SD clock
 *     (HCLK 200 MHz); this firmware runs HCLK = 216 MHz, and the
 *     MT48LC4M32B2-6 rates CL2 only up to 100 MHz (tCK(2) >= 10 ns) while
 *     CL3 is rated to 167 MHz (tCK(3) >= 6 ns) -- so 108 MHz requires CAS3.
 *     Read sampling margin is in fact better than the BSP's: tAC(3) <= 5.4 ns
 *     against a 9.26 ns period vs the BSP's tAC(2) <= 7.5 ns against 10 ns,
 *     so RBURST/RPIPE_0 carry over unchanged.
 *   - Pins (UM1907 Table 13, all AF12, pull-up like the BSP): PC3 (SDCKE0),
 *     PD0,1,8,9,10,14,15 / PE0,1,7..15 (data + NBL0/1), PF0..5,11..15 and
 *     PG0,1,4,5 (address + BA), PG8 (SDCLK), PF11 (SDNRAS), PG15 (SDNCAS),
 *     PH3 (SDNE0), PH5 (SDNWE).
 *   - Refresh: COUNT = 0x0603 (BSP value, computed for 100 MHz).  At 108 MHz
 *     it refreshes every ~14.4 us/row instead of the required 15.625 us --
 *     early refresh is always safe, only marginally costing bandwidth.
 *
 * The JEDEC power-up sequence (clock enable -> >=100 us -> precharge-all ->
 * 8 auto-refresh cycles -> load mode register -> refresh counter) runs on
 * HAL_SDRAM_SendCommand, which polls the FMC busy flag.  The 100 us wait uses
 * udelay (svc/timebase TIM2 busy-wait) because sdram_init() runs before the
 * ThreadX scheduler/tick is live.
 *
 * Mode register: burst length 1, sequential, CAS 3, standard operation,
 * single-location writes -- the device CAS must match the controller's
 * CASLatency (no read bursts from the memory side are needed; the FMC
 * re-orders via RBURST).
 *
 * Cache/MPU: this driver does not touch the MPU.  bsp_init() maps the 8 MB
 * region Normal non-cacheable before the D-cache is enabled (src/bsp.c), so
 * everything here -- and every later DMA -- is coherent by construction.
 *
 * Clean-room implementation; ST BSP / RM0385 §13 used as reference only.
 */
#include "sdram.h"
#include "timebase.h"

#include "stm32f7xx_hal.h"

#define LOG_TAG "sdram"
#include "log.h"

/* MT48LC4M32B2 mode register fields (datasheet). */
#define MODEREG_BURST_LEN_1     0x0000u
#define MODEREG_BURST_SEQ       0x0000u
#define MODEREG_CAS_3           0x0030u
#define MODEREG_OP_STANDARD     0x0000u
#define MODEREG_WBURST_SINGLE   0x0200u

/* 64 ms / 4096 rows at ~100 MHz SDCLK (ST BSP REFRESH_COUNT). */
#define SDRAM_REFRESH_COUNT     0x0603u

/* HAL_SDRAM_SendCommand busy-poll ceiling (HAL ticks / ms). */
#define SDRAM_CMD_TIMEOUT       0xFFFFu

static SDRAM_HandleTypeDef hsdram;

static int sdram_up;        /* init succeeded                 */
static int sdram_tried;     /* init ran (idempotence latch)   */

int sdram_is_up(void)
{
	return sdram_up;
}

static void sdram_gpio_init(void)
{
	GPIO_InitTypeDef g = {0};

	__HAL_RCC_GPIOC_CLK_ENABLE();
	__HAL_RCC_GPIOD_CLK_ENABLE();
	__HAL_RCC_GPIOE_CLK_ENABLE();
	__HAL_RCC_GPIOF_CLK_ENABLE();
	__HAL_RCC_GPIOG_CLK_ENABLE();
	__HAL_RCC_GPIOH_CLK_ENABLE();

	g.Mode      = GPIO_MODE_AF_PP;
	g.Pull      = GPIO_PULLUP;
	g.Speed     = GPIO_SPEED_FREQ_HIGH;
	g.Alternate = GPIO_AF12_FMC;

	g.Pin = GPIO_PIN_3;                                       /* SDCKE0 */
	HAL_GPIO_Init(GPIOC, &g);

	g.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_8 | GPIO_PIN_9 |
	        GPIO_PIN_10 | GPIO_PIN_14 | GPIO_PIN_15;          /* D2,D3,D13..D15,D0,D1 */
	HAL_GPIO_Init(GPIOD, &g);

	g.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_7 | GPIO_PIN_8 |
	        GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 |
	        GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;          /* NBL0/1, D4..D12 */
	HAL_GPIO_Init(GPIOE, &g);

	g.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 |
	        GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_11 | GPIO_PIN_12 |
	        GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;          /* A0..A5, SDNRAS, A6..A9 */
	HAL_GPIO_Init(GPIOF, &g);

	g.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5 |
	        GPIO_PIN_8 | GPIO_PIN_15;                         /* A10,A11,BA0,BA1,SDCLK,SDNCAS */
	HAL_GPIO_Init(GPIOG, &g);

	g.Pin = GPIO_PIN_3 | GPIO_PIN_5;                          /* SDNE0, SDNWE */
	HAL_GPIO_Init(GPIOH, &g);
}

/* JEDEC SDRAM power-up command sequence (RM0385 §13.7.3, MT48LC4M32B2 DS). */
static int sdram_powerup_sequence(void)
{
	FMC_SDRAM_CommandTypeDef cmd = {0};
	uint32_t mrd;

	/* 1: start the SDCLK to the device. */
	cmd.CommandMode            = FMC_SDRAM_CMD_CLK_ENABLE;
	cmd.CommandTarget          = FMC_SDRAM_CMD_TARGET_BANK1;
	cmd.AutoRefreshNumber      = 1;
	cmd.ModeRegisterDefinition = 0;
	if (HAL_SDRAM_SendCommand(&hsdram, &cmd, SDRAM_CMD_TIMEOUT) != HAL_OK)
		return SDRAM_ERR_HAL;

	/* 2: >= 100 us before the first command (datasheet power-up).  Busy-wait
	   on TIM2 -- the ThreadX tick is not running yet. */
	udelay(200);

	/* 3: precharge all banks. */
	cmd.CommandMode = FMC_SDRAM_CMD_PALL;
	if (HAL_SDRAM_SendCommand(&hsdram, &cmd, SDRAM_CMD_TIMEOUT) != HAL_OK)
		return SDRAM_ERR_HAL;

	/* 4: 8 auto-refresh cycles. */
	cmd.CommandMode       = FMC_SDRAM_CMD_AUTOREFRESH_MODE;
	cmd.AutoRefreshNumber = 8;
	if (HAL_SDRAM_SendCommand(&hsdram, &cmd, SDRAM_CMD_TIMEOUT) != HAL_OK)
		return SDRAM_ERR_HAL;

	/* 5: load the device mode register (CAS must match the controller). */
	mrd = MODEREG_BURST_LEN_1 | MODEREG_BURST_SEQ | MODEREG_CAS_3 |
	      MODEREG_OP_STANDARD | MODEREG_WBURST_SINGLE;
	cmd.CommandMode            = FMC_SDRAM_CMD_LOAD_MODE;
	cmd.AutoRefreshNumber      = 1;
	cmd.ModeRegisterDefinition = mrd;
	if (HAL_SDRAM_SendCommand(&hsdram, &cmd, SDRAM_CMD_TIMEOUT) != HAL_OK)
		return SDRAM_ERR_HAL;

	/* 6: refresh rate counter (FMC_SDRTR). */
	if (HAL_SDRAM_ProgramRefreshRate(&hsdram, SDRAM_REFRESH_COUNT) != HAL_OK)
		return SDRAM_ERR_HAL;

	return SDRAM_OK;
}

int sdram_init(void)
{
	FMC_SDRAM_TimingTypeDef timing;

	if (sdram_tried)
		return sdram_up ? SDRAM_OK : SDRAM_ERR_STATE;
	sdram_tried = 1;

	sdram_gpio_init();
	__HAL_RCC_FMC_CLK_ENABLE();

	/* Timings in SDCLK cycles at 108 MHz (9.26 ns) against the
	   MT48LC4M32B2-6 datasheet minimums.  NOT the raw BSP set: the BSP
	   numbers assume a 100 MHz SDCLK (10 ns), where TXSR=7 and TRAS=4
	   suffice; at 9.26 ns they would fall short (64.8 < 67 ns, 37 < 42 ns),
	   so both gain a cycle:
	     TMRD 2                  >= tMRD  2 cyc
	     TXSR 8 -> 74.1 ns       >= tXSR  67 ns
	     TRAS 5 -> 46.3 ns       >= tRAS  42 ns
	     TRC  7 -> 64.8 ns       >= tRC   60 ns
	     TWR  2 -> 18.5 ns       >= tWR   1 cyc + 6 ns
	     TRP  2 -> 18.5 ns       >= tRP   18 ns
	     TRCD 2 -> 18.5 ns       >= tRCD  18 ns */
	timing.LoadToActiveDelay    = 2;   /* TMRD */
	timing.ExitSelfRefreshDelay = 8;   /* TXSR */
	timing.SelfRefreshTime      = 5;   /* TRAS */
	timing.RowCycleDelay        = 7;   /* TRC  */
	timing.WriteRecoveryTime    = 2;   /* TWR  */
	timing.RPDelay              = 2;   /* TRP  */
	timing.RCDDelay             = 2;   /* TRCD */

	hsdram.Instance                = FMC_SDRAM_DEVICE;
	hsdram.Init.SDBank             = FMC_SDRAM_BANK1;
	hsdram.Init.ColumnBitsNumber   = FMC_SDRAM_COLUMN_BITS_NUM_8;
	hsdram.Init.RowBitsNumber      = FMC_SDRAM_ROW_BITS_NUM_12;
	hsdram.Init.MemoryDataWidth    = FMC_SDRAM_MEM_BUS_WIDTH_16;
	hsdram.Init.InternalBankNumber = FMC_SDRAM_INTERN_BANKS_NUM_4;
	hsdram.Init.CASLatency         = FMC_SDRAM_CAS_LATENCY_3;  /* 108 MHz > CL2's 100 MHz rating */
	hsdram.Init.WriteProtection    = FMC_SDRAM_WRITE_PROTECTION_DISABLE;
	hsdram.Init.SDClockPeriod      = FMC_SDRAM_CLOCK_PERIOD_2;  /* 108 MHz */
	hsdram.Init.ReadBurst          = FMC_SDRAM_RBURST_ENABLE;
	hsdram.Init.ReadPipeDelay      = FMC_SDRAM_RPIPE_DELAY_0;

	if (HAL_SDRAM_Init(&hsdram, &timing) != HAL_OK) {
		LOG_ERR("FMC SDRAM controller init failed");
		return SDRAM_ERR_HAL;
	}

	if (sdram_powerup_sequence() != SDRAM_OK) {
		LOG_ERR("SDRAM power-up command sequence failed");
		return SDRAM_ERR_HAL;
	}

	sdram_up = 1;
	LOG_INF("8 MB up at 0xC0000000 (16-bit, CAS3, SDCLK 108 MHz, non-cacheable)");
	return SDRAM_OK;
}
