/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    ltdc_display.c
 * @brief   RK043FN48H LCD bring-up via LTDC (issue #52).
 *
 * See ltdc_display.h for the API contract and the clock/memory rationale.
 * Hardware setup, mirroring the ST BSP (stm32746g_discovery_lcd.c) / RM0385:
 *
 *   - Pixel clock from PLLSAI: VCO_in = HSE/M = 25/25 = 1 MHz (M shared with the
 *     main PLL, RM0385 §5.3.8), PLLSAIN=192 -> VCO 192 MHz, PLLSAIR=5 ->
 *     PLLLCDCLK 38.4 MHz, PLLSAIDIVR=4 -> LCD_CLK 9.6 MHz.  PLLSAI is a separate
 *     PLL, so SYSCLK (216 MHz) and the FMC SDRAM clock (108 MHz) are untouched.
 *   - Panel timing (RK043FN48H datasheet / rk043fn48h.h), HW values are the
 *     spec minus one: HSYNC 41, HBP 13, HFP 32, VSYNC 10, VBP 2, VFP 2.
 *     Polarity HS/VS/DE active-low, pixel clock not inverted (LTDC_PCPOLARITY_IPC).
 *   - LTDC pins all AF14 except PG12 (AF9): PE4, PG12, PI9/10/14/15, PJ0..11/13..15,
 *     PK0/1/2/4/5/6/7 (UM1907 / ST BSP).  LCD_DISP (PI12) and LCD_BL_CTRL (PK3)
 *     are plain GPIO outputs driven manually.
 *   - One RGB565 layer covering the full 480x272 window, source = the `.sdram`
 *     frame buffer below.
 *
 * Phase 1 draws with the CPU only (the `.sdram` region is MPU non-cacheable, so
 * writes are immediately visible to the LTDC read DMA -- no cache maintenance).
 * DMA2D and double buffering arrive in #53.  No LTDC interrupt is used; the
 * FIFO-underrun / transfer-error flags are polled via ltdc_errors().
 *
 * Clean-room implementation; ST BSP / RM0385 §18 used as reference only.
 */
#include "ltdc_display.h"
#include "sdram.h"

#include "stm32f7xx_hal.h"

#define LOG_TAG "ltdc"
#include "log.h"

/* Manually-driven panel control pins (UM1907 / ST BSP). */
#define LCD_DISP_PORT     GPIOI
#define LCD_DISP_PIN      GPIO_PIN_12
#define LCD_BL_CTRL_PORT  GPIOK
#define LCD_BL_CTRL_PIN   GPIO_PIN_3

/* RK043FN48H timing (rk043fn48h.h); LTDC programs spec-minus-one. */
#define RK_HSYNC  41u
#define RK_HBP    13u
#define RK_HFP    32u
#define RK_VSYNC  10u
#define RK_VBP     2u
#define RK_VFP     2u

/* Frame buffer: 480 x 272 x RGB565 = 261120 B, in the non-cacheable SDRAM. */
static uint16_t ltdc_fb[LTDC_LCD_WIDTH * LTDC_LCD_HEIGHT]
	__attribute__((aligned(32), section(".sdram")));

static LTDC_HandleTypeDef hltdc;

static bool ltdc_up;       /* init succeeded               */
static bool ltdc_tried;    /* init ran (idempotence latch) */

bool ltdc_is_up(void)
{
	return ltdc_up;
}

uint16_t *ltdc_framebuffer(void)
{
	return ltdc_up ? ltdc_fb : NULL;
}

uint32_t ltdc_pixel_clock_hz(void)
{
	return ltdc_up ? LTDC_PIXEL_CLOCK_HZ : 0u;
}

void ltdc_backlight(bool on)
{
	GPIO_PinState s = on ? GPIO_PIN_SET : GPIO_PIN_RESET;

	HAL_GPIO_WritePin(LCD_DISP_PORT, LCD_DISP_PIN, s);
	HAL_GPIO_WritePin(LCD_BL_CTRL_PORT, LCD_BL_CTRL_PIN, s);
}

uint32_t ltdc_errors(bool clear)
{
	uint32_t mask = 0u;

	if (!ltdc_up)
		return 0u;
	if (__HAL_LTDC_GET_FLAG(&hltdc, LTDC_FLAG_FU))
		mask |= LTDC_ERRFLAG_FIFO_UNDERRUN;
	if (__HAL_LTDC_GET_FLAG(&hltdc, LTDC_FLAG_TE))
		mask |= LTDC_ERRFLAG_TRANSFER_ERROR;
	if (clear) {
		__HAL_LTDC_CLEAR_FLAG(&hltdc, LTDC_FLAG_FU);
		__HAL_LTDC_CLEAR_FLAG(&hltdc, LTDC_FLAG_TE);
	}
	return mask;
}

/* PLLSAI -> LCD_CLK = 9.6 MHz (see file header / RM0385 §5.3.24/25). */
static int ltdc_clock_init(void)
{
	RCC_PeriphCLKInitTypeDef pclk = {0};

	pclk.PeriphClockSelection = RCC_PERIPHCLK_LTDC;
	pclk.PLLSAI.PLLSAIN       = 192;
	pclk.PLLSAI.PLLSAIR       = 5;
	pclk.PLLSAIDivR           = RCC_PLLSAIDIVR_4;

	if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK) {
		LOG_ERR("PLLSAI/LCD clock config failed");
		return LTDC_ERR_HAL;
	}
	return LTDC_OK;
}

