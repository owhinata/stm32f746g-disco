/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    timebase.h
 * @brief   TIM2 free-running time source + microsecond busy-wait (svc/ layer).
 *
 * TIM2 is set up as a free-running 32-bit counter at TIM2CLK = 2*PCLK1 = 108 MHz
 * (the ThreadX Execution Profile Kit reads TIM2->CNT directly, issue #19) and
 * doubles as the source for the udelay() busy-wait.  Split out of bsp.c (issue
 * #43) so port/ drivers (e.g. sdram) get the microsecond delay from the
 * freestanding svc/ layer instead of reaching up into bsp.h.
 */
#ifndef TIMEBASE_H
#define TIMEBASE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start TIM2 as a free-running 32-bit time source for the ThreadX Execution
 * Profile Kit (issue #19) and the udelay() busy-wait.  Call from bsp_init()
 * AFTER SystemClock_Config() (so PCLK1 is final) and BEFORE tx_kernel_enter()
 * (so the source is live when the kit first samples it).
 */
void timebase_init(void);

/**
 * Busy-wait @p us microseconds on the free-running TIM2 counter (108 MHz, set up
 * by timebase_init()).  Does NOT yield -- short delays only; the `usleep`
 * command caps it (issue #21).
 */
void udelay(uint32_t us);

#ifdef __cplusplus
}
#endif

#endif /* TIMEBASE_H */
