/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    camera.h
 * @brief   B-CAMS-OMV (OV5640) camera driver over DCMI/I2C1 (issues #39/#41,
 *          Epic #22).
 *
 * Sensor-control layer for the B-CAMS-OMV camera bundle (MB1683 adapter +
 * MB1379 OV5640 module) on the board's P1 30-pin ZIF connector: power
 * control and sensor identification over I2C1/SCCB (#39), plus single-frame
 * QVGA RGB565 snapshot capture over DCMI + DMA2 into the SDRAM frame buffer
 * (#41).
 *
 * Hardware facts (UM1907 / UM2779, verified in #22 Phase 0):
 *   - P1 <-> B-CAMS-OMV CN5 wire 1:1 over the FFC (reversed pin numbering).
 *   - The MB1379 module clocks the OV5640 from its own 24 MHz crystal (UM2779
 *     §3.2) -- the host supplies no XCLK/MCO.
 *   - Sensor I2C: I2C1, SCL=PB8 / SDA=PB9 (AF4), write address 0x78, 16-bit
 *     register addresses, chip ID 0x300A/0x300B = 0x5640.
 *   - Power: DCMI_PWR_EN = PH13, LOW = camera powered (ST BSP semantics).
 *   - Reset: P1's DCMI_NRST is tied to the board NRST net (no GPIO control);
 *     a PH13 power cycle plus the OV5640 software reset stands in for it.
 *
 * Concurrency: public calls serialize on an internal TX_MUTEX; the API is
 * thread-context only (never from an ISR, never before camera_init() ran in
 * tx_application_define).  The actual work lives in *_locked() helpers so a
 * public entry never re-acquires the mutex it already holds.
 *
 * The OV5640 register sequences come from the ST component driver submodule
 * (lib/ov5640, BSD-3-Clause); this glue layer is clean-room MIT.
 */
#ifndef CAMERA_H
#define CAMERA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error returns (negative); 0 is success. */
#define CAM_OK             0
#define CAM_ERR_PARAM     -1   /* bad argument                                  */
#define CAM_ERR_HAL       -2   /* HAL / sensor I/O reported an error            */
#define CAM_ERR_TIMEOUT   -3   /* capture never completed                       */
#define CAM_ERR_STATE     -4   /* driver not initialized / SDRAM down           */
#define CAM_ERR_NO_SENSOR -5   /* OV5640 not detected (no module / bad ID)      */
#define CAM_ERR_NO_FRAME  -6   /* no captured frame available                   */

/* Fixed capture geometry (issue #41): QVGA RGB565, little-endian 16-bit
   pixels (R5 in bits 15..11, G6 in 10..5, B5 in 4..0). */
#define CAMERA_FRAME_WIDTH   320u
#define CAMERA_FRAME_HEIGHT  240u
#define CAMERA_FRAME_BYTES   (CAMERA_FRAME_WIDTH * CAMERA_FRAME_HEIGHT * 2u)

/** Driver state snapshot for the `camera info` command. */
struct camera_info {
	uint32_t chip_id;     /* 0x5640 after a successful probe, else 0       */
	int      powered;     /* PWR_EN asserted and probe succeeded           */
	int      configured;  /* sensor programmed for QVGA RGB565 capture     */
	int      frame_valid; /* a captured frame is in the buffer             */
};

/**
 * One-time bring-up: PWR_EN GPIO (PH13, parked OFF), I2C1 (PB8/PB9 AF4,
 * 100 kHz) and the operation mutex.  Performs **no sensor I/O**, so it is safe
 * to call from tx_application_define() before the scheduler runs.  Idempotent:
 * a second call returns 0 without re-doing setup.
 */
int camera_init(void);

/**
 * Power-cycle the module (PH13 high 100 ms -> low, ST BSP timing) and read the
 * OV5640 chip ID over I2C.  On success *chip_id (may be NULL) receives 0x5640
 * and the sensor is left powered but unconfigured.  On failure the module is
 * powered back off and CAM_ERR_NO_SENSOR / CAM_ERR_HAL is returned.
 */
int camera_probe(uint32_t *chip_id);

/** Cut module power (PH13 high) and clear the probed/configured state. */
int camera_power_off(void);

/** Fill @p out with the current driver state.  Never touches the sensor. */
int camera_get_info(struct camera_info *out);

/**
 * Capture one QVGA RGB565 frame into the SDRAM frame buffer (DCMI snapshot +
 * DMA2).  Probes and configures the sensor on demand (lazy); with @p colorbar
 * nonzero the OV5640 emits its 8-bar test pattern instead of the live image.
 * Blocks until the frame completes (<=1 s timeout).  On success the frame is
 * readable via camera_frame_read() until the next capture/power-off.
 * Returns CAM_ERR_STATE when SDRAM is down, CAM_ERR_TIMEOUT when no frame
 * arrived (wiring/sync), CAM_ERR_HAL on a DCMI/DMA error (e.g. overrun).
 */
int camera_capture(int colorbar);

/**
 * Copy @p len bytes at byte offset @p offset out of the captured frame into
 * @p dst (any alignment).  Serialized against capture/save by the driver
 * mutex.  Fails with CAM_ERR_NO_FRAME until a capture succeeded.
 *
 * @p gen (optional, may be NULL) receives the frame's generation counter,
 * bumped by every successful capture.  A multi-call reader (stats, save)
 * must compare generations across its reads: a concurrent capture between
 * two reads re-validates the buffer with NEW pixels -- frame_valid alone
 * cannot detect that, only the generation change does.
 */
int camera_frame_read(uint32_t offset, void *dst, uint32_t len,
                      uint32_t *gen);

/**
 * Drop the captured-frame flag (the buffer contents are about to be clobbered
 * -- called by `sdram test` before it overwrites the .sdram region).  Safe to
 * call in any state; a no-op when the driver is not initialized.
 */
void camera_frame_invalidate(void);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_H */
