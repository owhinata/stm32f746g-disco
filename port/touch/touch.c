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
 *     transaction is lazy (touch_probe()/touch_read()).
 *   - The FT5336 INT line (PI13, EXTI15_10) is NOT used -- this driver polls.
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

/* Per-transaction I2C ceiling; only ever matters when the bus is wedged. */
#define TOUCH_I2C_TIMEOUT_MS 100u

static I2C_HandleTypeDef hts_i2c;   /* I2C3, FT5336                          */
static bool touch_up;               /* touch_init() brought the bus up       */
static bool touch_tried;            /* touch_init() ran (idempotent latch)   */
static TX_MUTEX touch_lock;         /* per-operation serialization           */

/* Locked I2C read helper: register @p reg, @p n bytes into @p buf. */
static int touch_rd(uint8_t reg, uint8_t *buf, uint16_t n)
{
	HAL_StatusTypeDef st;

	if (tx_mutex_get(&touch_lock, TX_WAIT_FOREVER) != TX_SUCCESS)
		return TOUCH_ERR_STATE;
	st = HAL_I2C_Mem_Read(&hts_i2c, TS_I2C_ADDR, reg,
	                      I2C_MEMADD_SIZE_8BIT, buf, n,
	                      TOUCH_I2C_TIMEOUT_MS);
	tx_mutex_put(&touch_lock);
	return (st == HAL_OK) ? TOUCH_OK : TOUCH_ERR_HAL;
}

int touch_init(void)
{
	GPIO_InitTypeDef g = {0};

	if (touch_tried)
		return touch_up ? TOUCH_OK : TOUCH_ERR_HAL;
	touch_tried = true;

	if (tx_mutex_create(&touch_lock, "touch", TX_INHERIT) != TX_SUCCESS)
		return TOUCH_ERR_HAL;

	__HAL_RCC_GPIOH_CLK_ENABLE();
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

	touch_up = true;
	LOG_INF("I2C3 up (PH7/PH8); FT5336 I/O is lazy");
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

	for (uint8_t n = 0; n < count; n++) {
		uint8_t  b[4];                   /* XH, XL, YH, YL       */
		uint16_t rawx, rawy;

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
		st->p[n].x     = rawy;
		st->p[n].y     = rawx;
		st->p[n].event = (uint8_t)((b[0] >> FT5336_EVT_SHIFT) &
		                           FT5336_EVT_MASK);
		st->p[n].id    = (uint8_t)((b[2] >> FT5336_ID_SHIFT) &
		                           FT5336_ID_MASK);
	}

	st->count = count;
	return TOUCH_OK;
}
