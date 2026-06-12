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

/**
 * Enable the configurable faults (MemManage/Bus/Usage) and divide-by-zero
 * trapping so the crash-dump handler (src/fault.c, issue #28) can classify a
 * fault precisely.  Call early in bsp_init(), after log_init().
 */
void fault_init(void);

#endif /* BSP_H */
