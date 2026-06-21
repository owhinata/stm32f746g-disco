/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    touch.c
 * @brief   FT5336 capacitive touch panel driver: I2C3 bring-up + polled
 *          multi-touch read (issue #54).
 *
 * See touch.h for the API contract and the hardware facts.  Setup:
 *
 *   - I2C3 on PH7 (SCL) / PH8 (SDA), AF4 open-drain (the board's shared
 *     "DISCOVERY" I2C).  Standard mode ~100 kHz; TIMINGR is computed for
 *     PCLK1 = 54 MHz, the same value the camera's I2C1 uses (the ST BSP
 *     constant 0x40912732 assumes 50 MHz and would land at ~118 kHz).
 *   - No XCLK / reset / power pins: the FT5336 powers from the LCD connector.
 *     touch_init() only sets up the GPIO/I2C and the mutex; the first bus
 *     transaction is lazy (touch_probe()/touch_read()/touch_irq_enable()).
 *   - The FT5336 INT line (PI13, EXTI15_10) drives an interrupt-driven wake
 *     (touch_irq_enable, issue #62): the GUIX input thread blocks on it and
 *     idles at ~0 % CPU instead of polling.  EXTI line 13 is SHARED with PC13
 *     (SD card-detect): SYSCFG_EXTICR4 maps line 13 to exactly one port, so PI13
 *     and PC13 cannot both use EXTI13.  SD-detect is polled (port/sd/sd_card.c),
 *     so PI13 owns line 13 -- do not move SD-detect to EXTI13 without resolving
 *     this mux conflict.
 *
 * I2C reads/writes are interrupt-driven (HAL_I2C_Mem_Read_IT / _Write_IT) and
 * the caller blocks on a completion semaphore posted from the I2C3 ISR, so a
 * transaction never busy-waits.  The Rx/Tx/error HAL callbacks are global (weak)
 * across all I2C units, so they filter on Instance == I2C3 (the camera's I2C1 is
 * blocking and never drives them).
 *
 * Coordinate assembly mirrors the ST BSP (stm32746g_discovery_ts.c):
 * per-point XH/XL/YH/YL bytes pack a 12-bit position plus the event flag and
 * the touch-ID tag, and this panel needs TS_SWAP_XY (the controller's native
 * axes are transposed relative to the LCD), so the assembled X comes from the
 * raw Y and vice-versa.
 *
 * Clean-room glue; the ST BSP component driver (ft5336.h) and the discovery TS
 * BSP were used as a register / mapping reference only.
 */
#include "touch.h"

#include "stm32f7xx_hal.h"

#define LOG_TAG "touch"
#include "log.h"

#include "tx_api.h"

/* FT5336 I2C address (ft5336.h FT5336_I2C_SLAVE_ADDRESS), 8-bit form for the
   HAL Mem API. */
#define TS_I2C_ADDR 0x70u

/* FT5336 register map (ft5336.h), 8-bit register addresses. */
#define FT5336_CHIP_ID_REG  0xA8u   /* chip identification register           */
#define FT5336_ID_VALUE     0x51u   /* expected FT5336_CHIP_ID_REG value      */
#define FT5336_TD_STATUS    0x02u   /* touch-data status: b3..0 = #points     */
/* Point n register block: XH=base, XL=base+1, YH=base+2, YL=base+3, packed at
   6-byte stride (XH,XL,YH,YL,WEIGHT,MISC).  Point 0 starts at 0x03. */
#define FT5336_P1_BASE      0x03u
#define FT5336_P_STRIDE     6u

/* FT5336 gesture/interrupt-mode register (ft5336.h GMODE 0xA4): 0x01 = interrupt
   (trigger) mode -> the controller drives INT on touch; 0x00 = polling (the
   power-on default, INT idle).  We arm trigger mode to get EXTI edges (#62). */
#define FT5336_GMODE_REG     0xA4u
#define FT5336_GMODE_TRIGGER 0x01u

/* FT5336 INT = PI13 -> EXTI line 13 (EXTI15_10), rising-edge wake (mirrors the
   ST BSP stm32746g_discovery_ts.c BSP_TS_ITConfig). */
#define TS_INT_PIN  GPIO_PIN_13
#define TS_INT_PORT GPIOI
#define TS_INT_IRQn EXTI15_10_IRQn

/* TD_STATUS low nibble holds the active-touch count. */
#define FT5336_TD_COUNT_MASK 0x0Fu
/* Pn_XH: b7..6 = event flag, b3..0 = position MSB. */
#define FT5336_EVT_SHIFT    6u
#define FT5336_EVT_MASK     0x03u
#define FT5336_POS_MSB_MASK 0x0Fu
/* Pn_YH: b7..4 = touch-ID tag, b3..0 = position MSB. */
#define FT5336_ID_SHIFT     4u
#define FT5336_ID_MASK      0x0Fu

/*
 * I2C3 TIMINGR for 100 kHz standard mode at I2CCLK = PCLK1 = 54 MHz -- the same
 * value the camera's I2C1 uses (see port/camera/camera.c CAM_I2C_TIMING,
 * RM0385 30.4.10):
 *   PRESC=11, SCLL=24, SCLH=19, SCLDEL=5, SDADEL=2 -> SCL ~= 99 kHz.
 */
#define TOUCH_I2C_TIMING ((11u << 28) | (5u << 20) | (2u << 16) | (19u << 8) | 24u)

/* Per-transaction I2C-IT completion ceiling (ThreadX ticks; 1 tick = 1 ms).
   Only ever matters when the bus is wedged. */
#define TOUCH_I2C_TIMEOUT_TICKS 100u

/* NVIC: both below LTDC(9) so a touch IRQ never delays the display reload, and
   above SysTick(14)/PendSV(15).  The ThreadX port masks with PRIMASK, so
   tx_semaphore_put from either ISR is safe whatever the numeric priority; the
   values are a latency budget, not a correctness constraint.  EXTI above I2C so
   a new touch edge latches even while an I2C completion is being serviced. */
#define TOUCH_EXTI_IRQ_PRIO 10u
#define TOUCH_I2C_IRQ_PRIO  11u

static I2C_HandleTypeDef hts_i2c;   /* I2C3, FT5336                          */
static bool touch_up;               /* touch_init() brought the bus up       */
static bool touch_tried;            /* touch_init() ran (idempotent latch)   */
static TX_MUTEX touch_lock;         /* per-operation serialization           */

/* I2C-IT completion (#62): the Rx/Tx/error HAL callback posts touch_i2c_done;
   the issuing thread bounded-waits on it (no busy-wait).  touch_i2c_active gates
   a late post after a recovered timeout; touch_i2c_err carries the error verdict. */
static TX_SEMAPHORE     touch_i2c_done;
static volatile int     touch_i2c_active;
static volatile int     touch_i2c_err;

/* EXTI13 wake (#62): the PI13 ISR posts touch_evt_sem; the GUIX input thread
   blocks on it (touch_wait_event) and idles at ~0 % CPU.  touch_irq_armed lets
   the ISR be silenced (park / disarm) without tearing down the EXTI config. */
static TX_SEMAPHORE     touch_evt_sem;
static volatile bool    touch_irq_armed;

/* ISR execution-profile wrappers (mirror port/camera/camera.c): account ISR CPU
   to the (isr) row of `thread` when TX_EXECUTION_PROFILE_ENABLE is set. */
#if defined(TX_EXECUTION_PROFILE_ENABLE)
#define TOUCH_ISR_ENTER() do { uint32_t pm = __get_PRIMASK(); __disable_irq(); \
		_tx_execution_isr_enter(); __set_PRIMASK(pm); } while (0)