static void ltdc_gpio_init(void)
{
	GPIO_InitTypeDef g = {0};

	__HAL_RCC_GPIOE_CLK_ENABLE();
	__HAL_RCC_GPIOG_CLK_ENABLE();
	__HAL_RCC_GPIOI_CLK_ENABLE();
	__HAL_RCC_GPIOJ_CLK_ENABLE();
	__HAL_RCC_GPIOK_CLK_ENABLE();

	g.Mode  = GPIO_MODE_AF_PP;
	g.Pull  = GPIO_NOPULL;
	g.Speed = GPIO_SPEED_FREQ_HIGH;

	g.Alternate = GPIO_AF14_LTDC;
	g.Pin = GPIO_PIN_4;                                       /* PE4  */
	HAL_GPIO_Init(GPIOE, &g);

	g.Alternate = GPIO_AF9_LTDC;                             /* PG12 is AF9 */
	g.Pin = GPIO_PIN_12;
	HAL_GPIO_Init(GPIOG, &g);

	g.Alternate = GPIO_AF14_LTDC;
	g.Pin = GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_14 | GPIO_PIN_15;
	HAL_GPIO_Init(GPIOI, &g);

	g.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 |
	        GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7 |
	        GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 |
	        GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;          /* PJ0..11,13..15 */
	HAL_GPIO_Init(GPIOJ, &g);

	g.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_4 |
	        GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;             /* PK0,1,2,4,5,6,7 */
	HAL_GPIO_Init(GPIOK, &g);

	/* LCD_DISP / LCD_BL_CTRL: plain outputs, parked off (asserted last). */
	g.Mode      = GPIO_MODE_OUTPUT_PP;
	g.Pull      = GPIO_NOPULL;
	g.Alternate = 0;
	g.Pin = LCD_DISP_PIN;
	HAL_GPIO_Init(LCD_DISP_PORT, &g);
	g.Pin = LCD_BL_CTRL_PIN;
	HAL_GPIO_Init(LCD_BL_CTRL_PORT, &g);
	ltdc_backlight(false);
}

static int ltdc_controller_init(void)
{
	LTDC_LayerCfgTypeDef layer = {0};

	hltdc.Instance = LTDC;

	hltdc.Init.HSPolarity = LTDC_HSPOLARITY_AL;
	hltdc.Init.VSPolarity = LTDC_VSPOLARITY_AL;
	hltdc.Init.DEPolarity = LTDC_DEPOLARITY_AL;
	hltdc.Init.PCPolarity = LTDC_PCPOLARITY_IPC;

	hltdc.Init.HorizontalSync     = RK_HSYNC - 1u;                       /* 40  */
	hltdc.Init.VerticalSync       = RK_VSYNC - 1u;                       /* 9   */
	hltdc.Init.AccumulatedHBP     = RK_HSYNC + RK_HBP - 1u;              /* 53  */
	hltdc.Init.AccumulatedVBP     = RK_VSYNC + RK_VBP - 1u;              /* 11  */
	hltdc.Init.AccumulatedActiveW = LTDC_LCD_WIDTH + RK_HSYNC + RK_HBP - 1u;   /* 533 */
	hltdc.Init.AccumulatedActiveH = LTDC_LCD_HEIGHT + RK_VSYNC + RK_VBP - 1u;  /* 283 */
	hltdc.Init.TotalWidth         = LTDC_LCD_WIDTH + RK_HSYNC + RK_HBP + RK_HFP - 1u;   /* 565 */
	hltdc.Init.TotalHeigh         = LTDC_LCD_HEIGHT + RK_VSYNC + RK_VBP + RK_VFP - 1u;  /* 285 */

	hltdc.Init.Backcolor.Red   = 0;
	hltdc.Init.Backcolor.Green = 0;
	hltdc.Init.Backcolor.Blue  = 0;

	if (HAL_LTDC_Init(&hltdc) != HAL_OK) {
		LOG_ERR("HAL_LTDC_Init failed");
		return LTDC_ERR_HAL;
	}

	layer.WindowX0        = 0;
	layer.WindowX1        = LTDC_LCD_WIDTH;
	layer.WindowY0        = 0;
	layer.WindowY1        = LTDC_LCD_HEIGHT;
	layer.PixelFormat     = LTDC_PIXEL_FORMAT_RGB565;
	layer.Alpha           = 255;
	layer.Alpha0          = 0;
	layer.BlendingFactor1 = LTDC_BLENDING_FACTOR1_CA;
	layer.BlendingFactor2 = LTDC_BLENDING_FACTOR2_CA;
	layer.FBStartAdress   = (uint32_t)(uintptr_t)ltdc_fb;
	layer.ImageWidth      = LTDC_LCD_WIDTH;
	layer.ImageHeight     = LTDC_LCD_HEIGHT;
	layer.Backcolor.Red   = 0;
	layer.Backcolor.Green = 0;
	layer.Backcolor.Blue  = 0;

	if (HAL_LTDC_ConfigLayer(&hltdc, &layer, 0) != HAL_OK) {
		LOG_ERR("HAL_LTDC_ConfigLayer failed");
		return LTDC_ERR_HAL;
	}
	return LTDC_OK;
}

