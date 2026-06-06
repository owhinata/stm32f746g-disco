/**
 * @file    bsp.h
 * @brief   Board init shared by all apps (clock, caches, VCP UART, printf).
 */
#ifndef BSP_H
#define BSP_H

#include "stm32f7xx_hal.h"

extern UART_HandleTypeDef huart1;   /* ST-Link Virtual COM Port (USART1) */

/**
 * Enable I/D caches, HAL_Init, switch SYSCLK to 216 MHz, bring up the VCP
 * UART (USART1, PA9/PB7, 115200 8N1) and make stdout unbuffered so printf
 * goes straight to the VCP.
 */
void bsp_init(void);

void Error_Handler(void);

#endif /* BSP_H */
