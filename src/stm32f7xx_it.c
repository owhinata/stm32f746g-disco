/**
 * @file    stm32f7xx_it.c
 * @brief   Cortex-M7 / STM32F7 interrupt handlers.
 */
#include "main.h"
#include "stm32f7xx_it.h"

/* ----- Cortex-M7 core exception handlers ----- */

void NMI_Handler(void)
{
    while (1) { }
}

void HardFault_Handler(void)
{
    while (1) { }
}

void MemManage_Handler(void)
{
    while (1) { }
}

void BusFault_Handler(void)
{
    while (1) { }
}

void UsageFault_Handler(void)
{
    while (1) { }
}

void SVC_Handler(void)
{
}

void DebugMon_Handler(void)
{
}

void PendSV_Handler(void)
{
}

/**
 * @brief  1 ms SysTick used by HAL_Delay()/HAL_GetTick().
 */
void SysTick_Handler(void)
{
    HAL_IncTick();
}
