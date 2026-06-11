/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    sd_card.c
 * @brief   microSD low-level driver over SDMMC1 + DMA2 (issue #33).
 *
 * See sd_card.h for the API contract.  Hardware setup:
 *
 *   - SDMMC1 clocked from CLK48 (48 MHz PLLQ, configured in bsp.c).  ClockDiv=0
 *     gives the transfer clock SDMMC_CK = 48 MHz / (0 + 2) = 24 MHz (RM0385
 *     35.8.2); identification runs at <400 kHz via the HAL's internal divider.
 *   - Pins (UM1907 CN3, all AF12): PC8=D0, PC9=D1, PC10=D2, PC11=D3, PC12=CK,
 *     PD2=CMD.  Card detect = PC13, input pull-up, active low (low = inserted).
 *   - DMA2: Rx on Stream3/Ch4, Tx on Stream6/Ch4 (RM0385 Table 26), SDMMC as
 *     the flow controller (PFCTRL), 32-bit words, INC4 bursts.
 *
 * Completion model: HAL_SD runs the data phase on DMA and signals completion
 * asynchronously -- a read finishes in the DMA Rx ISR (SD_DMAReceiveCplt ->
 * HAL_SD_RxCpltCallback), a write finishes when the DMA Tx ISR arms DATAEND and
 * the SDMMC1 ISR then fires (HAL_SD_TxCpltCallback).  Both ISRs post a count-0
 * semaphore that the calling thread waits on with a finite timeout.  A stale or
 * late callback after an aborted/timed-out transfer cannot fake success: the
 * callback only posts while sd_xfer_active is set, and every operation drains
 * the semaphore before it starts.
 *
 * Cache coherency: the DMA only ever touches sd_bounce, a 32 B-aligned buffer in
 * SRAM1 (.sram1_dma).  Reads invalidate it around the DMA and memcpy out; writes
 * memcpy in and clean it before the DMA.  The caller's buffer is never handed to
 * the DMA, so any alignment is safe and no adjacent cache line is disturbed.
 *
 * Clean-room implementation; ST BSP / RM0385 used as a register reference only.
 */
#include "sd_card.h"

#include "stm32f7xx_hal.h"
#include "tx_api.h"

#include <string.h>

#define LOG_TAG "sd"
#include "log.h"

/* Card detect: PC13, active low (UM1907 CN3 + ST BSP). */
#define SD_DETECT_PORT   GPIOC
#define SD_DETECT_PIN    GPIO_PIN_13

/* Bounce buffer span: 8 sectors = 4 KiB per DMA chunk (matches .sram1_dma). */
#define SD_BOUNCE_BLOCKS 8u

/* Wait ceilings.  State wait spins on HAL_GetTick (ms); the DMA wait uses the
   ThreadX tick (1 ms here, TX_GLUE_TICK_DIV=1). */
#define SD_STATE_TIMEOUT_MS    1000u
#define SD_XFER_TIMEOUT_TICKS  2000u

static SD_HandleTypeDef  hsd;
static DMA_HandleTypeDef hdma_rx;   /* DMA2 Stream3 Ch4: card -> memory */
static DMA_HandleTypeDef hdma_tx;   /* DMA2 Stream6 Ch4: memory -> card */

static TX_MUTEX     sd_lock;        /* per-operation serialization        */
static TX_SEMAPHORE sd_done;        /* count 0; ISR posts on DMA complete */
static volatile int sd_xfer_err;    /* set by HAL_SD_ErrorCallback        */
static volatile int sd_xfer_active; /* 1 between DMA issue and completion  */

static int sd_ready;                /* sd_card_init() done                */
static int sd_probed;               /* sd_card_probe() succeeded          */
static struct sd_card_info info;

/* DMA bounce buffer: 32 B aligned, SRAM1 (linker .sram1_dma, ASSERTed there). */
static uint8_t sd_bounce[SD_BOUNCE_BLOCKS * SD_BLOCK_SIZE]
	__attribute__((aligned(32), section(".sram1_dma")));

const struct sd_card_info *sd_card_get_info(void)
{
	return &info;
}

int sd_card_is_present(void)
{
	return HAL_GPIO_ReadPin(SD_DETECT_PORT, SD_DETECT_PIN) == GPIO_PIN_RESET;
}

/* ---- locking ------------------------------------------------------------ */

static int op_lock(void)
{
	if (!sd_ready)
		return SD_ERR_STATE;
	if (tx_mutex_get(&sd_lock, TX_WAIT_FOREVER) != TX_SUCCESS)
		return SD_ERR_STATE;
	return 0;
}

static void op_unlock(void)
{
	tx_mutex_put(&sd_lock);
}

/* ---- DMA completion plumbing (all run under the operation mutex) --------- */

/* Remove any leftover signals so a stale post cannot satisfy the next wait. */
static void drain_done(void)
{
	while (tx_semaphore_get(&sd_done, TX_NO_WAIT) == TX_SUCCESS)
		;
}

/* Wait for the card to return to the data-transfer (tran) state.  CMD13 over
   the bus; yields between polls so other threads keep running. */
static int wait_transfer_state(void)
{
	uint32_t start = HAL_GetTick();

	for (;;) {
		if (HAL_SD_GetCardState(&hsd) == HAL_SD_CARD_TRANSFER)
			return 0;
		if (!sd_card_is_present())
			return SD_ERR_NO_CARD;
		if ((HAL_GetTick() - start) >= SD_STATE_TIMEOUT_MS)
			return SD_ERR_TIMEOUT;
		tx_thread_sleep(1);
	}
}

/*
 * Wait for the in-flight DMA to complete.  On timeout or a reported transfer
 * error, stop the controller (HAL_SD_Abort synchronously clears the SDMMC IT/
 * flags and aborts the DMA) and drain any signal the abort raced with.  Clears
 * sd_xfer_active first so a late callback after the abort is a no-op.
 */
static int wait_done(void)
{
	if (tx_semaphore_get(&sd_done, SD_XFER_TIMEOUT_TICKS) != TX_SUCCESS) {
		sd_xfer_active = 0;
		(void)HAL_SD_Abort(&hsd);
		drain_done();
		LOG_ERR("DMA transfer timed out");
		return SD_ERR_TIMEOUT;
	}

	sd_xfer_active = 0;

	if (sd_xfer_err) {
		(void)HAL_SD_Abort(&hsd);
		drain_done();
		LOG_ERR("DMA transfer error (HAL err 0x%lx)",
		        (unsigned long)HAL_SD_GetError(&hsd));
		return SD_ERR_HAL;
	}
	return 0;
}

/* ---- public API ---------------------------------------------------------- */

int sd_card_read_blocks(uint32_t lba, void *buf, uint32_t count)
{
	uint8_t *dst = buf;
	int rc;

	if (buf == NULL || count == 0u)
		return SD_ERR_PARAM;

	rc = op_lock();
	if (rc != 0)
		return rc;
	if (!sd_probed) {
		op_unlock();
		return SD_ERR_STATE;
	}
	/* Range-check against the card before any CMD: HAL only tests
	   (lba + count) > LogBlockNbr, which wraps, so guard the wrap here. */
	if (lba >= info.block_count || count > info.block_count - lba) {
		op_unlock();
		return SD_ERR_PARAM;
	}
	if (!sd_card_is_present()) {
		op_unlock();
		return SD_ERR_NO_CARD;
	}

	while (count > 0u) {
		uint32_t c   = (count > SD_BOUNCE_BLOCKS) ? SD_BOUNCE_BLOCKS : count;
		uint32_t len = c * SD_BLOCK_SIZE;

		rc = wait_transfer_state();
		if (rc != 0)
			break;

		/* Discard any cached lines over the bounce before the DMA writes it:
		   a dirty eviction mid-transfer would corrupt the DMA'd data. */
		SCB_InvalidateDCache_by_Addr(sd_bounce, (int32_t)len);

		drain_done();
		sd_xfer_err    = 0;
		sd_xfer_active = 1;
		if (HAL_SD_ReadBlocks_DMA(&hsd, sd_bounce, lba, c) != HAL_OK) {
			sd_xfer_active = 0;
			(void)HAL_SD_Abort(&hsd);
			drain_done();
			rc = SD_ERR_HAL;
			break;
		}

		rc = wait_done();
		if (rc != 0)
			break;

		/* Drop speculative prefetches made during the DMA, then copy out. */
		SCB_InvalidateDCache_by_Addr(sd_bounce, (int32_t)len);
		memcpy(dst, sd_bounce, len);

		lba   += c;
		dst   += len;
		count -= c;
	}

	op_unlock();
	return rc;
}

int sd_card_write_blocks(uint32_t lba, const void *buf, uint32_t count)
{
	const uint8_t *src = buf;
	int rc;

	if (buf == NULL || count == 0u)
		return SD_ERR_PARAM;

	rc = op_lock();
	if (rc != 0)
		return rc;
	if (!sd_probed) {
		op_unlock();
		return SD_ERR_STATE;
	}
	if (lba >= info.block_count || count > info.block_count - lba) {
		op_unlock();
		return SD_ERR_PARAM;
	}
	if (!sd_card_is_present()) {
		op_unlock();
		return SD_ERR_NO_CARD;
	}

	while (count > 0u) {
		uint32_t c   = (count > SD_BOUNCE_BLOCKS) ? SD_BOUNCE_BLOCKS : count;
		uint32_t len = c * SD_BLOCK_SIZE;

		rc = wait_transfer_state();
		if (rc != 0)
			break;

		memcpy(sd_bounce, src, len);
		/* Flush the bounce to physical SRAM so the DMA reads what we wrote. */
		SCB_CleanDCache_by_Addr((uint32_t *)sd_bounce, (int32_t)len);

		drain_done();
		sd_xfer_err    = 0;
		sd_xfer_active = 1;
		if (HAL_SD_WriteBlocks_DMA(&hsd, sd_bounce, lba, c) != HAL_OK) {
			sd_xfer_active = 0;
			(void)HAL_SD_Abort(&hsd);
			drain_done();
			rc = SD_ERR_HAL;
			break;
		}

		rc = wait_done();
		if (rc != 0)
			break;

		lba   += c;
		src   += len;
		count -= c;
	}

	/* Make sure the card finished programming before reporting success, so the
	   written data is durable once this returns. */
	if (rc == 0)
		rc = wait_transfer_state();

	op_unlock();
	return rc;
}

int sd_card_probe(void)
{
	HAL_SD_CardInfoTypeDef ci;
	uint32_t widemode;
	int i, rc;

	rc = op_lock();
	if (rc != 0)
		return rc;

	if (!sd_card_is_present()) {
		op_unlock();
		return SD_ERR_NO_CARD;
	}

	/* Re-identify from scratch every probe so a swapped card is picked up. */
	if (hsd.State != HAL_SD_STATE_RESET)
		(void)HAL_SD_DeInit(&hsd);

	hsd.Instance                 = SDMMC1;
	hsd.Init.ClockEdge           = SDMMC_CLOCK_EDGE_RISING;
	hsd.Init.ClockBypass         = SDMMC_CLOCK_BYPASS_DISABLE;
	hsd.Init.ClockPowerSave      = SDMMC_CLOCK_POWER_SAVE_DISABLE;
	hsd.Init.BusWide             = SDMMC_BUS_WIDE_1B;   /* identify in 1-bit */
	hsd.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
	hsd.Init.ClockDiv            = 0;                   /* transfer = 24 MHz */

	sd_probed = 0;

	if (HAL_SD_Init(&hsd) != HAL_OK) {
		LOG_ERR("HAL_SD_Init failed (err 0x%lx)",
		        (unsigned long)HAL_SD_GetError(&hsd));
		op_unlock();
		return SD_ERR_HAL;
	}

	/* HAL_SD_Init leaves SDMMC_CK at the 400 kHz identification divider; the
	   transfer clock (ClockDiv=0 -> 24 MHz) and the bus width only take effect
	   when ConfigWideBusOperation re-runs SDMMC_Init.  Call it on every path. */
#ifdef SD_BUS_WIDE_1B
	widemode = SDMMC_BUS_WIDE_1B;
#else
	widemode = SDMMC_BUS_WIDE_4B;
#endif
	if (HAL_SD_ConfigWideBusOperation(&hsd, widemode) != HAL_OK) {
		/* A failed 4-bit attempt leaves hsd.ErrorCode sticky:
		   HAL_SD_ConfigWideBusOperation OR-accumulates into ErrorCode and never
		   clears it at entry, so any later call sees the old error, returns
		   HAL_ERROR and -- worse -- skips the SDMMC_Init that applies the 24 MHz
		   transfer clock.  Reset it before retrying 1-bit. */
		hsd.ErrorCode = HAL_SD_ERROR_NONE;
		if (widemode == SDMMC_BUS_WIDE_4B &&
		    HAL_SD_ConfigWideBusOperation(&hsd, SDMMC_BUS_WIDE_1B) == HAL_OK) {
			widemode = SDMMC_BUS_WIDE_1B;
			LOG_WRN("4-bit bus failed; running 1-bit");
		} else {
			op_unlock();
			return SD_ERR_HAL;
		}
	}

	(void)HAL_SD_GetCardInfo(&hsd, &ci);
	memset(&info, 0, sizeof info);
	info.type           = ci.CardType;
	info.version        = ci.CardVersion;
	info.card_class     = ci.Class;
	info.rca            = ci.RelCardAdd;
	info.block_count    = ci.LogBlockNbr;
	info.block_size     = ci.LogBlockSize;
	info.bus_width      = (widemode == SDMMC_BUS_WIDE_4B) ? 4u : 1u;
	info.capacity_bytes = (uint64_t)ci.LogBlockNbr * ci.LogBlockSize;
	for (i = 0; i < 4; i++) {
		info.cid[i] = hsd.CID[i];
		info.csd[i] = hsd.CSD[i];
	}

	sd_probed = 1;
	LOG_INF("card up: %lu MiB, %u-bit",
	        (unsigned long)(info.capacity_bytes / (1024u * 1024u)),
	        (unsigned)info.bus_width);
	op_unlock();
	return 0;
}

int sd_card_deinit(void)
{
	int rc = op_lock();
	if (rc != 0)
		return rc;
	if (hsd.State != HAL_SD_STATE_RESET)
		(void)HAL_SD_DeInit(&hsd);
	sd_probed = 0;
	op_unlock();
	return 0;
}

int sd_card_status(void)
{
	int rc = op_lock();
	if (rc != 0)
		return rc;
	if (!sd_probed) {
		op_unlock();
		return SD_ERR_STATE;
	}
	if (!sd_card_is_present()) {
		op_unlock();
		return SD_ERR_NO_CARD;
	}
	rc = (HAL_SD_GetCardState(&hsd) == HAL_SD_CARD_TRANSFER)
	     ? SD_OK : SD_ERR_TIMEOUT;
	op_unlock();
	return rc;
}

int sd_card_init(void)
{
	GPIO_InitTypeDef g = {0};

	if (sd_ready)
		return 0;

	if (tx_mutex_create(&sd_lock, "sd", TX_INHERIT) != TX_SUCCESS)
		return SD_ERR_STATE;
	if (tx_semaphore_create(&sd_done, "sd_done", 0) != TX_SUCCESS) {
		tx_mutex_delete(&sd_lock);
		return SD_ERR_STATE;
	}

	/* SDMMC1 clock source = CLK48 (48 MHz PLLQ from bsp.c); data/cmd GPIO banks
	   + card-detect on GPIOC/GPIOD; DMA2 for the data path. */
	__HAL_RCC_SDMMC1_CONFIG(RCC_SDMMC1CLKSOURCE_CLK48);
	__HAL_RCC_GPIOC_CLK_ENABLE();
	__HAL_RCC_GPIOD_CLK_ENABLE();
	__HAL_RCC_DMA2_CLK_ENABLE();
	__HAL_RCC_SDMMC1_CLK_ENABLE();

	/* PC8..PC12 = D0,D1,D2,D3,CK and PD2 = CMD, all AF12, pull-up. */
	g.Mode      = GPIO_MODE_AF_PP;
	g.Pull      = GPIO_PULLUP;
	g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
	g.Alternate = GPIO_AF12_SDMMC1;
	g.Pin       = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12;
	HAL_GPIO_Init(GPIOC, &g);
	g.Pin       = GPIO_PIN_2;
	HAL_GPIO_Init(GPIOD, &g);

	/* PC13 = card detect, input pull-up (active low). */
	g.Mode      = GPIO_MODE_INPUT;
	g.Pull      = GPIO_PULLUP;
	g.Alternate = 0;
	g.Pin       = SD_DETECT_PIN;
	HAL_GPIO_Init(SD_DETECT_PORT, &g);

	/* DMA2 Rx: Stream3/Ch4, periph->mem.  SDMMC is the flow controller; 32-bit
	   words, INC4 bursts, full FIFO (RM0385 35.3.2). */
	hdma_rx.Instance                 = DMA2_Stream3;
	hdma_rx.Init.Channel             = DMA_CHANNEL_4;
	hdma_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
	hdma_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
	hdma_rx.Init.MemInc              = DMA_MINC_ENABLE;
	hdma_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
	hdma_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
	hdma_rx.Init.Mode                = DMA_PFCTRL;
	hdma_rx.Init.Priority            = DMA_PRIORITY_VERY_HIGH;
	hdma_rx.Init.FIFOMode            = DMA_FIFOMODE_ENABLE;
	hdma_rx.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
	hdma_rx.Init.MemBurst            = DMA_MBURST_INC4;
	hdma_rx.Init.PeriphBurst         = DMA_PBURST_INC4;
	if (HAL_DMA_Init(&hdma_rx) != HAL_OK)
		goto fail;
	__HAL_LINKDMA(&hsd, hdmarx, hdma_rx);

	/* DMA2 Tx: Stream6/Ch4, mem->periph; otherwise identical. */
	hdma_tx.Instance                 = DMA2_Stream6;
	hdma_tx.Init.Channel             = DMA_CHANNEL_4;
	hdma_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
	hdma_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
	hdma_tx.Init.MemInc              = DMA_MINC_ENABLE;
	hdma_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
	hdma_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
	hdma_tx.Init.Mode                = DMA_PFCTRL;
	hdma_tx.Init.Priority            = DMA_PRIORITY_VERY_HIGH;
	hdma_tx.Init.FIFOMode            = DMA_FIFOMODE_ENABLE;
	hdma_tx.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
	hdma_tx.Init.MemBurst            = DMA_MBURST_INC4;
	hdma_tx.Init.PeriphBurst         = DMA_PBURST_INC4;
	if (HAL_DMA_Init(&hdma_tx) != HAL_OK)
		goto fail;
	__HAL_LINKDMA(&hsd, hdmatx, hdma_tx);

	/* NVIC: SDMMC1 above USART1 (5), DMA streams just under it; all below
	   SysTick (14).  The port masks with PRIMASK, so tx_semaphore_put from any
	   of these ISRs is safe whatever the numeric priority. */
	HAL_NVIC_SetPriority(SDMMC1_IRQn, 6, 0);
	HAL_NVIC_EnableIRQ(SDMMC1_IRQn);
	HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, 7, 0);
	HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);
	HAL_NVIC_SetPriority(DMA2_Stream6_IRQn, 7, 0);
	HAL_NVIC_EnableIRQ(DMA2_Stream6_IRQn);

	sd_ready = 1;
	LOG_INF("SDMMC1 up: DMA2 S3/S6, 24 MHz xfer clock, card I/O is lazy");
	return 0;

