/*
 * tx_user.h - ThreadX build-time configuration for STM32F746G-DISCO.
 *
 * Included by the Cortex-M7 GNU port assembly (unconditionally) and by the C
 * core when TX_INCLUDE_USER_DEFINE_FILE is defined (set in the Makefile).
 */
#ifndef TX_USER_H
#define TX_USER_H

/* SysTick is driven at 1 kHz by the HAL timebase, and the SysTick handler
   calls _tx_timer_interrupt() once per ms, so one ThreadX tick == 1 ms. */
#define TX_TIMER_TICKS_PER_SECOND  1000

#endif /* TX_USER_H */
