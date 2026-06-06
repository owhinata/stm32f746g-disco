/**
 * @file    app_exec_profile.c
 * @brief   ThreadX Execution Profile Kit demo for STM32F746G-DISCO.
 *
 * Measures per-thread / ISR / idle execution time using the Cortex-M7 DWT
 * cycle counter (the kit's default TX_EXECUTION_TIME_SOURCE is DWT->CYCCNT at
 * 0xE0001004). Two worker threads do different amounts of work; a reporter
 * thread prints the cycle distribution over the VCP every 3 s.
 *
 * Build with -DBUILD_EXEC_PROFILE=ON (which also defines
 * TX_EXECUTION_PROFILE_ENABLE so the ThreadX port asm calls the profile hooks).
 */
#include "tx_api.h"
#include "tx_execution_profile.h"
#include "bsp.h"
#include <stdio.h>

void tx_glue_timer_enable(void);   /* port/threadx/tx_glue.c */

#define WORK_STACK   1024
#define REPORT_STACK 2048

static TX_THREAD worker_a, worker_b, reporter;
static UCHAR     worker_a_stack[WORK_STACK];
static UCHAR     worker_b_stack[WORK_STACK];
static UCHAR     reporter_stack[REPORT_STACK];

static volatile uint32_t work_sink;

/* Busy for `work` iterations, then sleep 20 ms. The work amount (passed as the
   thread entry input) sets each worker's CPU duty cycle. */
static void worker_entry(ULONG work)
{
    for (;;)
    {
        for (volatile uint32_t i = 0; i < work; i++)
        {
            work_sink += i;
        }
        tx_thread_sleep(20);   /* 20 ms (1 tick = 1 ms) */
    }
}

static void report_entry(ULONG arg)
{
    EXECUTION_TIME ta, tb, ti, tisr;

    (void)arg;
    for (;;)
    {
        /* Reset, observe one 3 s window, then read. */
        _tx_execution_thread_time_reset(&worker_a);
        _tx_execution_thread_time_reset(&worker_b);
        _tx_execution_isr_time_reset();
        _tx_execution_idle_time_reset();

        tx_thread_sleep(3000);   /* 3 s window */

        _tx_execution_thread_time_get(&worker_a, &ta);
        _tx_execution_thread_time_get(&worker_b, &tb);
        _tx_execution_isr_time_get(&tisr);
        _tx_execution_idle_time_get(&ti);

        EXECUTION_TIME total = ta + tb + ti + tisr;
        if (total == 0)
        {
            total = 1;
        }

        /* Per-window cycle counts fit in 32 bits (3 s * 216 MHz < 2^32), so
           print them as unsigned long; percentages use 64-bit math. */
        printf("exec_profile (cycles / 3 s @216 MHz):\r\n");
        printf("  worker_a : %10lu  (%2lu%%)\r\n",
               (unsigned long)ta, (unsigned long)(ta * 100 / total));
        printf("  worker_b : %10lu  (%2lu%%)\r\n",
               (unsigned long)tb, (unsigned long)(tb * 100 / total));
        printf("  ISR      : %10lu  (%2lu%%)\r\n",
               (unsigned long)tisr, (unsigned long)(tisr * 100 / total));
        printf("  idle     : %10lu  (%2lu%%)\r\n\r\n",
               (unsigned long)ti, (unsigned long)(ti * 100 / total));
    }
}

/* Enable the DWT cycle counter (the kit's default time source).
   On Cortex-M7 the DWT registers sit behind a software lock; without writing
   the unlock key to the Lock Access Register first, CYCCNT stays at 0. */
static void dwt_cycle_counter_enable(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    *((volatile uint32_t *)0xE0001FB0) = 0xC5ACCE55u;  /* DWT->LAR unlock */
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

int main(void)
{
    bsp_init();
    dwt_cycle_counter_enable();
    printf("exec_profile: starting\r\n");
    tx_kernel_enter();   /* does not return */
    return 0;
}

void tx_application_define(void *first_unused_memory)
{
    (void)first_unused_memory;

    tx_thread_create(&worker_a, "worker_a", worker_entry, 300000,
                     worker_a_stack, sizeof(worker_a_stack),
                     10, 10, TX_NO_TIME_SLICE, TX_AUTO_START);
    tx_thread_create(&worker_b, "worker_b", worker_entry, 120000,
                     worker_b_stack, sizeof(worker_b_stack),
                     10, 10, TX_NO_TIME_SLICE, TX_AUTO_START);
    tx_thread_create(&reporter, "report", report_entry, 0,
                     reporter_stack, sizeof(reporter_stack),
                     5, 5, TX_NO_TIME_SLICE, TX_AUTO_START);

    tx_glue_timer_enable();
}
