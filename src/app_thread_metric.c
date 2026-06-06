/**
 * @file    app_thread_metric.c
 * @brief   Thread-Metric RTOS benchmark runner on Eclipse ThreadX.
 *
 * The selected tm_*_test.c provides tm_main(); the ThreadX porting layer
 * (utility/benchmarks/thread_metric/threadx_example/tm_porting_layer_threadx.c)
 * provides the tm_* primitives. Board bring-up (clock/cache/VCP printf) is in
 * bsp.c, and the SysTick/ThreadX glue (incl. the 100 Hz tick divider for the
 * benchmark's TM_THREADX_TICKS_PER_SECOND assumption) is in port/threadx/.
 *
 * Results are printed over the VCP (USART1, 115200 8N1).
 */
#include "tx_api.h"
#include "bsp.h"
#include <stdio.h>

void tm_main(void);             /* from the selected tm_*_test.c */
void tx_glue_timer_enable(void); /* from port/threadx/tx_glue.c  */

int main(void)
{
    bsp_init();
    printf("thread_metric: starting (results follow)\r\n");
    tx_kernel_enter();   /* does not return */
    return 0;
}

void tx_application_define(void *first_unused_memory)
{
    (void)first_unused_memory;

    /* Create the benchmark threads (TX_DONT_START + tm_thread_resume). */
    tm_main();

    /* Timer lists exist now: let the SysTick ISR drive ThreadX. */
    tx_glue_timer_enable();
}
