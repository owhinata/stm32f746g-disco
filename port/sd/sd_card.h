/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    sd_card.h
 * @brief   microSD low-level driver over SDMMC1 + DMA (issue #33, Epic #32).
 *
 * Block driver for the on-board microSD slot behind the STM32F746 SDMMC1
 * controller, using HAL_SD in DMA mode.  It is the SDMMC analogue of the QSPI
 * NOR driver (port/qspi/qspi_flash.c): a singleton HAL handle, a per-operation
 * ThreadX mutex, an idempotent init that touches no card, and negative error
 * codes.  Unlike QSPI (indirect/polled), SD transfers run on DMA2, so two extra
 * mechanisms appear here:
 *
 *   - DMA completion is signalled from an ISR via a count-0 TX_SEMAPHORE; the
 *     calling thread waits on it with a finite timeout (no busy-wait).
 *   - The DMA always targets a single 32 B-aligned bounce buffer in SRAM1
 *     (.sram1_dma section); the D-cache is cleaned/invalidated around that
 *     buffer only, and the caller's buffer is touched solely by memcpy.  This
 *     keeps cache coherency correct for any caller alignment without an MPU.
 *
 * Concurrency: every public call serializes on the internal mutex for the whole
 * operation (state wait -> DMA -> completion -> cache).  The API is therefore
 * **thread-context only**: never call it from an ISR, and never before
 * sd_card_init() ran (tx_application_define).
 *
 * Clean-room implementation; the ST BSP (stm32746g_discovery_sd.c) and RM0385
 * were used as a register/pin/DMA-mapping reference only.
 */
#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Logical/physical block size.  SDHC/SDXC are fixed 512 B; HAL also assumes it. */
#define SD_BLOCK_SIZE   512u

/* Error returns (negative); 0 is success. */
#define SD_OK            0
#define SD_ERR_PARAM    -1   /* bad argument (null buffer, zero count)          */
#define SD_ERR_HAL      -2   /* HAL/DMA reported an error or refused the request*/
#define SD_ERR_TIMEOUT  -3   /* card never reached TRANSFER / DMA never completed*/
#define SD_ERR_STATE    -4   /* driver not initialized / card not probed        */
#define SD_ERR_NO_CARD  -5   /* no card in the slot (card-detect = released)    */

/** Snapshot of the probed card's identity/geometry (filled by sd_card_probe). */
struct sd_card_info {
	uint32_t type;            /* CARD_SDSC / CARD_SDHC_SDXC (HAL CardType)     */
	uint32_t version;         /* CARD_V1_X / CARD_V2_X                         */
	uint32_t card_class;      /* card command classes (CCC)                   */
	uint32_t rca;             /* relative card address                        */
	uint32_t block_count;     /* logical 512 B block count (LogBlockNbr)      */
	uint32_t block_size;      /* logical block size, always 512               */
	uint32_t bus_width;       /* negotiated data lines: 1 or 4                */
	uint64_t capacity_bytes;  /* block_count * 512                            */
	uint32_t cid[4];          /* raw CID                                       */
	uint32_t csd[4];          /* raw CSD                                       */
};

/**
 * One-time bring-up: GPIO (AF12), DMA2 streams, NVIC, SDMMC1 clock source mux,
 * operation mutex and DMA-completion semaphore.  Performs **no card I/O** (no
 * HAL_SD_Init), so it is safe to call from tx_application_define() before the
 * scheduler runs.  Idempotent: a second call returns 0 without re-doing setup.
 */
int sd_card_init(void);

/**
 * Power up / identify the inserted card (CMD0..ACMD41..CMD2/3, 4-bit bus, 24 MHz
 * transfer clock) and fill the info snapshot.  Returns SD_ERR_NO_CARD if the
 * slot is empty, SD_ERR_HAL on an identification failure.  Re-probes from
 * RESET each call, so it doubles as a remount entry point (hot-plug, Phase C).
 */
int sd_card_probe(void);

/** Tear the card down to HAL_SD_STATE_RESET (HAL_SD_DeInit).  Idempotent. */
int sd_card_deinit(void);

/** Nonzero when a card is physically present (card-detect PC13 = low). */
int sd_card_is_present(void);

/**
 * Read @p count 512 B blocks starting at LBA @p lba into @p buf (any alignment).
 * Internally chunked through the SRAM1 bounce buffer with DMA + cache
 * invalidation.  @p buf may be unaligned; @p count may exceed the bounce size.
 */
int sd_card_read_blocks(uint32_t lba, void *buf, uint32_t count);

/**
 * Write @p count 512 B blocks from @p buf (any alignment) to LBA @p lba.
 * Destructive; not exposed by the Phase A `sd` command (read-only).  Waits for
 * the card to leave PROGRAMMING before returning so the data is committed.
 */
int sd_card_write_blocks(uint32_t lba, const void *buf, uint32_t count);

/** Probed card identity/geometry; valid only after a successful sd_card_probe(). */
const struct sd_card_info *sd_card_get_info(void);

/** Normalized current state: SD_OK if the card is in TRANSFER, else an SD_ERR_*. */
int sd_card_status(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_CARD_H */