fail:
	tx_semaphore_delete(&sd_done);
	tx_mutex_delete(&sd_lock);
	LOG_ERR("SDMMC1 DMA init failed");
	return SD_ERR_HAL;
}

/* ---- ISRs + HAL completion callbacks ------------------------------------ */
/*
 * Strong overrides of the CMSIS weak vectors.  Each wraps the HAL handler in the
 * ThreadX execution-profile enter/exit (issue #19), PRIMASK-protected exactly
 * like USART1_IRQHandler.  No profile_active gate is needed: these only fire
 * during an SD operation, which can only be started from a shell thread long
 * after the profile kit is armed (mirrors the USART1 backend).
 */
void SDMMC1_IRQHandler(void)
{
#if defined(TX_EXECUTION_PROFILE_ENABLE)
	{ uint32_t pm = __get_PRIMASK(); __disable_irq();
	  _tx_execution_isr_enter(); __set_PRIMASK(pm); }
#endif
	HAL_SD_IRQHandler(&hsd);
#if defined(TX_EXECUTION_PROFILE_ENABLE)
	{ uint32_t pm = __get_PRIMASK(); __disable_irq();
	  _tx_execution_isr_exit(); __set_PRIMASK(pm); }
#endif
}

void DMA2_Stream3_IRQHandler(void)
{
#if defined(TX_EXECUTION_PROFILE_ENABLE)
	{ uint32_t pm = __get_PRIMASK(); __disable_irq();
	  _tx_execution_isr_enter(); __set_PRIMASK(pm); }
#endif
	HAL_DMA_IRQHandler(&hdma_rx);
#if defined(TX_EXECUTION_PROFILE_ENABLE)
	{ uint32_t pm = __get_PRIMASK(); __disable_irq();
	  _tx_execution_isr_exit(); __set_PRIMASK(pm); }
#endif
}

