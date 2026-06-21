/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    ltdc_display.c
 * @brief   RK043FN48H LCD bring-up via LTDC (issue #52/#53).
 *
 * See ltdc_display.h for the API contract and the clock/memory rationale.
 * Hardware setup, mirroring the ST BSP (stm32746g_discovery_lcd.c) / RM0385:
 *
 *   - Pixel clock from PLLSAI: VCO_in = HSE/M = 25/25 = 1 MHz (M shared with the
 *     main PLL, RM0385 §5.3.8), PLLSAIN=192 -> VCO 192 MHz, PLLSAIR=5 ->
 *     PLLLCDCLK 38.4 MHz, PLLSAIDIVR=8 -> LCD_CLK 4.8 MHz (~29.6 Hz).  PLLSAI is a
 *     separate PLL, so SYSCLK (216 MHz) and the FMC SDRAM clock (108 MHz) are
 *     untouched.  4.8 MHz is LOWERED from the stock 9.6 MHz (only PLLSAIDIVR
 *     doubled 4 -> 8) to cut the LTDC's continuous SDRAM read pressure and the
 *     DCMI DMA FIFO errors it causes during camera preview (#59; measured -51%
 *     preview FE vs no levers).  NOTE: at 4.8 MHz the line period is 566 clk =
 *     118 us, well OUTSIDE the RK043FN48H 55..65 us spec (the 480 active px alone
 *     take 100 us), and ~29.6 Hz is below the panel's ~50 Hz floor -- a deliberate
 *     out-of-spec operating point validated on hardware (stable image, no sync
 *     loss / flicker, LTDC underrun=0); the in-spec fallback is ~8.8 MHz / ~54 Hz
 *     (PLLSAIN=176, R=5, DIVR=4).
 *   - Panel timing (RK043FN48H datasheet / rk043fn48h.h), HW values are the
 *     spec minus one: HSYNC 41, HBP 13, HFP 32, VSYNC 10, VBP 2, VFP 2.
 *     Polarity HS/VS/DE active-low, pixel clock not inverted (LTDC_PCPOLARITY_IPC).
 *   - LTDC pins all AF14 except PG12 (AF9): PE4, PG12, PI9/10/14/15, PJ0..11/13..15,
 *     PK0/1/2/4/5/6/7 (UM1907 / ST BSP).  LCD_DISP (PI12) and LCD_BL_CTRL (PK3)
 *     are plain GPIO outputs driven manually.
 *   - One RGB565 layer covering the full 480x272 window, source = the `.sdram`
 *     frame buffers below (two of them, double-buffered).
 *
 * Drawing is DMA2D-accelerated (#53): the Chrom-ART engine does register-to-
 * memory fills (ltdc_dma2d_fill) and memory-to-memory blits (ltdc_dma2d_blit).
 * Small transfers are polled to completion; large ones (>= LTDC_DMA2D_IT_MIN_
 * PIXELS) block on a DMA2D completion IRQ instead of spinning the CPU (#64).
 * DMA2D, the CPU and the LTDC read DMA all touch the same MPU non-cacheable
 * `.sdram` region, so they are coherent with no cache work.
 *
 * Double buffer + tear-free flip (#53): drawing targets the back buffer;
 * ltdc_flip() stages it via HAL_LTDC_SetAddress_NoReload() then HAL_LTDC_Reload
 * (VERTICAL_BLANKING) and waits for the hardware reload.  SRCR.VBR is the
 * authoritative signal -- it self-clears only after the HW commits the reload at
 * the next vertical blanking (RM0385 §18.7.6) -- and the reload-ready IRQ
 * (LTDC_IRQHandler -> HAL_LTDC_ReloadEventCallback -> ltdc_reload_sem) is just a
 * wake-up hint.  fail-closed: a stuck reload latches ltdc_fault and leaves the
 * front buffer untouched.  ltdc_lock serializes all drawing + flips.  The
 * FIFO-underrun / transfer-error flags are still polled via ltdc_errors().
 *
 * Clean-room implementation; ST BSP / RM0385 §18 used as reference only.
 */
#include "ltdc_display.h"
#include "sdram.h"

#include "stm32f7xx_hal.h"
#include "tx_api.h"

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

/* Double buffer: two 480 x 272 RGB565 frames = 2 x 261120 B in non-cacheable
   SDRAM.  ltdc_front selects the displayed one; the other is the draw target. */
static uint16_t ltdc_fb[2][LTDC_LCD_WIDTH * LTDC_LCD_HEIGHT]
	__attribute__((aligned(32), section(".sdram.fixed.ltdc")));

static LTDC_HandleTypeDef  hltdc;
static DMA2D_HandleTypeDef hdma2d;

static uint8_t      ltdc_front;       /* index (0/1) of the displayed buffer  */
static bool         ltdc_up;          /* init succeeded                       */
static bool         ltdc_tried;       /* init ran (idempotence latch)         */
static bool         ltdc_fault;       /* reload stuck -> display latched down */
static bool         ltdc_gui_owned;   /* GUIX owns the display (#55)          */
static bool         ltdc_disabled;    /* LTDC scanout stopped (lcd off, #66) */
static TX_SEMAPHORE ltdc_reload_sem;  /* posted by the reload-ready IRQ       */
static TX_MUTEX     ltdc_lock;        /* serializes drawing + flip            */

/* DMA2D interrupt-driven completion (#64).  The engine is single and serialized
   on ltdc_lock, so at most one transfer is armed at a time; dma2d_active is the
   handle DMA2D_IRQHandler dispatches to (NULL between transfers), and
   dma2d_done_sem is posted by the transfer-complete/error callback. */
static TX_SEMAPHORE dma2d_done_sem;                  /* posted on DMA2D completion */
static DMA2D_HandleTypeDef *volatile dma2d_active;   /* in-flight handle (ISR reads) */

bool ltdc_is_up(void)
{
	return ltdc_up && !ltdc_fault;
}

uint16_t *ltdc_framebuffer(void)
{
	return ltdc_is_up() ? &ltdc_fb[ltdc_front][0] : NULL;
}

uint16_t *ltdc_back_buffer(void)
{
	return ltdc_is_up() ? &ltdc_fb[!ltdc_front][0] : NULL;
}

uint8_t ltdc_active_buffer(void)
{
	return ltdc_front;
}

void ltdc_lock_frame(void)
{
	(void)tx_mutex_get(&ltdc_lock, TX_WAIT_FOREVER);
}

void ltdc_unlock_frame(void)
{
	(void)tx_mutex_put(&ltdc_lock);
}

/* GUIX ownership (#55).  Set/clear under ltdc_lock so the flag flip is atomic
   against an in-flight draw helper: take() cannot complete while a helper holds
   the lock mid-draw, and once owned every public draw/flip path (which re-reads
   ltdc_gui_owned under the same lock) becomes a no-op/refusal. */
bool ltdc_gui_take(bool on)
{
	if (!ltdc_up)               /* lock/objects may not exist yet */
		return false;
	ltdc_lock_frame();
	/* Re-check under the lock: fault and the scanout-disabled flag (#66) are
	   both set under ltdc_lock, so the decision cannot race a concurrent flip
	   or lcd off.  GUIX cannot run/flip with scanout off. */
	if (ltdc_fault || (on && ltdc_disabled)) {
		ltdc_unlock_frame();
		return false;
	}
	ltdc_gui_owned = on;
	ltdc_unlock_frame();
	return true;
}

/* Stop/start LTDC scanout at runtime (#66).  Disabling clears LTDC_GCR.LTDCEN so
   the controller stops fetching the framebuffer from SDRAM (parks the backlight
   off too); enabling restarts it -- the layer/timing registers are untouched, so
   scanout resumes on the current front buffer.  Refused while GUIX owns the
   display (it cannot run without scanout) or while the LTDC is down/faulted.
   While disabled, ltdc_flip()/ltdc_gui_take(true) refuse (no VBR reload would
   come, so a flip would otherwise latch ltdc_fault).  Thread-context only. */
int ltdc_set_scanout(bool on)
{
	if (!ltdc_up)               /* lock/objects may not exist yet */
		return LTDC_ERR_STATE;
	ltdc_lock_frame();
	/* Re-check fault/ownership under the lock (an in-flight flip can fault, or
	   GUIX can take ownership, between the unlocked entry and here). */
	if (ltdc_fault || ltdc_gui_owned) {
		ltdc_unlock_frame();
		return LTDC_ERR_STATE;
	}
	if (on) {
		__HAL_LTDC_ENABLE(&hltdc);
		ltdc_disabled = false;
		ltdc_backlight(true);
	} else {
		__HAL_LTDC_DISABLE(&hltdc);
		ltdc_disabled = true;
		ltdc_backlight(false);
	}
	ltdc_unlock_frame();
	return LTDC_OK;
}

bool ltdc_scanout_off(void)
{
	return ltdc_is_up() && ltdc_disabled;
}

bool ltdc_scanout_active(void)
{
	return ltdc_is_up() && !ltdc_disabled;
}

bool ltdc_gui_owns(void)
{
	return ltdc_gui_owned;
}

/* Clear BOTH framebuffers to black (issue #65).  Low-level: it writes ltdc_fb
   directly under ltdc_lock and does NOT go through the scanout-disabled draw
   refusal, so it works while scanout is OFF -- a destructive `sdram test`
   repaints the clobbered .sdram buffers with this before re-enabling scanout.
   No-op if the LTDC never came up or is faulted.  Thread-context only. */
void ltdc_clear(void)
{
	if (!ltdc_is_up())
		return;
	ltdc_lock_frame();
	for (uint32_t b = 0; b < 2u; b++)
		for (uint32_t i = 0; i < LTDC_LCD_WIDTH * LTDC_LCD_HEIGHT; i++)
			ltdc_fb[b][i] = LTDC_RGB565_BLACK;
	ltdc_unlock_frame();
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

/* ---- DMA2D interrupt-driven completion (#64) -------------------------------
 * Large transfers block on dma2d_done_sem instead of spinning in
 * HAL_DMA2D_PollForTransfer (see ltdc_display.h).  HAL_DMA2D_Start_IT enables
 * TC|TE|CE together but its IRQ handler only disables the bit of the event that
 * fired, so the disarm below MUST explicitly clear all three: a leftover TEIE/
 * CEIE could otherwise fire DMA2D_IRQHandler during a later POLLED transfer (it
 * does not touch the IT enables), where it would steal the completion flag from
 * HAL_DMA2D_PollForTransfer with a stale dma2d_active.  Between transfers we keep
 * the invariant: dma2d_active == NULL and TC|TE|CE all disabled. */

/* Transfer-complete AND transfer-error callback (same for both; the waiter reads
   h->State to tell success from failure).  Runs in DMA2D_IRQHandler context. */
static void dma2d_xfer_done(DMA2D_HandleTypeDef *h)
{
	(void)h;
	(void)tx_semaphore_put(&dma2d_done_sem);
}

void ltdc_dma2d_arm_it(struct __DMA2D_HandleTypeDef *h_)
{
	DMA2D_HandleTypeDef *h = (DMA2D_HandleTypeDef *)h_;

	/* Drain any stale post (e.g. a late IRQ from a previous timed-out transfer)
	   so the wait blocks on THIS transfer.  Mirrors ltdc_flip_locked's drain. */
	while (tx_semaphore_get(&dma2d_done_sem, TX_NO_WAIT) == TX_SUCCESS)
		;
	h->XferCpltCallback  = dma2d_xfer_done;
	h->XferErrorCallback = dma2d_xfer_done;
	h->ErrorCode         = HAL_DMA2D_ERROR_NONE;   /* TC path |= NONE never clears */
	dma2d_active         = h;                       /* before HAL_DMA2D_Start_IT */
}

/* Tear down an arm and leave DMA2D idle + the handle unlocked.  @p completed is
   true only when the HAL completion callback actually ran (the semaphore was
   obtained): the handle is then already READY and __HAL_UNLOCK'd, and CR.START
   is 0, so no abort is needed.  When false (timeout, or a start that never
   delivered a callback) the handle is still BUSY + LOCKED from
   HAL_DMA2D_Start_IT -- and the hardware may have self-cleared CR.START exactly
   as we masked the IRQ, so a CR.START test would wrongly skip cleanup -- so we
   ALWAYS HAL_DMA2D_Abort: it disables the ITs, sets State=READY and UNLOCKS the
   handle even when START is already 0, preventing a wedged handle (the next
   HAL_DMA2D_Init would otherwise spin on __HAL_LOCK forever). */
static void dma2d_disarm(DMA2D_HandleTypeDef *h, bool completed)
{
	uint32_t pm;

	/* Stop any pending/late completion IRQ from interleaving the teardown, then
	   drop the in-flight handle, under PRIMASK.  A DMA2D IRQ taken after this
	   sees dma2d_active == NULL and silences itself (see DMA2D_IRQHandler). */
	pm = __get_PRIMASK();
	__disable_irq();
	__HAL_DMA2D_DISABLE_IT(h, DMA2D_IT_TC | DMA2D_IT_TE | DMA2D_IT_CE);
	dma2d_active = NULL;
	__set_PRIMASK(pm);

	if (!completed || (DMA2D->CR & DMA2D_CR_START) != 0u)
		(void)HAL_DMA2D_Abort(h);   /* unlock + idle even if START already 0 */
	__HAL_DMA2D_CLEAR_FLAG(h, DMA2D_FLAG_TC | DMA2D_FLAG_TE | DMA2D_FLAG_CE);
}

void ltdc_dma2d_disarm_it(struct __DMA2D_HandleTypeDef *h_)
{
	/* No completion callback reached us (e.g. HAL_DMA2D_Start_IT failed): force
	   the abort/unlock path so the arm cannot leave the handle wedged. */
	dma2d_disarm((DMA2D_HandleTypeDef *)h_, false);
}

bool ltdc_dma2d_wait_it(struct __DMA2D_HandleTypeDef *h_, uint32_t timeout_ms)
{
	DMA2D_HandleTypeDef *h = (DMA2D_HandleTypeDef *)h_;
	bool got, ok;

	got = (tx_semaphore_get(&dma2d_done_sem, timeout_ms) == TX_SUCCESS);
	ok  = got && (h->State == HAL_DMA2D_STATE_READY);
	dma2d_disarm(h, got);   /* timeout (!got) -> force HAL_DMA2D_Abort to unlock */
	return ok;
}

/* ---- DMA2D (Chrom-ART) draw primitives (ST BSP LL_FillBuffer / -------------
 * LL_ConvertLineToARGB8888 idiom: per-op Init + ConfigLayer + Start + complete
 * (polled for small transfers, interrupt-driven for large ones, #64)). */

/* Expand an RGB565 colour to ARGB8888 (0x00RRGGBB).  R2M fills MUST pass this
   to HAL_DMA2D_Start(): the F7 HAL takes the R2M "color" argument as an
   ARGB8888 value and re-packs it to the output format (stm32f7xx_hal_dma2d.c
   DMA2D_SetConfig, RM0385 §9), so a raw RGB565 word would render as a wrong
   colour.  The 5/6/5 -> 8/8/8 expansion replicates the high bits into the low
   ones, and the HAL truncates straight back to the original 5/6/5. */
static uint32_t rgb565_to_argb8888(uint16_t c)
{
	uint32_t r = (uint32_t)(c >> 11) & 0x1Fu;
	uint32_t g = (uint32_t)(c >> 5) & 0x3Fu;
	uint32_t b = (uint32_t)c & 0x1Fu;

	r = (r << 3) | (r >> 2);          /* 5 -> 8 */
	g = (g << 2) | (g >> 4);          /* 6 -> 8 */
	b = (b << 3) | (b >> 2);          /* 5 -> 8 */
	return (r << 16) | (g << 8) | b;
}

/* Kick off an already-configured 2-operand DMA2D transfer (R2M fill or M2M blit
   -- both use HAL_DMA2D_Start/_Start_IT with the same signature) and run it to
   completion: interrupt-driven (block) when w*h is large, else polled (spin),
   per LTDC_DMA2D_IT_MIN_PIXELS (#64).  Returns true on a clean completion.
   Caller holds ltdc_lock. */
static bool dma2d_run(uint32_t pdata, uint32_t dst, uint32_t w, uint32_t h)
{
	if ((uint64_t)w * h >= LTDC_DMA2D_IT_MIN_PIXELS) {
		ltdc_dma2d_arm_it(&hdma2d);
		if (HAL_DMA2D_Start_IT(&hdma2d, pdata, dst, w, h) != HAL_OK) {
			ltdc_dma2d_disarm_it(&hdma2d);
			return false;
		}
		return ltdc_dma2d_wait_it(&hdma2d, 30);
	}
	if (HAL_DMA2D_Start(&hdma2d, pdata, dst, w, h) != HAL_OK)
		return false;
	return HAL_DMA2D_PollForTransfer(&hdma2d, 30) == HAL_OK;
}

/* Register-to-memory single-colour fill of a w x h RGB565 block, @p line_off
   u16 of padding skipped at each row end (= dst stride - w). */
static void ltdc_dma2d_fill(uint16_t *dst, uint16_t color, uint32_t w,
                            uint32_t h, uint32_t line_off)
{
	if (w == 0u || h == 0u)
		return;       /* a zero-size DMA2D transfer is not meaningful */

	hdma2d.Instance         = DMA2D;
	hdma2d.Init.Mode        = DMA2D_R2M;
	hdma2d.Init.ColorMode   = DMA2D_OUTPUT_RGB565;
	hdma2d.Init.OutputOffset = line_off;

	if (HAL_DMA2D_Init(&hdma2d) == HAL_OK &&
	    HAL_DMA2D_ConfigLayer(&hdma2d, 1) == HAL_OK)
		(void)dma2d_run(rgb565_to_argb8888(color), (uint32_t)(uintptr_t)dst,
		                w, h);
}

/* Memory-to-memory copy of a w x h RGB565 block, with separate u16 row offsets
   for destination (@p dst_off) and source (@p src_off). */
static void ltdc_dma2d_blit(uint16_t *dst, const uint16_t *src, uint32_t w,
                            uint32_t h, uint32_t dst_off, uint32_t src_off)
{
	if (w == 0u || h == 0u)
		return;       /* a zero-size DMA2D transfer is not meaningful */

	hdma2d.Instance          = DMA2D;
	hdma2d.Init.Mode         = DMA2D_M2M;
	hdma2d.Init.ColorMode    = DMA2D_OUTPUT_RGB565;
	hdma2d.Init.OutputOffset = dst_off;

	hdma2d.LayerCfg[1].AlphaMode      = DMA2D_NO_MODIF_ALPHA;
	hdma2d.LayerCfg[1].InputAlpha     = 0xFF;
	hdma2d.LayerCfg[1].InputColorMode = DMA2D_INPUT_RGB565;
	hdma2d.LayerCfg[1].InputOffset    = src_off;

	if (HAL_DMA2D_Init(&hdma2d) == HAL_OK &&
	    HAL_DMA2D_ConfigLayer(&hdma2d, 1) == HAL_OK)
		(void)dma2d_run((uint32_t)(uintptr_t)src, (uint32_t)(uintptr_t)dst,
		                w, h);
}

/* PLLSAI -> LCD_CLK = 4.8 MHz (#59; see file header / RM0385 §5.3.24/25).
   VCO_in 1 MHz * PLLSAIN(192) = 192 MHz (100..432 range) / PLLSAIR(5) = 38.4 MHz
   / PLLSAIDIVR(8) = 4.8 MHz (~29.6 Hz).  Out-of-spec on purpose (validated on
   hardware); in-spec fallback PLLSAIN=176, R=5, DIVR=4 (-> 8.8 MHz / ~54 Hz). */
static int ltdc_clock_init(void)
{
	RCC_PeriphCLKInitTypeDef pclk = {0};

	pclk.PeriphClockSelection = RCC_PERIPHCLK_LTDC;
	pclk.PLLSAI.PLLSAIN       = 192;
	pclk.PLLSAI.PLLSAIR       = 5;
	pclk.PLLSAIDivR           = RCC_PLLSAIDIVR_8;

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
	layer.FBStartAdress   = (uint32_t)(uintptr_t)&ltdc_fb[0][0];
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

	/* ThreadX objects for the tear-free flip (the reload-ready IRQ posts the
	   semaphore; the mutex serializes drawing + flip).  Created before the
	   controller comes up so the IRQ has somewhere to post once enabled. */
	if (tx_semaphore_create(&ltdc_reload_sem, "ltdc_rl", 0) != TX_SUCCESS) {
		LOG_ERR("ltdc reload semaphore create failed");
		return LTDC_ERR_STATE;
	}
	if (tx_mutex_create(&ltdc_lock, "ltdc", TX_INHERIT) != TX_SUCCESS) {
		LOG_ERR("ltdc lock create failed");
		tx_semaphore_delete(&ltdc_reload_sem);
		return LTDC_ERR_STATE;
	}
	/* DMA2D completion semaphore (#64): posted by DMA2D_IRQHandler so a large
	   transfer's caller blocks instead of spinning HAL_DMA2D_PollForTransfer. */
	if (tx_semaphore_create(&dma2d_done_sem, "dma2d", 0) != TX_SUCCESS) {
		LOG_ERR("dma2d completion semaphore create failed");
		tx_mutex_delete(&ltdc_lock);
		tx_semaphore_delete(&ltdc_reload_sem);
		return LTDC_ERR_STATE;
	}

	rc = ltdc_clock_init();
	if (rc != LTDC_OK)
		goto fail_obj;

	__HAL_RCC_LTDC_CLK_ENABLE();
	__HAL_RCC_DMA2D_CLK_ENABLE();    /* Chrom-ART for the draw primitives */
	ltdc_gpio_init();

	/* `.sdram` is NOLOAD (undefined at reset): clear BOTH buffers to black
	   BEFORE the layer is configured and the backlight comes on, so neither a
	   garbage front frame nor a garbage first flip ever shows. */
	for (uint32_t b = 0; b < 2u; b++)
		for (uint32_t i = 0; i < LTDC_LCD_WIDTH * LTDC_LCD_HEIGHT; i++)
			ltdc_fb[b][i] = LTDC_RGB565_BLACK;

	ltdc_front = 0;
	ltdc_fault = false;

	rc = ltdc_controller_init();
	if (rc != LTDC_OK) {
		ltdc_backlight(false);
		(void)HAL_LTDC_DeInit(&hltdc);
		__HAL_RCC_DMA2D_CLK_DISABLE();
		__HAL_RCC_LTDC_CLK_DISABLE();
		goto fail_obj;     /* ltdc_up stays false */
	}

	/* Reload-ready IRQ: below DCMI/DMA2 (8), above SD (6-7) / USART (5).  The
	   handler only fires after HAL_LTDC_Reload() arms LTDC_IT_RR in ltdc_flip. */
	HAL_NVIC_SetPriority(LTDC_IRQn, 9, 0);
	HAL_NVIC_EnableIRQ(LTDC_IRQn);

	/* DMA2D completion IRQ (#64): priority 10, below DCMI/DMA2 (8) and LTDC (9)
	   so it never preempts the camera DMA -- completion notification is not
	   latency-critical, it just wakes the blocked draw thread.  Only armed
	   transiently by HAL_DMA2D_Start_IT (ltdc_dma2d_arm_it); idle otherwise.
	   ThreadX-call safety from the ISR is by the port's PRIMASK critical sections
	   (like the camera/SD/LTDC ISRs), not by this priority value. */
	HAL_NVIC_SetPriority(DMA2D_IRQn, 10, 0);
	HAL_NVIC_EnableIRQ(DMA2D_IRQn);

	ltdc_backlight(true);

	ltdc_up = true;
	LOG_INF("RK043FN48H up: 480x272 RGB565 double-buffered, LCD_CLK 4.8 MHz, "
	        "FB0 @0x%08lx FB1 @0x%08lx",
	        (unsigned long)(uintptr_t)&ltdc_fb[0][0],
	        (unsigned long)(uintptr_t)&ltdc_fb[1][0]);
	return LTDC_OK;

fail_obj:
	tx_semaphore_delete(&dma2d_done_sem);
	tx_mutex_delete(&ltdc_lock);
	tx_semaphore_delete(&ltdc_reload_sem);
	return rc;
}

void ltdc_fill(uint16_t rgb565)
{
	uint16_t *back;

	if (!ltdc_is_up())
		return;       /* no-op when down (lock/objects may not exist) */
	ltdc_lock_frame();
	if (ltdc_gui_owned) {        /* GUIX owns the display: shell draw is a no-op */
		ltdc_unlock_frame();
		return;
	}
	back = ltdc_back_buffer();
	if (back != NULL)
		ltdc_dma2d_fill(back, rgb565, LTDC_LCD_WIDTH, LTDC_LCD_HEIGHT, 0);
	ltdc_unlock_frame();
}

void ltdc_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                    uint16_t rgb565)
{
	uint16_t *back;
	uint32_t x1, y1;

	if (!ltdc_is_up())
		return;       /* no-op when down (lock/objects may not exist) */
	ltdc_lock_frame();
	if (ltdc_gui_owned) {        /* GUIX owns the display: shell draw is a no-op */
		ltdc_unlock_frame();
		return;
	}
	back = ltdc_back_buffer();
	if (back == NULL || x >= LTDC_LCD_WIDTH || y >= LTDC_LCD_HEIGHT) {
		ltdc_unlock_frame();
		return;
	}
	x1 = (uint32_t)x + w;
	y1 = (uint32_t)y + h;
	if (x1 > LTDC_LCD_WIDTH)
		x1 = LTDC_LCD_WIDTH;
	if (y1 > LTDC_LCD_HEIGHT)
		y1 = LTDC_LCD_HEIGHT;
	w = (uint16_t)(x1 - x);
	h = (uint16_t)(y1 - y);
	ltdc_dma2d_fill(back + (uint32_t)y * LTDC_LCD_WIDTH + x, rgb565, w, h,
	                LTDC_LCD_WIDTH - w);
	ltdc_unlock_frame();
}

void ltdc_blit(const uint16_t *src, uint16_t x, uint16_t y,
               uint16_t w, uint16_t h)
{
	uint16_t *back;
	uint32_t x1, y1;

	if (!ltdc_is_up())
		return;       /* no-op when down (lock/objects may not exist) */
	ltdc_lock_frame();
	if (ltdc_gui_owned) {        /* GUIX owns the display: shell draw is a no-op */
		ltdc_unlock_frame();
		return;
	}
	back = ltdc_back_buffer();
	if (back == NULL || src == NULL || x >= LTDC_LCD_WIDTH ||
	    y >= LTDC_LCD_HEIGHT) {
		ltdc_unlock_frame();
		return;
	}
	x1 = (uint32_t)x + w;
	y1 = (uint32_t)y + h;
	if (x1 > LTDC_LCD_WIDTH)
		x1 = LTDC_LCD_WIDTH;
	if (y1 > LTDC_LCD_HEIGHT)
		y1 = LTDC_LCD_HEIGHT;
	/* src stride stays the full requested w; the clipped width is x1-x. */
	ltdc_dma2d_blit(back + (uint32_t)y * LTDC_LCD_WIDTH + x, src,
	                x1 - x, y1 - y,
	                LTDC_LCD_WIDTH - (x1 - x), w - (x1 - x));
	ltdc_unlock_frame();
}

void ltdc_colorbar(void)
{
	static const uint16_t bars[8] = {
		LTDC_RGB565_WHITE, LTDC_RGB565_YELLOW, LTDC_RGB565_CYAN,
		LTDC_RGB565_GREEN, LTDC_RGB565_MAGENTA, LTDC_RGB565_RED,
		LTDC_RGB565_BLUE, LTDC_RGB565_BLACK,
	};
	uint16_t *back;
	uint32_t bar_w = LTDC_LCD_WIDTH / 8u;   /* 60 */

	if (!ltdc_is_up())
		return;       /* no-op when down (lock/objects may not exist) */
	ltdc_lock_frame();
	if (ltdc_gui_owned) {        /* GUIX owns the display: shell draw is a no-op */
		ltdc_unlock_frame();
		return;
	}
	back = ltdc_back_buffer();
	if (back == NULL) {
		ltdc_unlock_frame();
		return;
	}
	/* One DMA2D R2M fill per bar; the last bar absorbs the remainder so the
	   eight bars cover the full width exactly. */
	for (uint32_t i = 0; i < 8u; i++) {
		uint32_t x0 = i * bar_w;
		uint32_t w  = (i == 7u) ? (LTDC_LCD_WIDTH - x0) : bar_w;

		ltdc_dma2d_fill(back + x0, bars[i], w, LTDC_LCD_HEIGHT,
		                LTDC_LCD_WIDTH - w);
	}
	ltdc_unlock_frame();
}

void ltdc_blit_demo(void)
{
	uint16_t *back;
	uint32_t half = LTDC_LCD_WIDTH / 2u;   /* 240 */

	if (!ltdc_is_up())
		return;       /* no-op when down (lock/objects may not exist) */
	ltdc_lock_frame();
	if (ltdc_gui_owned) {        /* GUIX owns the display: shell draw is a no-op */
		ltdc_unlock_frame();
		return;
	}
	back = ltdc_back_buffer();
	if (back == NULL) {
		ltdc_unlock_frame();
		return;
	}
	/* M2M copy of the left half (src) over the right half (dst), both within
	   the same back buffer.  Each half is `half` px wide but lives in a
	   full-width (W) frame buffer, so both src and dst skip W-half u16 per row
	   (this is exactly the strided case ltdc_blit() cannot express). */
	ltdc_dma2d_blit(back + half, back, half, LTDC_LCD_HEIGHT,
	                LTDC_LCD_WIDTH - half, LTDC_LCD_WIDTH - half);
	ltdc_unlock_frame();
}

void ltdc_gradient(void)
{
	uint16_t *back;

	if (!ltdc_is_up())
		return;       /* no-op when down (lock/objects may not exist) */
	ltdc_lock_frame();
	if (ltdc_gui_owned) {        /* GUIX owns the display: shell draw is a no-op */
		ltdc_unlock_frame();
		return;
	}
	back = ltdc_back_buffer();
	if (back == NULL) {
		ltdc_unlock_frame();
		return;
	}
	/* Per-column colour, so the CPU draws this one straight into the
	   non-cacheable back buffer (no DMA2D acceleration possible). */
	for (uint32_t col = 0; col < LTDC_LCD_WIDTH; col++) {
		uint32_t lum = col * 255u / (LTDC_LCD_WIDTH - 1u);
		uint16_t px = (uint16_t)(((lum >> 3) << 11) |   /* R5 */
		                         ((lum >> 2) << 5) |    /* G6 */
		                          (lum >> 3));           /* B5 */
		for (uint32_t row = 0; row < LTDC_LCD_HEIGHT; row++)
			back[row * LTDC_LCD_WIDTH + col] = px;
	}
	ltdc_unlock_frame();
}

/*
 * Tear-free present (see file/header doc).  ltdc_lock is held across the whole
 * sequence; every return path releases it.  SRCR.VBR is authoritative: it is
 * set by HAL_LTDC_Reload(VERTICAL_BLANKING) and self-cleared by hardware only
 * after the register reload commits at the next vertical blanking (RM0385
 * §18.7.6).  The reload-ready IRQ just wakes us; we still poll VBR to confirm.
 */
/* Raw tear-free present; caller MUST hold ltdc_lock.  Shared by the public
   ltdc_flip() (ownership-gated) and the owner-only ltdc_gui_flip(). */
static int ltdc_flip_locked(void)
{
	uint8_t back;
	int rc = LTDC_OK;

	if (!ltdc_up || ltdc_fault)
		return LTDC_ERR_STATE;
	if (ltdc_disabled)
		return LTDC_ERR_STATE;   /* scanout off (#66): no VBR reload would come */

	back = (uint8_t)!ltdc_front;

	/* Drain any stale reload posts from a previous flip so the wait below
	   blocks on THIS reload, not a leftover one. */
	while (tx_semaphore_get(&ltdc_reload_sem, TX_NO_WAIT) == TX_SUCCESS)
		;

	(void)HAL_LTDC_SetAddress_NoReload(&hltdc,
	                                   (uint32_t)(uintptr_t)&ltdc_fb[back][0],
	                                   0);
	(void)HAL_LTDC_Reload(&hltdc, LTDC_RELOAD_VERTICAL_BLANKING);

	/* Wake-up hint (the IRQ posts on reload-ready); the truth is VBR below.
	   A frame is ~33.7 ms (~29.6 Hz @ 4.8 MHz, #59), so 100 ms is still several
	   frames of slack. */
	(void)tx_semaphore_get(&ltdc_reload_sem, 100);

	/* Authoritative confirmation: wait (up to ~100 ms total) for the hardware
	   to actually clear VBR.  Poll at 1 ms (1 tick) intervals. */
	for (uint32_t i = 0; i < 100u; i++) {
		if ((hltdc.Instance->SRCR & LTDC_SRCR_VBR) == 0u)
			break;
		tx_thread_sleep(1);
	}

	if ((hltdc.Instance->SRCR & LTDC_SRCR_VBR) == 0u) {
		ltdc_front = back;            /* swap committed */
	} else {
		/* fail-closed: the reload never landed -- leave the front buffer
		   as-is and latch the display down (recovery is a system reset). */
		ltdc_fault = true;
		LOG_ERR("LTDC reload stuck (VBR), display down");
		rc = LTDC_ERR_HAL;
	}
	return rc;
}

int ltdc_flip(void)
{
	int rc;

	if (!ltdc_is_up())
		return LTDC_ERR_STATE;   /* down: lock/objects may not exist */
	ltdc_lock_frame();
	/* While GUIX owns the display the shell must not move ltdc_front (it would
	   strand GUIX's canvas on the wrong buffer); refuse under the lock. */
	if (ltdc_gui_owned) {
		ltdc_unlock_frame();
		return LTDC_ERR_STATE;
	}
	rc = ltdc_flip_locked();
	ltdc_unlock_frame();
	return rc;
}

int ltdc_gui_flip(void)
{
	int rc;

	if (!ltdc_is_up())
		return LTDC_ERR_STATE;
	ltdc_lock_frame();
	rc = ltdc_flip_locked();
	ltdc_unlock_frame();
	return rc;
}

/* ---- LTDC reload-ready IRQ (only armed transiently by HAL_LTDC_Reload). ----
 * Wrapped in the ThreadX execution-profile enter/exit exactly like the camera /
 * SD drivers' ISRs.  HAL_LTDC_IRQHandler() clears the RR flag, disables the RR
 * interrupt and dispatches HAL_LTDC_ReloadEventCallback() below. */
void LTDC_IRQHandler(void)
{
#if defined(TX_EXECUTION_PROFILE_ENABLE)
	{ uint32_t pm = __get_PRIMASK(); __disable_irq();
	  _tx_execution_isr_enter(); __set_PRIMASK(pm); }
#endif
	HAL_LTDC_IRQHandler(&hltdc);
#if defined(TX_EXECUTION_PROFILE_ENABLE)
	{ uint32_t pm = __get_PRIMASK(); __disable_irq();
	  _tx_execution_isr_exit(); __set_PRIMASK(pm); }
#endif
}

/* Reload-ready: wake ltdc_flip() promptly (it still re-checks VBR). */
void HAL_LTDC_ReloadEventCallback(LTDC_HandleTypeDef *hltdc_cb)
{
	if (hltdc_cb == &hltdc)
		(void)tx_semaphore_put(&ltdc_reload_sem);
}

/* ---- DMA2D completion IRQ (#64; armed transiently by HAL_DMA2D_Start_IT). ----
 * Dispatches to the in-flight handle.  Between transfers dma2d_active is NULL
 * (ltdc_dma2d_disarm_it); a DMA2D IRQ taken then is a late/stale completion from
 * a just-disarmed transfer (e.g. a timeout that finished right as we tore down)
 * -- silence every source it could have raised so it cannot re-pend (interrupt
 * storm), rather than feeding HAL a NULL handle.  Wrapped in the ThreadX
 * execution-profile enter/exit exactly like LTDC_IRQHandler. */
void DMA2D_IRQHandler(void)
{
	DMA2D_HandleTypeDef *h;

#if defined(TX_EXECUTION_PROFILE_ENABLE)
	{ uint32_t pm = __get_PRIMASK(); __disable_irq();
	  _tx_execution_isr_enter(); __set_PRIMASK(pm); }
#endif
	h = dma2d_active;
	if (h != NULL) {
		HAL_DMA2D_IRQHandler(h);
	} else {
		DMA2D->CR  &= ~(DMA2D_IT_TC | DMA2D_IT_TE | DMA2D_IT_CE);
		DMA2D->IFCR =  DMA2D_FLAG_TC | DMA2D_FLAG_TE | DMA2D_FLAG_CE;
	}
#if defined(TX_EXECUTION_PROFILE_ENABLE)
	{ uint32_t pm = __get_PRIMASK(); __disable_irq();
	  _tx_execution_isr_exit(); __set_PRIMASK(pm); }
#endif
}