#define TOUCH_ISR_EXIT()  do { uint32_t pm = __get_PRIMASK(); __disable_irq(); \
		_tx_execution_isr_exit(); __set_PRIMASK(pm); } while (0)
#else
#define TOUCH_ISR_ENTER() do { } while (0)
#define TOUCH_ISR_EXIT()  do { } while (0)
#endif

/* Drop a stale completion post left by a prior aborted/timed-out transfer. */
static void touch_i2c_drain(void)
{
	while (tx_semaphore_get(&touch_i2c_done, TX_NO_WAIT) == TX_SUCCESS)
		;
}

/* No synchronous HAL_I2C abort exists (only HAL_I2C_Master_Abort_IT, async), so
   a wedged/timed-out transfer is recovered with a clean DeInit/Init -- the next
   touch_rd/touch_wr then starts from a known-good READY state. */
static void touch_i2c_recover(void)
{
	(void)HAL_I2C_DeInit(&hts_i2c);
	(void)HAL_I2C_Init(&hts_i2c);
}

/* Wait for the in-flight IT transaction (touch_i2c_done), recovering the bus on
   timeout/error.  Caller holds touch_lock and has set touch_i2c_active = 1. */
static int touch_i2c_wait(void)
{
	if (tx_semaphore_get(&touch_i2c_done, TOUCH_I2C_TIMEOUT_TICKS) != TX_SUCCESS) {
		touch_i2c_active = 0;
		touch_i2c_recover();
		touch_i2c_drain();
		return TOUCH_ERR_TIMEOUT;
	}
	touch_i2c_active = 0;
	if (touch_i2c_err) {
		touch_i2c_recover();
		touch_i2c_drain();
		return TOUCH_ERR_HAL;
	}
	return TOUCH_OK;
}

