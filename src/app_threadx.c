/**
 * @file    app_threadx.c
 * @brief   ThreadX demo for STM32F746G-DISCO: two threads.
 *
 *   - led_thread   : toggles LD1 (PI1) every 250 ms
 *   - print_thread : prints a counter over the VCP every 1 s
 *
 * Board bring-up (216 MHz clock, caches, VCP UART, printf) is in bsp.c; the
 * SysTick/ThreadX integration is in port/threadx/tx_glue.c.
 */
#include "tx_api.h"
#include "main.h"
#include "bsp.h"
#include <stdio.h>

void tx_glue_timer_enable(void);

#define LED_STACK_SIZE     1024
#define PRINT_STACK_SIZE   2048   /* printf needs headroom */

static TX_THREAD led_thread;
static TX_THREAD print_thread;
static UCHAR     led_stack[LED_STACK_SIZE];
static UCHAR     print_stack[PRINT_STACK_SIZE];

static void led_entry(ULONG arg)
{
    GPIO_InitTypeDef g = {0};

    (void)arg;
    LD1_GPIO_CLK_EN();
    g.Pin   = LD1_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LD1_GPIO_PORT, &g);

    for (;;)
    {
        HAL_GPIO_TogglePin(LD1_GPIO_PORT, LD1_PIN);
        tx_thread_sleep(250);   /* 250 ms (1 tick == 1 ms) */
    }
}

static void print_entry(ULONG arg)
{
    ULONG n = 0;

    (void)arg;
    for (;;)
    {
        printf("threadx: print thread, n=%lu\r\n", (unsigned long)n++);
        tx_thread_sleep(1000);  /* 1 s */
    }
}

/* Called by ThreadX during tx_kernel_enter() to create the application. */
void tx_application_define(void *first_unused_memory)
{
    (void)first_unused_memory;

    tx_thread_create(&led_thread, "led", led_entry, 0,
                     led_stack, sizeof(led_stack),
                     10, 10, TX_NO_TIME_SLICE, TX_AUTO_START);

    tx_thread_create(&print_thread, "print", print_entry, 0,
                     print_stack, sizeof(print_stack),
                     10, 10, TX_NO_TIME_SLICE, TX_AUTO_START);

    /* Timer lists exist now: let the SysTick ISR drive ThreadX. */
    tx_glue_timer_enable();
}

int main(void)
{
    bsp_init();

    printf("threadx: starting kernel\r\n");
    tx_kernel_enter();   /* does not return */

    return 0;
}
