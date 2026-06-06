/**
 * @file    main.h
 * @brief   Bare-metal LED blink sample for STM32F746G-DISCO (OS-less).
 */
#ifndef MAIN_H
#define MAIN_H

#include "stm32f7xx_hal.h"

/* User LED LD1 (green) on STM32F746G-DISCO is wired to PI1, active high. */
#define LD1_PIN          GPIO_PIN_1
#define LD1_GPIO_PORT    GPIOI
#define LD1_GPIO_CLK_EN()  __HAL_RCC_GPIOI_CLK_ENABLE()

void Error_Handler(void);

#endif /* MAIN_H */
