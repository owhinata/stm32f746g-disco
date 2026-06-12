/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    timebase.c
 * @brief   TIM2 free-running time source + microsecond busy-wait (issue #43).
 *
 * Split out of bsp.c so the freestanding svc/ layer owns the TIM2 timebase and
 * udelay(); port/ drivers take the microsecond delay from here instead of
 * reaching up into bsp.h.  Pure register access -- no HAL_Init() dependency.
 */
#include "timebase.h"

#include "stm32f7xx_hal.h"

/**
 * @brief  TIM2 as a free-running 32-bit time source for the ThreadX Execution
 *         Profile Kit (issue #19: `thread` cpu% column).
 *
 * TIM2CLK = 2*PCLK1 = 108 MHz (APB1 /4, TIMPRE = 0), so the counter advances at
 * ~9.26 ns/count and wraps every ~39.77 s -- far longer than any single
 * enter/exit or 1 ms idle interval, which is all the kit measures between ticks.
 *
 * Chosen over the kit default DWT_CYCCNT: DWT freezes when the core clock is
 * gated by WFI (#20), whereas TIM2 keeps its clock in Sleep (TIM2LPEN reset = 1)
 * and so keeps counting -- idle/cpu% stay correct once WFI is enabled.  No
 * interrupt is used; the kit just reads TIM2->CNT (see tx_user.h).  Started after
 * SystemClock_Config() so PCLK1 is final, and before tx_kernel_enter() so the
 * source is live when _tx_execution_initialize() samples it.
 */
void timebase_init(void)
{
	__HAL_RCC_TIM2_CLK_ENABLE();
	TIM2->PSC = 0u;             /* full 108 MHz resolution */
	TIM2->ARR = 0xFFFFFFFFu;    /* 32-bit free-run */
	TIM2->CNT = 0u;
	TIM2->EGR = TIM_EGR_UG;     /* latch PSC/ARR */
	TIM2->CR1 = TIM_CR1_CEN;    /* up-count, no interrupt */
}

/**
 * @brief  Busy-wait @p us microseconds on the free-running TIM2 counter.
 *
 * TIM2 runs at 108 MHz (set up in timebase_init for the ThreadX execution
 * profile, issue #19), so 108 counts == 1 us.  Unsigned 32-bit subtraction makes
 * the wait wrap-safe as long as the delay is shorter than TIM2's wrap period
 * (~39.77 s); the `usleep` command caps @p us far below that.  Pure busy loop --
 * it does NOT yield, so keep delays short (issue #21).  Interrupts (SysTick,
 * UART) still run, so the ThreadX tick and higher-priority threads are unaffected.
 */
void udelay(uint32_t us)
{
	uint32_t start = TIM2->CNT;
	uint32_t ticks = us * 108u;            /* 108 TIM2 counts per microsecond */

	while ((TIM2->CNT - start) < ticks)
		;
}
