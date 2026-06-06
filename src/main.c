/**
 * @file    main.c
 * @brief   OS-less LED blink app for STM32F746G-DISCO.
 *
 * Blinks user LED LD1 (PI1) at 1 Hz and prints "hello world" + a tick
 * counter over the ST-Link VCP. Board bring-up lives in bsp.c.
 */
#include "main.h"
#include "bsp.h"
#include <stdio.h>

static void LD1_GPIO_Init(void);

int main(void)
{
    bsp_init();
    LD1_GPIO_Init();

    printf("hello world\r\n");

    uint32_t tick = 0;
    while (1)
    {
        HAL_GPIO_TogglePin(LD1_GPIO_PORT, LD1_PIN);
        printf("tick %lu\r\n", (unsigned long)tick++);
        HAL_Delay(500);   /* 500 ms on / 500 ms off -> 1 Hz blink */
    }
}

/**
 * @brief  Configure PI1 as a push-pull output for LD1.
 */
static void LD1_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    LD1_GPIO_CLK_EN();

    HAL_GPIO_WritePin(LD1_GPIO_PORT, LD1_PIN, GPIO_PIN_RESET);

    GPIO_InitStruct.Pin   = LD1_PIN;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LD1_GPIO_PORT, &GPIO_InitStruct);
}