/* Locked interrupt-driven read: register @p reg, @p n bytes into @p buf.  The
   thread blocks on the completion semaphore (no busy-wait). */
static int touch_rd(uint8_t reg, uint8_t *buf, uint16_t n)
{
	int rc;

	if (tx_mutex_get(&touch_lock, TX_WAIT_FOREVER) != TX_SUCCESS)
		return TOUCH_ERR_STATE;
	touch_i2c_drain();
	touch_i2c_err    = 0;
	touch_i2c_active = 1;
	if (HAL_I2C_Mem_Read_IT(&hts_i2c, TS_I2C_ADDR, reg, I2C_MEMADD_SIZE_8BIT,
	                        buf, n) != HAL_OK) {
		touch_i2c_active = 0;
		touch_i2c_recover();
		tx_mutex_put(&touch_lock);
		return TOUCH_ERR_HAL;
	}
	rc = touch_i2c_wait();
	tx_mutex_put(&touch_lock);
	return rc;
}

/* Locked interrupt-driven single-byte write (used for the GMODE arm).  @p val is
   on the caller stack and stays valid because this blocks until completion. */
static int touch_wr(uint8_t reg, uint8_t val)
{
	int rc;

	if (tx_mutex_get(&touch_lock, TX_WAIT_FOREVER) != TX_SUCCESS)
		return TOUCH_ERR_STATE;
	touch_i2c_drain();
	touch_i2c_err    = 0;
	touch_i2c_active = 1;
	if (HAL_I2C_Mem_Write_IT(&hts_i2c, TS_I2C_ADDR, reg, I2C_MEMADD_SIZE_8BIT,
	                         &val, 1) != HAL_OK) {
		touch_i2c_active = 0;
		touch_i2c_recover();
		tx_mutex_put(&touch_lock);
		return TOUCH_ERR_HAL;
	}
	rc = touch_i2c_wait();
	tx_mutex_put(&touch_lock);
	return rc;
}

int touch_init(void)
{
	GPIO_InitTypeDef g = {0};

	if (touch_tried)
		return touch_up ? TOUCH_OK : TOUCH_ERR_HAL;
	touch_tried = true;

	if (tx_mutex_create(&touch_lock, "touch", TX_INHERIT) != TX_SUCCESS)
		return TOUCH_ERR_HAL;
	/* IT completion + EXTI wake semaphores (#62), unwind on failure. */
	if (tx_semaphore_create(&touch_i2c_done, "ts_i2c", 0) != TX_SUCCESS) {
		tx_mutex_delete(&touch_lock);
		return TOUCH_ERR_HAL;
	}
	if (tx_semaphore_create(&touch_evt_sem, "ts_evt", 0) != TX_SUCCESS) {
		tx_semaphore_delete(&touch_i2c_done);
		tx_mutex_delete(&touch_lock);
		return TOUCH_ERR_HAL;
	}

	__HAL_RCC_GPIOH_CLK_ENABLE();
	__HAL_RCC_GPIOI_CLK_ENABLE();    /* PI13 = FT5336 INT (EXTI13, #62)        */
	__HAL_RCC_SYSCFG_CLK_ENABLE();   /* EXTICR for the PI13 EXTI line          */
	__HAL_RCC_I2C3_CLK_ENABLE();

	/* I2C3: PH7 = SCL, PH8 = SDA (UM1907 DISCOVERY I2C), AF4 open-drain.
	   The bus has board pull-ups. */
	g.Pin       = GPIO_PIN_7 | GPIO_PIN_8;
	g.Mode      = GPIO_MODE_AF_OD;
	g.Pull      = GPIO_NOPULL;
	g.Speed     = GPIO_SPEED_FREQ_HIGH;
	g.Alternate = GPIO_AF4_I2C3;
	HAL_GPIO_Init(GPIOH, &g);

	hts_i2c.Instance              = I2C3;
	hts_i2c.Init.Timing           = TOUCH_I2C_TIMING;
	hts_i2c.Init.OwnAddress1      = 0;
	hts_i2c.Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
	hts_i2c.Init.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
	hts_i2c.Init.OwnAddress2      = 0;
	hts_i2c.Init.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
	hts_i2c.Init.NoStretchMode    = I2C_NOSTRETCH_DISABLE;
	if (HAL_I2C_Init(&hts_i2c) != HAL_OK) {
		LOG_ERR("I2C3 init failed");
		return TOUCH_ERR_HAL;
	}

	/* Arm the I2C3 event/error interrupts for the IT-driven reads/writes (#62).
	   Safe to enable now even before any transaction.  The EXTI line + GMODE are
	   armed later in touch_irq_enable() -- they need bus I/O, which init avoids. */
	HAL_NVIC_SetPriority(I2C3_EV_IRQn, TOUCH_I2C_IRQ_PRIO, 0);
	HAL_NVIC_EnableIRQ(I2C3_EV_IRQn);
	HAL_NVIC_SetPriority(I2C3_ER_IRQn, TOUCH_I2C_IRQ_PRIO, 0);
	HAL_NVIC_EnableIRQ(I2C3_ER_IRQn);

	touch_up = true;
	LOG_INF("I2C3 up (PH7/PH8, IT); FT5336 I/O is lazy");
	return TOUCH_OK;
}

