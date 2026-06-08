/**
 * @file    bsp.c
 * @brief   Board support: 216 MHz clock, caches, VCP UART, printf retarget.
 *
 *   - FPU    : enabled (CMSIS SystemInit + hard-float build flags)
 *   - I-Cache: enabled
 *   - D-Cache: enabled
 *   - SYSCLK : 216 MHz from 25 MHz HSE (PLL: M=25, N=432, P=2)
 *   - VCP    : USART1, TX=PA9 / RX=PB7, 115200 8N1 (ST-Link Virtual COM Port)
 */
#include "bsp.h"
#include <stdio.h>

UART_HandleTypeDef huart1;

static void SystemClock_Config(void);
static void VCP_UART_Init(void);
static void exec_timebase_init(void);

void bsp_init(void)
{
    /* Caches on before HAL_Init so all later accesses are cached.
       The FPU is already enabled by SystemInit() in the startup path. */
    SCB_EnableICache();
    SCB_EnableDCache();

    HAL_Init();
    SystemClock_Config();
    VCP_UART_Init();
    exec_timebase_init();   /* TIM2 free-run = ThreadX exec-profile time source (issue #19) */

    /* Unbuffered stdout so each printf reaches the VCP immediately. */
    setvbuf(stdout, NULL, _IONBF, 0);
}

/**
 * @brief  TIM2 as a free-running 32-bit time source for the ThreadX Execution
 *         Profile Kit (issue #19: `thread` cpu% column).
 *
 * TIM2CLK = 2*PCLK1 = 108 MHz (APB1 /4, TIMPRE = 0), so the counter advances at
 * ~9.26 ns/count and wraps every ~39.77 s -- far longer than any single
 * enter/exit or 1 ms idle interval, which is all the kit measures between ticks.
 *
 * Chosen over the kit default DWT_CYCCNT: DWT freezes when the core clock is
 * gated by WFI (#20), whereas TIM2 keeps its clock in Sleep (TIM2LPEN reset = 1)
 * and so keeps counting -- idle/cpu% stay correct once WFI is enabled.  No
 * interrupt is used; the kit just reads TIM2->CNT (see tx_user.h).  Started after
 * SystemClock_Config() so PCLK1 is final, and before tx_kernel_enter() so the
 * source is live when _tx_execution_initialize() samples it.
 */
static void exec_timebase_init(void)
{
    __HAL_RCC_TIM2_CLK_ENABLE();
    TIM2->PSC = 0u;             /* full 108 MHz resolution */
    TIM2->ARR = 0xFFFFFFFFu;    /* 32-bit free-run */
    TIM2->CNT = 0u;
    TIM2->EGR = TIM_EGR_UG;     /* latch PSC/ARR */
    TIM2->CR1 = TIM_CR1_CEN;    /* up-count, no interrupt */
}

/**
 * @brief  Busy-wait @p us microseconds on the free-running TIM2 counter.
 *
 * TIM2 runs at 108 MHz (set up in exec_timebase_init for the ThreadX execution
 * profile, issue #19), so 108 counts == 1 us.  Unsigned 32-bit subtraction makes
 * the wait wrap-safe as long as the delay is shorter than TIM2's wrap period
 * (~39.77 s); the `usleep` command caps @p us far below that.  Pure busy loop --
 * it does NOT yield, so keep delays short (issue #21).  Interrupts (SysTick,
 * UART) still run, so the ThreadX tick and higher-priority threads are unaffected.
 */
void bsp_udelay(uint32_t us)
{
    uint32_t start = TIM2->CNT;
    uint32_t ticks = us * 108u;            /* 108 TIM2 counts per microsecond */

    while ((TIM2->CNT - start) < ticks)
        ;
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
 * @brief  USART1 on the ST-Link Virtual COM Port (PA9=TX, PB7=RX), 115200 8N1.
 */
static void VCP_UART_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    /* PA9 -> USART1_TX */
    GPIO_InitStruct.Pin       = GPIO_PIN_9;
    GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull      = GPIO_NOPULL;
    GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* PB7 -> USART1_RX */
    GPIO_InitStruct.Pin       = GPIO_PIN_7;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    huart1.Instance          = USART1;
    huart1.Init.BaudRate     = 115200;
    huart1.Init.WordLength   = UART_WORDLENGTH_8B;
    huart1.Init.StopBits     = UART_STOPBITS_1;
    huart1.Init.Parity       = UART_PARITY_NONE;
    huart1.Init.Mode         = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart1) != HAL_OK)
    {
        Error_Handler();
    }
}

/**
 * @brief  Retarget newlib stdout/stderr to the VCP UART so printf works.
 *
 * Weak so a backend can override it: the interrupt-driven UART shell backend
 * (shell/backend/cli_backend_uart.c, issue #7) supplies a strong _write that,
 * once the console is up, routes printf through the same TX ring as the shell so
 * USART1 has a single owner.  Apps that do not link that backend (blink /
 * coremark / threadx demo / thread_metric / exec_profile) keep this blocking
 * polling path unchanged.
 */
__attribute__((weak)) int _write(int file, char *ptr, int len)
{
    (void)file;
    HAL_UART_Transmit(&huart1, (uint8_t *)ptr, (uint16_t)len, HAL_MAX_DELAY);
    return len;
}

void Error_Handler(void)
{
    __disable_irq();
    while (1)
    {
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
