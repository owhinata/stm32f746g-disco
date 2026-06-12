/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    sdram.h
 * @brief   On-board FMC SDRAM bring-up (issue #40, Epic #22).
 *
 * Driver for the board's 128-Mbit SDRAM (Micron MT48LC4M32B2, 4M x 32) behind
 * the STM32F746 FMC SDRAM controller.  Only the low 16 data lines are wired
 * (UM1907 §6.13), so the accessible array is 8 MB at 0xC0000000 (FMC bank 1).
 *
 * The region's primary purpose is large DMA-target buffers (the camera frame
 * buffer, #41).  To make those coherent by construction, bsp_init() maps the
 * whole 8 MB through the MPU as Normal **non-cacheable** memory (see
 * mpu_config() in src/bsp.c) -- DMA writes and CPU reads need no cache
 * maintenance at the cost of slower CPU access.  Linker objects land here via
 * the `.sdram` (NOLOAD) section; nothing in it survives reset and nothing may
 * touch it before sdram_init() ran.
 *
 * sdram_init() performs the FMC/GPIO/controller setup plus the JEDEC power-up
 * command sequence.  It busy-waits (bsp_udelay / register polls) and uses no
 * interrupts or ThreadX objects, so it may run from tx_application_define()
 * before the scheduler starts.  Idempotent; later calls return the first
 * result.
 *
 * Clean-room implementation; the ST BSP (stm32746g_discovery_sdram.c) and
 * RM0385 §13 were used as a register/pin/timing reference only.
 */
#ifndef SDRAM_H
#define SDRAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error returns (negative); 0 is success. */
#define SDRAM_OK          0
#define SDRAM_ERR_HAL    -1   /* FMC init or a power-up command failed */
#define SDRAM_ERR_STATE  -2   /* sdram_init() did not succeed          */

#define SDRAM_BASE_ADDR  0xC0000000u
#define SDRAM_SIZE_BYTES (8u * 1024u * 1024u)   /* 16-bit bus: 8 MB usable */

/**
 * One-time bring-up: FMC GPIO (AF12), FMC SDRAM controller (16-bit, CAS3,
 * SDCLK = HCLK/2 = 108 MHz; CL2 is only rated to 100 MHz on this part) and
 * the JEDEC init sequence (clock enable -> 100 us -> PALL -> 8x auto-refresh
 * -> mode register -> refresh rate).  Polling only -- safe from
 * tx_application_define().  Idempotent.
 */
int sdram_init(void);

/** Nonzero once sdram_init() succeeded (the 8 MB at 0xC0000000 is usable). */
int sdram_is_up(void);

#ifdef __cplusplus
}
#endif

#endif /* SDRAM_H */