bool touch_is_up(void)
{
	return touch_up;
}

int touch_probe(uint8_t *id)
{
	uint8_t val = 0;
	int rc;

	if (!touch_up)
		return TOUCH_ERR_STATE;

	rc = touch_rd(FT5336_CHIP_ID_REG, &val, 1);
	if (rc != TOUCH_OK)
		return rc;
	if (id != NULL)
		*id = val;
	return (val == FT5336_ID_VALUE) ? TOUCH_OK : TOUCH_ERR_ID;
}

int touch_read(struct touch_state *st)
{
	uint8_t td = 0;
	uint8_t count;
	int rc;

	if (st == NULL)
		return TOUCH_ERR_STATE;
	if (!touch_up)
		return TOUCH_ERR_STATE;

	st->count = 0;

	rc = touch_rd(FT5336_TD_STATUS, &td, 1);
	if (rc != TOUCH_OK)
		return rc;

	count = td & FT5336_TD_COUNT_MASK;
	if (count > TOUCH_MAX_POINTS)
		count = TOUCH_MAX_POINTS;        /* clamp a bogus status */

	uint8_t valid = 0;

	for (uint8_t n = 0; n < count; n++) {
		uint8_t  b[4];                   /* XH, XL, YH, YL       */
		uint16_t rawx, rawy, x, y;

		rc = touch_rd((uint8_t)(FT5336_P1_BASE + FT5336_P_STRIDE * n),
		              b, 4);
		if (rc != TOUCH_OK)
			return rc;

		rawx = (uint16_t)(((b[0] & FT5336_POS_MSB_MASK) << 8) | b[1]);
		rawy = (uint16_t)(((b[2] & FT5336_POS_MSB_MASK) << 8) | b[3]);

		/* TS_SWAP_XY: this panel transposes the controller's native axes
		   relative to the LCD, so swap X <-> Y (stm32746g_discovery_ts.c).
		   The FT5336 reports panel-pixel coordinates directly here (x 0..479,
		   y 0..271) -- confirmed on hardware by corner taps -- so no scaling
		   is applied. */
		x = rawy;
		y = rawx;

		/* Drop the FT5336 "not touched" sentinel: an idle or just-released
		   controller (notably while left in trigger mode after the GUIX input
		   path, #73) reports a nonzero TD_STATUS count with all-ones
		   (0xFFF/0xFFF) out-of-panel coordinates.  Keep only points inside the
		   panel -- the same validity rule the GUIX driver applies
		   (guix_touch.c) -- so callers never see the phantom point. */
		if (x >= TOUCH_PANEL_W || y >= TOUCH_PANEL_H)
			continue;

		st->p[valid].x     = x;
		st->p[valid].y     = y;
		st->p[valid].event = (uint8_t)((b[0] >> FT5336_EVT_SHIFT) &
		                               FT5336_EVT_MASK);
		st->p[valid].id    = (uint8_t)((b[2] >> FT5336_ID_SHIFT) &
		                               FT5336_ID_MASK);
		valid++;
	}

	st->count = valid;
	return TOUCH_OK;
}

/* ---- EXTI13 wake + IT arming (issue #62) -------------------------------- */

