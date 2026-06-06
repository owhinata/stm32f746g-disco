/**
 * @file    main.c
 * @brief   OS-less LED blink sample for STM32F746G-DISCO.
 *
 * Chip configuration:
 *   - FPU    : enabled (CMSIS SystemInit + hard-float build flags)
 *   - I-Cache: enabled
 *   - D-Cache: enabled
 *   - SYSCLK : 216 MHz from 25 MHz HSE (PLL: M=25, N=432, P=2)
 *
 * Blinks user LED LD1 (PI1) at 1 Hz.
 */
#include "main.h"

static void SystemClock_Config(void);
static void LD1_GPIO_Init(void);

int main(void)
{
    /* CPU caches must be enabled before HAL_Init() so all subsequent
       accesses are cached. The FPU is already enabled by SystemInit(). */
    SCB_EnableICache();
    SCB_EnableDCache();

    /* HAL_Init(): reset peripherals, enable ART/prefetch, configure the
       SysTick to a 1 ms tick (re-based by HAL_RCC_ClockConfig below). */
    HAL_Init();

    /* Switch the system clock to 200 MHz. */
    SystemClock_Config();

    LD1_GPIO_Init();

    while (1)
    {
        HAL_GPIO_TogglePin(LD1_GPIO_PORT, LD1_PIN);
        HAL_Delay(500);   /* 500 ms on / 500 ms off -> 1 Hz blink */
    }
}

/**
 * @brief  System clock: 216 MHz from a 25 MHz HSE crystal (F746 max).
 *
 *   VCO in  = HSE / PLLM = 25 MHz / 25 = 1 MHz
 *   VCO out = VCO in * PLLN = 1 MHz * 432 = 432 MHz
 *   SYSCLK  = VCO out / PLLP = 432 MHz / 2 = 216 MHz
 *   HCLK    = 216 MHz, PCLK1 = 54 MHz (<=54), PCLK2 = 108 MHz (<=108)
 *
 * Over-drive + VOS scale 1 + 7 flash wait states are required at 216 MHz.
 */
static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    /* Power the voltage regulator at scale 1 for high-frequency operation. */
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState       = RCC_HSE_ON;
    RCC_OscInitStruct.PLL.PLLState   = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM       = 25;
    RCC_OscInitStruct.PLL.PLLN       = 432;
    RCC_OscInitStruct.PLL.PLLP       = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ       = 9;   /* 48 MHz; usable for USB/SDMMC */
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    /* Enable over-drive to reach 200 MHz. */
    if (HAL_PWREx_EnableOverDrive() != HAL_OK)
    {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                       RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;   /* HCLK  = 216 MHz */
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;     /* PCLK1 =  54 MHz */
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;     /* PCLK2 = 108 MHz */
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_7) != HAL_OK)
    {
        Error_Handler();
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

/**
 * @brief  Halt here on unrecoverable error (rapid blink as a hint).
 */
void Error_Handler(void)
{
    __disable_irq();
    while (1)
    {
        /* If GPIO was already up, flash fast; otherwise just spin. */
    }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
    Error_Handler();
}
#endif
