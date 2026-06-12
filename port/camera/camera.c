/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    camera.c
 * @brief   B-CAMS-OMV (OV5640) camera driver: power + I2C probe (#39),
 *          DCMI + DMA snapshot capture (#41).
 *
 * See camera.h for the API contract and the hardware facts.  Setup:
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
 *   - DCMI (issue #41): 8-bit parallel, hardware sync, HSYNC=HIGH /
 *     VSYNC=HIGH / PCLK=RISING -- the H747I-DISCO BSP's proven OV5640 values;
 *     OV5640_Init() programs the matching sensor-side polarities.  Pins
 *     (F746G-DISCO P1, all AF13): PA4=HSYNC, PA6=PIXCLK, PG9=VSYNC,
 *     PH9..PH12,PH14=D0..D4, PD3=D5, PE5=D6, PE6=D7.
 *   - DMA2 Stream1/Ch1 (RM0385 Table 26; SD owns Stream3/6), single-shot
 *     DMA_NORMAL: one QVGA RGB565 frame = 153600 B = 38400 words fits one
 *     NDTR (<= 65535), so HAL_DCMI_Start_DMA runs a plain single transfer.
 *
 * Completion model (HAL): when the DMA finishes the frame's words,
 * DCMI_DMAXferCplt arms the DCMI FRAME interrupt; the FRAME ISR then calls
 * HAL_DCMI_FrameEventCallback -> tx_semaphore_put.  Sync/overrun errors and
 * DMA errors all funnel into HAL_DCMI_ErrorCallback.  Same drain/active-gate
 * discipline as the SD driver (port/sd/sd_card.c).
 *
 * Cache coherency: none needed -- the frame buffer lives in the .sdram
 * section, which bsp_init() maps Normal non-cacheable through the MPU
 * (issue #40), so the DMA writes and CPU reads are coherent by construction.
 *
 * Clean-room glue; the ST BSP (stm32746g_discovery_camera.c, H747I BSP) and
 * RM0385/UM1907/UM2779 were used as a register/pin/timing reference only.
 */
#include "camera.h"
#include "sdram.h"

#include "stm32f7xx_hal.h"
#include "tx_api.h"

#include <string.h>

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

/* Settle times (ThreadX ticks = ms).  AEC/AWB needs a few frames after the
   big OV5640_Init register load; a mode switch (live <-> colorbar) needs a
   frame or two to propagate through the pipeline. */
#define CAM_SETTLE_INIT_MS  300u
#define CAM_SETTLE_MODE_MS  100u

/* One QVGA frame at any plausible OV5640 frame rate is well under 1 s. */
#define CAM_XFER_TIMEOUT_TICKS 1000u

static I2C_HandleTypeDef hcam_i2c;     /* I2C1, SCCB to the OV5640      */
static DCMI_HandleTypeDef hdcmi;       /* DCMI, 8-bit parallel capture  */
static DMA_HandleTypeDef hdma_dcmi;    /* DMA2 Stream1/Ch1: DCMI -> mem */
static OV5640_Object_t   ov5640;

static TX_MUTEX     cam_lock;          /* per-operation serialization        */
static TX_SEMAPHORE cam_done;          /* count 0; ISR posts frame complete  */
static volatile int cam_xfer_err;      /* set by HAL_DCMI_ErrorCallback      */
static volatile int cam_xfer_active;   /* 1 between DMA issue and completion */

static int cam_ready;                  /* camera_init() done           */
static int cam_colorbar = -1;          /* last pattern mode; -1 unknown */
static uint32_t cam_frame_gen;         /* bumped per successful capture */
static struct camera_info info;

/* Frame buffer in external SDRAM (.sdram: NOLOAD, MPU non-cacheable, #40).
   DMA-written by DCMI, CPU-read by camera_frame_read -- coherent with no
   cache maintenance because the region never allocates cache lines. */
static uint16_t cam_frame[CAMERA_FRAME_WIDTH * CAMERA_FRAME_HEIGHT]
	__attribute__((aligned(32), section(".sdram")));

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
	cam_colorbar     = -1;
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
	cam_colorbar     = -1;   /* power cycle reset the sensor registers */

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

/* ---- capture (locked helpers, issue #41) ---------------------------------- */

/* Remove any leftover signals so a stale post cannot satisfy the next wait. */
static void drain_done(void)
{
	while (tx_semaphore_get(&cam_done, TX_NO_WAIT) == TX_SUCCESS)
		;
}

/* Program the sensor for QVGA RGB565 (once after each power-up) and select
   the live / colorbar source.  Both paths sleep so AEC/AWB or the pattern
   switch settles before the snapshot. */
static int camera_configure_locked(int colorbar)
{
	if (!info.configured) {
		if (OV5640_Init(&ov5640, OV5640_R320x240, OV5640_RGB565)
		    != OV5640_OK) {
			LOG_ERR("OV5640_Init failed");
			return CAM_ERR_HAL;
		}
		info.configured = 1;
		cam_colorbar    = -1;
		tx_thread_sleep(CAM_SETTLE_INIT_MS);
	}

	if (cam_colorbar != colorbar) {
		if (OV5640_ColorbarModeConfig(&ov5640,
		                              colorbar ? COLORBAR_MODE_ENABLE
		                                       : COLORBAR_MODE_DISABLE)
		    != OV5640_OK) {
			LOG_ERR("OV5640 colorbar config failed");
			return CAM_ERR_HAL;
		}
		cam_colorbar = colorbar;
		tx_thread_sleep(CAM_SETTLE_MODE_MS);
	}
	return 0;
}

static int camera_capture_locked(int colorbar)
{
	int rc;

	if (!sdram_is_up())
		return CAM_ERR_STATE;

	if (!info.powered) {
		rc = camera_probe_locked(NULL);
		if (rc != 0)
			return rc;
	}

	rc = camera_configure_locked(colorbar);
	if (rc != 0)
		return rc;

	info.frame_valid = 0;
	drain_done();
	cam_xfer_err    = 0;
	cam_xfer_active = 1;

	/* Re-arm the error interrupts: HAL_DCMI_Init enabled LINE/VSYNC/ERR/OVR
	   once, but the snapshot FRAME ISR disables them all
	   (stm32f7xx_hal_dcmi.c FRAME handling), and HAL_DCMI_Start_DMA does not
	   re-enable them -- without this, an overrun/sync error on the second and
	   later captures would surface as a timeout instead of CAM_ERR_HAL.
	   LINE/VSYNC stay off (nothing consumes them; they fire per line/frame).
	   Clear stale flags first so an old latched error cannot trip the ISR
	   the moment the interrupt enables. */
	__HAL_DCMI_CLEAR_FLAG(&hdcmi, DCMI_FLAG_ERRRI | DCMI_FLAG_OVRRI |
	                              DCMI_FLAG_FRAMERI | DCMI_FLAG_LINERI |
	                              DCMI_FLAG_VSYNCRI);
	__HAL_DCMI_ENABLE_IT(&hdcmi, DCMI_IT_ERR | DCMI_IT_OVR);

	/* Single transfer: 38400 words <= 65535 NDTR, no double buffering.  No
	   cache maintenance -- cam_frame is in the MPU non-cacheable SDRAM. */
	if (HAL_DCMI_Start_DMA(&hdcmi, DCMI_MODE_SNAPSHOT, (uint32_t)cam_frame,
	                       CAMERA_FRAME_BYTES / 4u) != HAL_OK) {
		cam_xfer_active = 0;
		(void)HAL_DCMI_Stop(&hdcmi);
		drain_done();
		LOG_ERR("DCMI start failed (err 0x%lx)",
		        (unsigned long)HAL_DCMI_GetError(&hdcmi));
		return CAM_ERR_HAL;
	}

	if (tx_semaphore_get(&cam_done, CAM_XFER_TIMEOUT_TICKS) != TX_SUCCESS) {
		cam_xfer_active = 0;
		(void)HAL_DCMI_Stop(&hdcmi);
		drain_done();
		LOG_ERR("frame timed out (no DCMI sync? check wiring)");
		return CAM_ERR_TIMEOUT;
	}
	cam_xfer_active = 0;

	if (cam_xfer_err) {
		(void)HAL_DCMI_Stop(&hdcmi);
		drain_done();
		LOG_ERR("capture error (HAL err 0x%lx)",
		        (unsigned long)HAL_DCMI_GetError(&hdcmi));
		return CAM_ERR_HAL;
	}

	/* Snapshot auto-cleared CAPTURE; Stop also disables the DCMI and leaves
	   the HAL in a clean READY state for the next capture. */
	(void)HAL_DCMI_Stop(&hdcmi);
	cam_frame_gen++;        /* new pixels: multi-call readers must notice */
	info.frame_valid = 1;
	return 0;
}

/* ---- public API ----------------------------------------------------------- */

int camera_capture(int colorbar)
{
	int rc = op_lock();
	if (rc != 0)
		return rc;
	rc = camera_capture_locked(colorbar != 0);
	op_unlock();
	return rc;
}

int camera_frame_read(uint32_t offset, void *dst, uint32_t len,
                      uint32_t *gen)
{
	int rc;

	if (dst == NULL || len == 0u)
		return CAM_ERR_PARAM;
	/* Subtraction form: offset + len cannot wrap. */
	if (offset >= CAMERA_FRAME_BYTES || len > CAMERA_FRAME_BYTES - offset)
		return CAM_ERR_PARAM;

	rc = op_lock();
	if (rc != 0)
		return rc;
	if (!info.frame_valid) {
		op_unlock();
		return CAM_ERR_NO_FRAME;
	}
	memcpy(dst, (const uint8_t *)cam_frame + offset, len);
	if (gen != NULL)
		*gen = cam_frame_gen;
	op_unlock();
	return 0;
}

void camera_frame_invalidate(void)
{
	if (op_lock() != 0)
		return;
	info.frame_valid = 0;
	op_unlock();
}

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
	if (tx_semaphore_create(&cam_done, "cam_done", 0) != TX_SUCCESS) {
		tx_mutex_delete(&cam_lock);
		return CAM_ERR_STATE;
	}

	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOB_CLK_ENABLE();
	__HAL_RCC_GPIOD_CLK_ENABLE();
	__HAL_RCC_GPIOE_CLK_ENABLE();
	__HAL_RCC_GPIOG_CLK_ENABLE();
	__HAL_RCC_GPIOH_CLK_ENABLE();
	__HAL_RCC_I2C1_CLK_ENABLE();
	__HAL_RCC_DCMI_CLK_ENABLE();
	__HAL_RCC_DMA2_CLK_ENABLE();

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
		LOG_ERR("I2C1 init failed");
		goto fail;
	}

	/* DCMI pins (P1, AF13): PA4=HSYNC, PA6=PIXCLK, PG9=VSYNC, PD3=D5,
	   PE5=D6, PE6=D7, PH9..PH12,PH14=D0..D4.  Pull-up like the ST BSP. */
	g.Mode      = GPIO_MODE_AF_PP;
	g.Pull      = GPIO_PULLUP;
	g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
	g.Alternate = GPIO_AF13_DCMI;
	g.Pin       = GPIO_PIN_4 | GPIO_PIN_6;
	HAL_GPIO_Init(GPIOA, &g);
	g.Pin       = GPIO_PIN_3;
	HAL_GPIO_Init(GPIOD, &g);
	g.Pin       = GPIO_PIN_5 | GPIO_PIN_6;
	HAL_GPIO_Init(GPIOE, &g);
	g.Pin       = GPIO_PIN_9;
	HAL_GPIO_Init(GPIOG, &g);
	g.Pin       = GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 |
	              GPIO_PIN_14;
	HAL_GPIO_Init(GPIOH, &g);

	/* DMA2 Stream1/Ch1: DCMI -> memory, single-shot per frame (DMA_NORMAL).
	   32-bit words both sides (the DCMI DR packs four 8-bit pixels), FIFO on
	   with INC4 memory bursts; the peripheral side stays single-beat reads
	   of the one DR register. */
	hdma_dcmi.Instance                 = DMA2_Stream1;
	hdma_dcmi.Init.Channel             = DMA_CHANNEL_1;
	hdma_dcmi.Init.Direction           = DMA_PERIPH_TO_MEMORY;
	hdma_dcmi.Init.PeriphInc           = DMA_PINC_DISABLE;
	hdma_dcmi.Init.MemInc              = DMA_MINC_ENABLE;
	hdma_dcmi.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
	hdma_dcmi.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
	hdma_dcmi.Init.Mode                = DMA_NORMAL;
	hdma_dcmi.Init.Priority            = DMA_PRIORITY_HIGH;
	hdma_dcmi.Init.FIFOMode            = DMA_FIFOMODE_ENABLE;
	hdma_dcmi.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
	hdma_dcmi.Init.MemBurst            = DMA_MBURST_INC4;
	hdma_dcmi.Init.PeriphBurst         = DMA_PBURST_SINGLE;
	if (HAL_DMA_Init(&hdma_dcmi) != HAL_OK) {
		LOG_ERR("DCMI DMA init failed");
		goto fail_i2c;
	}
	__HAL_LINKDMA(&hdcmi, DMA_Handle, hdma_dcmi);

	/* DCMI: 8-bit, hardware sync, the H747I BSP's proven OV5640 polarities
	   (OV5640_Init programs the matching sensor side). */
	hdcmi.Instance               = DCMI;
	hdcmi.Init.SynchroMode       = DCMI_SYNCHRO_HARDWARE;
	hdcmi.Init.PCKPolarity       = DCMI_PCKPOLARITY_RISING;
	hdcmi.Init.VSPolarity        = DCMI_VSPOLARITY_HIGH;
	hdcmi.Init.HSPolarity        = DCMI_HSPOLARITY_HIGH;
	hdcmi.Init.CaptureRate       = DCMI_CR_ALL_FRAME;
	hdcmi.Init.ExtendedDataMode  = DCMI_EXTEND_DATA_8B;
	hdcmi.Init.JPEGMode          = DCMI_JPEG_DISABLE;
	if (HAL_DCMI_Init(&hdcmi) != HAL_OK) {
		(void)HAL_DMA_DeInit(&hdma_dcmi);
		LOG_ERR("DCMI init failed");
		goto fail_i2c;
	}

	/* NVIC: below USART1 (5) / SDMMC1 (6) / SD DMA (7), above SysTick (14).
	   The ThreadX port masks with PRIMASK, so tx_semaphore_put from these
	   ISRs is safe whatever the numeric priority. */
	HAL_NVIC_SetPriority(DCMI_IRQn, 8, 0);
	HAL_NVIC_EnableIRQ(DCMI_IRQn);
	HAL_NVIC_SetPriority(DMA2_Stream1_IRQn, 8, 0);
	HAL_NVIC_EnableIRQ(DMA2_Stream1_IRQn);

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
		(void)HAL_DCMI_DeInit(&hdcmi);
		(void)HAL_DMA_DeInit(&hdma_dcmi);
		LOG_ERR("OV5640 bus registration failed");
		goto fail_i2c;
	}

	cam_ready = 1;
	LOG_INF("I2C1 + DCMI/DMA2-S1 up; sensor I/O is lazy");
	return 0;

fail_i2c:
	(void)HAL_I2C_DeInit(&hcam_i2c);
fail:
	tx_semaphore_delete(&cam_done);
	tx_mutex_delete(&cam_lock);
	return CAM_ERR_HAL;
}

/* ---- ISRs + HAL completion callbacks ------------------------------------- */
/*
 * Strong overrides of the CMSIS weak vectors, wrapped in the ThreadX
 * execution-profile enter/exit exactly like the SD driver's (sd_card.c):
 * these only fire during a capture, which is started from a shell thread
 * long after the profile kit is armed.
 */
void DCMI_IRQHandler(void)
{
#if defined(TX_EXECUTION_PROFILE_ENABLE)
	{ uint32_t pm = __get_PRIMASK(); __disable_irq();
	  _tx_execution_isr_enter(); __set_PRIMASK(pm); }
#endif
	HAL_DCMI_IRQHandler(&hdcmi);
#if defined(TX_EXECUTION_PROFILE_ENABLE)
	{ uint32_t pm = __get_PRIMASK(); __disable_irq();
	  _tx_execution_isr_exit(); __set_PRIMASK(pm); }
#endif
}

void DMA2_Stream1_IRQHandler(void)
{
#if defined(TX_EXECUTION_PROFILE_ENABLE)
	{ uint32_t pm = __get_PRIMASK(); __disable_irq();
	  _tx_execution_isr_enter(); __set_PRIMASK(pm); }
#endif
	HAL_DMA_IRQHandler(&hdma_dcmi);
#if defined(TX_EXECUTION_PROFILE_ENABLE)
	{ uint32_t pm = __get_PRIMASK(); __disable_irq();
	  _tx_execution_isr_exit(); __set_PRIMASK(pm); }
#endif
}

/* Frame complete: the DMA finished the frame's words (DCMI_DMAXferCplt armed
   the FRAME interrupt) and the DCMI saw the frame end.  Posted only while a
   capture is in flight; a late post after a timeout/stop is suppressed by
   the cam_xfer_active gate and removed by the next drain. */
void HAL_DCMI_FrameEventCallback(DCMI_HandleTypeDef *h)
{
	(void)h;
	if (!cam_xfer_active)
		return;
	(void)tx_semaphore_put(&cam_done);
}

/* Sync error, overrun and DMA errors all funnel here via the HAL. */
void HAL_DCMI_ErrorCallback(DCMI_HandleTypeDef *h)
{
	(void)h;
	if (!cam_xfer_active)
		return;
	cam_xfer_err = 1;
	(void)tx_semaphore_put(&cam_done);
}