int touch_irq_enable(void)
{
	GPIO_InitTypeDef g = {0};
	int rc;

	if (!touch_up)
		return TOUCH_ERR_STATE;

	/* Arm the PI13 rising-edge EXTI + NVIC FIRST (mirrors ST BSP BSP_TS_ITConfig
	   order: GPIO/NVIC, then EnableIT), so no INT edge is lost in the window
	   between enabling the FT5336 INT and arming the line.  HAL_GPIO_Init
	   programs SYSCFG_EXTICR4 (port I -> line 13), the rising trigger and the
	   interrupt mask register. */
	g.Pin   = TS_INT_PIN;
	g.Mode  = GPIO_MODE_IT_RISING;
	g.Pull  = GPIO_NOPULL;
	g.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(TS_INT_PORT, &g);
	__HAL_GPIO_EXTI_CLEAR_IT(TS_INT_PIN);   /* drop a stale pending edge */
	HAL_NVIC_SetPriority(TS_INT_IRQn, TOUCH_EXTI_IRQ_PRIO, 0);
	HAL_NVIC_EnableIRQ(TS_INT_IRQn);
	touch_irq_armed = true;

	/* Now put the FT5336 in interrupt (trigger) mode so it drives INT on touch
	   (power-on default is polling = INT idle, no edges).  Done LAST -- the EXTI
	   is already armed -- and the GMODE write does I2C I/O so this needs thread
	   context (hence not in touch_init()).  Roll back the arm on failure so the
	   caller's degrade-to-polling fallback is clean. */
	rc = touch_wr(FT5336_GMODE_REG, FT5336_GMODE_TRIGGER);
	if (rc != TOUCH_OK) {
		touch_irq_armed = false;
		HAL_NVIC_DisableIRQ(TS_INT_IRQn);
		LOG_WRN("FT5336 GMODE arm failed (%d) -- input falls back to polling", rc);
		return rc;
	}
	LOG_INF("FT5336 INT armed (PI13/EXTI13, rising)");
	return TOUCH_OK;
}

void touch_irq_disable(void)
{
	touch_irq_armed = false;
	HAL_NVIC_DisableIRQ(TS_INT_IRQn);
}

int touch_wait_event(unsigned long timeout_ticks)
{
	return (tx_semaphore_get(&touch_evt_sem, (ULONG)timeout_ticks) == TX_SUCCESS)
	       ? TOUCH_OK : TOUCH_ERR_TIMEOUT;
}

void touch_evt_drain(void)
{
	while (tx_semaphore_get(&touch_evt_sem, TX_NO_WAIT) == TX_SUCCESS)
		;
}

void touch_evt_signal(void)
{
	/* Guard against a post to an uncreated semaphore: touch_up is set only after
	   both semaphores exist, so a caller (e.g. guix_stop with the touch bus down)
	   is a safe no-op when touch never came up. */
	if (!touch_up)
		return;
	(void)tx_semaphore_put(&touch_evt_sem);
}

/* ---- ISRs + HAL completion callbacks (issue #62) ------------------------ */

/* EXTI line 13 (PI13 = FT5336 INT).  HAL clears the pending bit and dispatches
   to HAL_GPIO_EXTI_Callback below.  Line 13 is shared with PC13 (SD-detect), but
   SD-detect is polled, so only PI13 fires this vector here. */
void EXTI15_10_IRQHandler(void)
{
	TOUCH_ISR_ENTER();
	HAL_GPIO_EXTI_IRQHandler(TS_INT_PIN);
	TOUCH_ISR_EXIT();
}

void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
	if (pin == TS_INT_PIN && touch_irq_armed)
		(void)tx_semaphore_put(&touch_evt_sem);
}

/* I2C3 event/error vectors -> HAL state machine, which drives the IT transfer
   and the Rx/Tx/error callbacks below. */
void I2C3_EV_IRQHandler(void)
{
	TOUCH_ISR_ENTER();
	HAL_I2C_EV_IRQHandler(&hts_i2c);
	TOUCH_ISR_EXIT();
}

void I2C3_ER_IRQHandler(void)
{
	TOUCH_ISR_ENTER();
	HAL_I2C_ER_IRQHandler(&hts_i2c);
	TOUCH_ISR_EXIT();
}

/* The IT completion callbacks are global (weak) across every I2C unit, so filter
   to I2C3 (the camera's blocking I2C1 never drives them, but stay forward-safe)
   and gate on touch_i2c_active so a late post after a recovered timeout is a
   no-op.  Read and write both complete onto the same touch_i2c_done. */
void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *h)
{
	if (h->Instance != I2C3 || !touch_i2c_active)
		return;
	(void)tx_semaphore_put(&touch_i2c_done);
}

void HAL_I2C_MemTxCpltCallback(I2C_HandleTypeDef *h)
{
	if (h->Instance != I2C3 || !touch_i2c_active)
		return;
	(void)tx_semaphore_put(&touch_i2c_done);
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *h)
{
	if (h->Instance != I2C3 || !touch_i2c_active)
		return;
	touch_i2c_err = 1;
	(void)tx_semaphore_put(&touch_i2c_done);
}
