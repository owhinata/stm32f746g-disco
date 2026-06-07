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

/* ThreadX Execution Profile Kit (issue #19: `thread` cpu% column).
 *
 * Enabling this here (rather than via CMake -D) keeps the define in one place so
 * every translation unit that sees TX_THREAD agrees on its layout: the kit adds
 * tx_thread_execution_time_total / _last_start to TX_THREAD (tx_api.h), and
 * tx_api.h auto-includes tx_execution_profile.h, both only when this is defined.
 * tx_user.h is included by the port asm (unconditionally) and by the C core +
 * shell_obj (via TX_INCLUDE_USER_DEFINE_FILE), so all of them stay ABI-consistent.
 */
#define TX_EXECUTION_PROFILE_ENABLE

/* Use the Cortex-M execution-profile path (nest counter) for ISR accounting.
 * Mandatory here: this port's TX_THREAD_GET_SYSTEM_STATE() ORs in the IPSR
 * (tx_port.h), so inside an ISR it is never == 1; the non-EPK "== 1" guard in
 * tx_execution_profile.c would drop all ISR time.  The EPK path guards on
 * "truthy && nest_counter == 1", which works when our plain-C ISRs
 * (SysTick_Handler / USART1_IRQHandler) call _tx_execution_isr_enter/exit. */
#define TX_CORTEX_M_EPK

/* Time source for the execution profile = TIM2->CNT (0x40000024): APB1, 32-bit,
 * free-running at TIM2CLK = 2*PCLK1 = 108 MHz (wrap ~39.77 s).  TIM2 is started
 * in bsp.c (exec_timebase_init).  Chosen over the kit default DWT_CYCCNT because
 * DWT freezes when the core clock is gated by WFI (#20): TIM2 keeps counting in
 * Sleep (TIM2LPEN reset = 1), so idle/cpu% stay correct once WFI is enabled.
 * TX_EXECUTION_MAX_TIME_SOURCE keeps its 0xFFFFFFFF default (32-bit). */
#define TX_EXECUTION_TIME_SOURCE \
    ((EXECUTION_TIME_SOURCE_TYPE)(*(volatile ULONG *)0x40000024UL))

/* Enable WFI in the scheduler idle loop (issue #20).  When no thread is ready,
 * the Cortex-M7 port inserts DSB; WFI; ISB instead of busy-spinning
 * (tx_thread_schedule.S __tx_ts_wait), so the core sleeps until an interrupt.
 *
 * Safe here because every wake/timekeeping path survives Cortex-M7 WFI Sleep
 * (HCLK and APB clocks keep running; only CPU instruction execution stops,
 * RM0385 Sleep mode):
 *   - SysTick (HCLK source, prio 14 > PendSV 15) keeps ticking -> tx_thread_sleep
 *     wakeups and HAL tick advance; sleeping threads (LED 250 ms) resume.
 *   - USART1 RX is interrupt-driven (HAL_UART_Receive_IT, prio 5) -> a byte wakes
 *     the core (WFI ignores PRIMASK for wake-up) and sets CLI_EVT_RX, resuming
 *     the shell thread.
 *   - The exec-profile time source is TIM2 (above), not DWT_CYCCNT, so cpu%/idle
 *     stay correct while the core sleeps (DWT would freeze with the core clock).
 * Not TX_LOW_POWER (tickless): that is a separate, future task. */
#define TX_ENABLE_WFI

#endif /* TX_USER_H */
