/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    iwdg.c
 * @brief   IWDG independent watchdog driver (issue #38).
 *
 * The IWDG is clocked by the LSI (~32 kHz typ, 17-47 kHz over tolerance),
 * independent of the HSE+PLL 216 MHz tree, so it keeps counting even if the main
 * clock or the ThreadX scheduler stalls.  prescaler /64 + reload 1499 gives
 * T = 64 * (1499 + 1) / f_LSI: ~3.0 s at the typical LSI and 2.04 s at the 47 kHz
 * fast corner.  A priority-5 petter thread (src/main.c) refreshes every ~1 s
 * (<= T/2 at every LSI corner), so it survives CoreMark (~12 s in the priority-16
 * shell thread) without any extra pet -- the petter preempts it.
 *
 * The whole file compiles to nothing when BSP_ENABLE_IWDG == 0, so no IWDG symbol
 * (and no LSI dependency) reaches the image (issue #38 acceptance: `nm`).
 *
 * Clean-room design; no third-party code reused.
 */
#include "iwdg.h"

#if BSP_ENABLE_IWDG

#include "stm32f7xx_hal.h"

#define LOG_TAG "iwdg"
#include "log.h"

static IWDG_HandleTypeDef hiwdg;
static uint8_t            g_iwdg_init_failed;

void iwdg_init(void)
{
	hiwdg.Instance       = IWDG;
	hiwdg.Init.Prescaler = IWDG_PRESCALER_64;
	hiwdg.Init.Reload    = 1499u;             /* ~3.0 s @32 kHz, 2.04 s @47 kHz */
	hiwdg.Init.Window    = IWDG_WINDOW_DISABLE;

	/* HAL_IWDG_Init() starts the IWDG (KR=0xCCCC) and then polls the PR/RLR update
	   (SR PVU/RVU); a failure here does NOT prove the watchdog is unarmed, so flag
	   it for `wdt info` ("init failed (may be armed)") and keep going.  Never halt:
	   the petter still refreshes, and an unrefreshed watchdog would only reset us. */
	if (HAL_IWDG_Init(&hiwdg) != HAL_OK) {
		g_iwdg_init_failed = 1u;
		LOG_ERR("HAL_IWDG_Init failed (may be armed)");
	}
}

void iwdg_refresh(void)
{
	HAL_IWDG_Refresh(&hiwdg);   /* == IWDG->KR = 0xAAAA; a single register write */
}

int iwdg_init_failed(void)
{
	return (int)g_iwdg_init_failed;
}

#endif /* BSP_ENABLE_IWDG */