int ltdc_init(void)
{
	int rc;

	if (ltdc_tried)
		return ltdc_up ? LTDC_OK : LTDC_ERR_STATE;
	ltdc_tried = true;

	/* The frame buffer lives in SDRAM; touching it (clear / LTDC CFBAR) with
	   the FMC down would fault.  Bring-up is fail-soft, so guard explicitly. */
	if (!sdram_is_up()) {
		LOG_ERR("SDRAM down -- LTDC not started");
		return LTDC_ERR_STATE;
	}

	rc = ltdc_clock_init();
	if (rc != LTDC_OK)
		return rc;

	__HAL_RCC_LTDC_CLK_ENABLE();
	ltdc_gpio_init();

	/* `.sdram` is NOLOAD (undefined at reset): clear to black BEFORE the layer
	   is configured and the backlight comes on, so no garbage frame shows. */
	for (uint32_t i = 0; i < LTDC_LCD_WIDTH * LTDC_LCD_HEIGHT; i++)
		ltdc_fb[i] = LTDC_RGB565_BLACK;

	rc = ltdc_controller_init();
	if (rc != LTDC_OK) {
		ltdc_backlight(false);
		(void)HAL_LTDC_DeInit(&hltdc);
		__HAL_RCC_LTDC_CLK_DISABLE();
		return rc;     /* ltdc_up stays false */
	}

	ltdc_backlight(true);

	ltdc_up = true;
	LOG_INF("RK043FN48H up: 480x272 RGB565, LCD_CLK 9.6 MHz, FB @0x%08lx",
	        (unsigned long)(uintptr_t)ltdc_fb);
	return LTDC_OK;
}

void ltdc_fill(uint16_t rgb565)
{
	if (!ltdc_up)
		return;
	for (uint32_t i = 0; i < LTDC_LCD_WIDTH * LTDC_LCD_HEIGHT; i++)
		ltdc_fb[i] = rgb565;
}

void ltdc_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                    uint16_t rgb565)
{
	uint32_t x1, y1;

	if (!ltdc_up || x >= LTDC_LCD_WIDTH || y >= LTDC_LCD_HEIGHT)
		return;
	x1 = (uint32_t)x + w;
	y1 = (uint32_t)y + h;
	if (x1 > LTDC_LCD_WIDTH)
		x1 = LTDC_LCD_WIDTH;
	if (y1 > LTDC_LCD_HEIGHT)
		y1 = LTDC_LCD_HEIGHT;
	for (uint32_t row = y; row < y1; row++)
		for (uint32_t col = x; col < x1; col++)
			ltdc_fb[row * LTDC_LCD_WIDTH + col] = rgb565;
}

void ltdc_colorbar(void)
{
	static const uint16_t bars[8] = {
		LTDC_RGB565_WHITE, LTDC_RGB565_YELLOW, LTDC_RGB565_CYAN,
		LTDC_RGB565_GREEN, LTDC_RGB565_MAGENTA, LTDC_RGB565_RED,
		LTDC_RGB565_BLUE, LTDC_RGB565_BLACK,
	};

	if (!ltdc_up)
		return;
	for (uint32_t row = 0; row < LTDC_LCD_HEIGHT; row++)
		for (uint32_t col = 0; col < LTDC_LCD_WIDTH; col++)
			ltdc_fb[row * LTDC_LCD_WIDTH + col] =
				bars[col * 8u / LTDC_LCD_WIDTH];
}

void ltdc_gradient(void)
{
	if (!ltdc_up)
		return;
	for (uint32_t col = 0; col < LTDC_LCD_WIDTH; col++) {
		uint32_t lum = col * 255u / (LTDC_LCD_WIDTH - 1u);
		uint16_t px = (uint16_t)(((lum >> 3) << 11) |   /* R5 */
		                         ((lum >> 2) << 5) |    /* G6 */
		                          (lum >> 3));           /* B5 */
		for (uint32_t row = 0; row < LTDC_LCD_HEIGHT; row++)
			ltdc_fb[row * LTDC_LCD_WIDTH + col] = px;
	}
}
