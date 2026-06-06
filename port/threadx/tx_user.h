/*
 * tx_user.h - ThreadX build-time configuration for STM32F746G-DISCO.
 *
 * Included by the Cortex-M7 GNU port assembly (unconditionally) and by the C
 * core when TX_INCLUDE_USER_DEFINE_FILE is defined (set in the Makefile).
 */
#ifndef TX_USER_H
#define TX_USER_H

/* ThreadX tick rate. The SysTick handler (tx_glue.c) calls
   _tx_timer_interrupt() every TX_GLUE_TICK_DIV ms, so the effective tick rate
   is 1000 / TX_GLUE_TICK_DIV Hz. Keep this define in sync with that rate.
   Default: 1 kHz (1 tick = 1 ms). Override with -DTX_TIMER_TICKS_PER_SECOND. */
#ifndef TX_TIMER_TICKS_PER_SECOND
#define TX_TIMER_TICKS_PER_SECOND  1000
#endif

#endif /* TX_USER_H */
