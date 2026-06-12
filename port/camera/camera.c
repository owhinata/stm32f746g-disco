/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    camera.c
 * @brief   B-CAMS-OMV (OV5640) camera driver -- Phase 1: power + I2C probe.
 *
 * See camera.h for the API contract and the hardware facts.  Phase 1 setup:
 *
 *   - I2C1 on PB8 (SCL) / PB9 (SDA), AF4 open-drain.  The OV5640 SCCB bus runs
 *     at ~100 kHz standard mode; TIMINGR is computed for PCLK1 = 54 MHz (the
 *     ST BSP constant 0x40912732 assumes 50 MHz and would land at ~118 kHz).
 *   - PWR_EN on PH13, LOW = camera powered (ST BSP semantics: the pin is the
 *     module's POWER_DOWN, active high).  It is parked HIGH (off) before the
 *     pin is switched to output so the module never sees a power glitch.
 *   - The OV5640 component driver (lib/ov5640) does all sensor register I/O
 *     through the OV5640_IO_t bus glue below: HAL_I2C_Mem_* with 16-bit
 *     register addresses at I2C address 0x78.
 *
 * No DCMI/DMA here -- the capture path is issue #41.
 *
 * Clean-room glue; the ST BSP (stm32746g_discovery_camera.c, H747I BSP) and
 * RM0385/UM1907/UM2779 were used as a register/pin/timing reference only.
 */
#include "camera.h"

#include "stm32f7xx_hal.h"
#include "tx_api.h"

#include "ov5640.h"

#define LOG_TAG "camera"
#include "log.h"

/* PWR_EN = PH13 (UM1907 Table 13: DCMI_PWR_EN).  LOW = powered. */
#define CAM_PWR_PORT GPIOH
#define CAM_PWR_PIN  GPIO_PIN_13

/* OV5640 SCCB write address (B-CAMS-OMV MB1379; H747I BSP CAMERA_OV5640_ADDRESS). */
#define CAM_I2C_ADDR 0x78u

/* Per-register I2C transaction ceiling.  SCCB ops are 3-4 bytes; 100 ms only
   ever matters when the bus is wedged. */
#define CAM_I2C_TIMEOUT_MS 100u

/*
 * I2C1 TIMINGR for 100 kHz standard mode at I2CCLK = PCLK1 = 54 MHz
 * (RM0385 30.4.10 / 30.7.5, tI2CCLK = 18.52 ns):
 *
 *   PRESC  = 11 -> tPRESC = 12 x 18.52 ns = 222.2 ns
 *   SCLL   = 24 -> tLOW  = 25 x 222.2 ns = 5.56 us  (>= 4.7 us SM minimum)
 *   SCLH   = 19 -> tHIGH = 20 x 222.2 ns = 4.44 us  (>= 4.0 us SM minimum)
 *   SCLDEL =  5 -> data setup  6 x 222.2 ns = 1.33 us (>= tr 1.0 us + tSU;DAT 0.25 us)
 *   SDADEL =  2 -> data hold   2 x 222.2 ns = 0.44 us (within 0 .. tHD;DAT max)
 *
 *   SCL ~= 1 / (5.56 us + 4.44 us + sync) ~= 99 kHz
 */
#define CAM_I2C_TIMING ((11u << 28) | (5u << 20) | (2u << 16) | (19u << 8) | 24u)

static I2C_HandleTypeDef hcam_i2c;     /* I2C1, SCCB to the OV5640 */
static OV5640_Object_t   ov5640;

static TX_MUTEX cam_lock;              /* per-operation serialization */

static int cam_ready;                  /* camera_init() done           */
static struct camera_info info;

/* ---- locking ------------------------------------------------------------ */
/* Public API entries take the mutex here; all real work below lives in
   *_locked() helpers so no path ever re-acquires cam_lock (a capture in #41
   probes on demand by calling the _locked helper directly). */

static int op_lock(void)
{
	if (!cam_ready)
		return CAM_ERR_STATE;
	if (tx_mutex_get(&cam_lock, TX_WAIT_FOREVER) != TX_SUCCESS)
		return CAM_ERR_STATE;
	return 0;
}

static void op_unlock(void)
{
	tx_mutex_put(&cam_lock);
}

/* ---- OV5640 bus glue (OV5640_IO_t) --------------------------------------- */

/* OV5640_ReadID() calls IO.Init() unconditionally, so a real (if empty)
   function is mandatory; the peripheral is already up from camera_init(). */
static int32_t cam_io_init(void)
{
	return OV5640_OK;
}

static int32_t cam_io_deinit(void)
{
	return OV5640_OK;
}

static int32_t cam_io_gettick(void)
{
	return (int32_t)HAL_GetTick();
}

static int32_t cam_io_write(uint16_t addr, uint16_t reg, uint8_t *data,
                            uint16_t len)
{
	if (HAL_I2C_Mem_Write(&hcam_i2c, addr, reg, I2C_MEMADD_SIZE_16BIT,
	                      data, len, CAM_I2C_TIMEOUT_MS) != HAL_OK)
		return OV5640_ERROR;
	return OV5640_OK;
}

static int32_t cam_io_read(uint16_t addr, uint16_t reg, uint8_t *data,
                           uint16_t len)
{
	if (HAL_I2C_Mem_Read(&hcam_i2c, addr, reg, I2C_MEMADD_SIZE_16BIT,
	                     data, len, CAM_I2C_TIMEOUT_MS) != HAL_OK)
		return OV5640_ERROR;
	return OV5640_OK;
}

/* ---- power + probe (locked helpers) -------------------------------------- */

static void power_off_locked(void)
{
	HAL_GPIO_WritePin(CAM_PWR_PORT, CAM_PWR_PIN, GPIO_PIN_SET);
	info.chip_id     = 0;
	info.powered     = 0;
	info.configured  = 0;
	info.frame_valid = 0;
}

/* PH13 high 100 ms -> low, then settle: the H747I BSP HwReset timing for the
   same module family.  Always a full cycle so a re-probe recovers a wedged
   sensor (there is no GPIO reset line -- DCMI_NRST is on the board NRST net). */
static void power_cycle_locked(void)
{
	HAL_GPIO_WritePin(CAM_PWR_PORT, CAM_PWR_PIN, GPIO_PIN_SET);
	tx_thread_sleep(100);
	HAL_GPIO_WritePin(CAM_PWR_PORT, CAM_PWR_PIN, GPIO_PIN_RESET);
	tx_thread_sleep(20);
}

static int camera_probe_locked(uint32_t *chip_id)
{
	uint32_t id = 0;

	power_cycle_locked();
	info.configured  = 0;
	info.frame_valid = 0;

	/* OV5640_ReadID software-resets the sensor and waits 500 ms (GetTick
	   poll) before reading 0x300A/0x300B. */
	if (OV5640_ReadID(&ov5640, &id) != OV5640_OK) {
		power_off_locked();
		LOG_WRN("no I2C response at 0x%02x (module connected?)",
		        CAM_I2C_ADDR);
		return CAM_ERR_NO_SENSOR;
	}
	if (id != OV5640_ID) {
		power_off_locked();
		LOG_ERR("unexpected chip ID 0x%04lx (want 0x%04x)",
		        (unsigned long)id, OV5640_ID);
		return CAM_ERR_NO_SENSOR;
	}

	info.chip_id = id;
	info.powered = 1;
	if (chip_id != NULL)
		*chip_id = id;
	LOG_INF("OV5640 up: chip ID 0x%04lx", (unsigned long)id);
	return 0;
}

/* ---- public API ----------------------------------------------------------- */

int camera_probe(uint32_t *chip_id)
{
	int rc = op_lock();
	if (rc != 0)
		return rc;
	rc = camera_probe_locked(chip_id);
	op_unlock();
	return rc;
}

int camera_power_off(void)
{
	int rc = op_lock();
	if (rc != 0)
		return rc;
	power_off_locked();
	op_unlock();
	return 0;
}

int camera_get_info(struct camera_info *out)
{
	int rc;

	if (out == NULL)
		return CAM_ERR_PARAM;
	rc = op_lock();
	if (rc != 0)
		return rc;
	*out = info;
	op_unlock();
	return 0;
}

int camera_init(void)
{
	GPIO_InitTypeDef g = {0};
	OV5640_IO_t io;

	if (cam_ready)
		return 0;

	if (tx_mutex_create(&cam_lock, "camera", TX_INHERIT) != TX_SUCCESS)
		return CAM_ERR_STATE;

	__HAL_RCC_GPIOB_CLK_ENABLE();
	__HAL_RCC_GPIOH_CLK_ENABLE();
	__HAL_RCC_I2C1_CLK_ENABLE();

	/* PWR_EN: drive the OFF level while the pin is still an input so the
	   output never glitches the module on. */
	HAL_GPIO_WritePin(CAM_PWR_PORT, CAM_PWR_PIN, GPIO_PIN_SET);
	g.Pin   = CAM_PWR_PIN;
	g.Mode  = GPIO_MODE_OUTPUT_PP;
	g.Pull  = GPIO_NOPULL;
	g.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(CAM_PWR_PORT, &g);

	/* I2C1: PB8 = SCL, PB9 = SDA (UM1907 CN2/P1), AF4 open-drain.  The bus
	   has 4.7k pull-ups on the board. */
	g.Pin       = GPIO_PIN_8 | GPIO_PIN_9;
	g.Mode      = GPIO_MODE_AF_OD;
	g.Pull      = GPIO_NOPULL;
	g.Speed     = GPIO_SPEED_FREQ_HIGH;
	g.Alternate = GPIO_AF4_I2C1;
	HAL_GPIO_Init(GPIOB, &g);

	hcam_i2c.Instance              = I2C1;
	hcam_i2c.Init.Timing           = CAM_I2C_TIMING;
	hcam_i2c.Init.OwnAddress1      = 0;
	hcam_i2c.Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
	hcam_i2c.Init.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
	hcam_i2c.Init.OwnAddress2      = 0;
	hcam_i2c.Init.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
	hcam_i2c.Init.NoStretchMode    = I2C_NOSTRETCH_DISABLE;
	if (HAL_I2C_Init(&hcam_i2c) != HAL_OK) {
		tx_mutex_delete(&cam_lock);
		LOG_ERR("I2C1 init failed");
		return CAM_ERR_HAL;
	}

	/* Bind the component driver to this bus once; Address is the 8-bit
	   write address the HAL Mem API expects. */
	io.Init      = cam_io_init;
	io.DeInit    = cam_io_deinit;
	io.Address   = CAM_I2C_ADDR;
	io.WriteReg  = cam_io_write;
	io.ReadReg   = cam_io_read;
	io.ModifyReg = NULL;
	io.GetTick   = cam_io_gettick;
	if (OV5640_RegisterBusIO(&ov5640, &io) != OV5640_OK) {
		(void)HAL_I2C_DeInit(&hcam_i2c);
		tx_mutex_delete(&cam_lock);
		LOG_ERR("OV5640 bus registration failed");
		return CAM_ERR_HAL;
	}

	cam_ready = 1;
	LOG_INF("I2C1 up (PB8/PB9, ~100 kHz); sensor I/O is lazy");
	return 0;
}