void DMA2_Stream6_IRQHandler(void)
{
#if defined(TX_EXECUTION_PROFILE_ENABLE)
	{ uint32_t pm = __get_PRIMASK(); __disable_irq();
	  _tx_execution_isr_enter(); __set_PRIMASK(pm); }
#endif
	HAL_DMA_IRQHandler(&hdma_tx);
#if defined(TX_EXECUTION_PROFILE_ENABLE)
	{ uint32_t pm = __get_PRIMASK(); __disable_irq();
	  _tx_execution_isr_exit(); __set_PRIMASK(pm); }
#endif
}

/* Posted only while an operation is in flight; a late post after abort is
   suppressed by the sd_xfer_active gate and removed by the next drain. */
void HAL_SD_RxCpltCallback(SD_HandleTypeDef *h)
{
	(void)h;
	if (!sd_xfer_active)
		return;
	(void)tx_semaphore_put(&sd_done);
}

void HAL_SD_TxCpltCallback(SD_HandleTypeDef *h)
{
	(void)h;
	if (!sd_xfer_active)
		return;
	(void)tx_semaphore_put(&sd_done);
}

void HAL_SD_ErrorCallback(SD_HandleTypeDef *h)
{
	(void)h;
	if (!sd_xfer_active)
		return;
	sd_xfer_err = 1;
	(void)tx_semaphore_put(&sd_done);
}
