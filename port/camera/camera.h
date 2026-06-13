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

/* ---- quality settings (issue #44) ---------------------------------------- */
/*
 * OV5640 ISP image-quality controls, exposed port-neutral so the shell never
 * sees the lib/ov5640 constants.  Settings live in a RAM cache: a `camera set`
 * applies immediately when the sensor is powered+configured, otherwise it is
 * stored and re-applied by the next capture's lazy configure -- OV5640_Init
 * rewrites the whole SDE register block, so the cache must outlive it.
 */
enum camera_awb {     /* white balance / light mode */
	CAM_AWB_AUTO = 0, CAM_AWB_SUNNY, CAM_AWB_OFFICE, CAM_AWB_HOME,
	CAM_AWB_CLOUDY
};
enum camera_effect {  /* special color effect */
	CAM_FX_NONE = 0, CAM_FX_BW, CAM_FX_SEPIA, CAM_FX_NEGATIVE,
	CAM_FX_BLUE, CAM_FX_RED, CAM_FX_GREEN
};
enum camera_flip {    /* mirror / flip orientation */
	CAM_FLIP_NONE = 0, CAM_FLIP_MIRROR, CAM_FLIP_FLIP, CAM_FLIP_BOTH
};

/* Inclusive ranges for the bipolar level controls. */
#define CAM_LEVEL_MIN  (-4)   /* brightness / contrast / saturation        */
#define CAM_LEVEL_MAX  (4)
#define CAM_HUE_MIN    (-6)   /* hue index; degrees = index * 30 (-180 deg) */
#define CAM_HUE_MAX    (5)    /* +150 deg                                    */

/** Current OV5640 quality settings (defaults are all neutral). */
struct camera_settings {
	int8_t  brightness;  /* CAM_LEVEL_MIN..MAX, 0 = neutral               */
	int8_t  contrast;    /* CAM_LEVEL_MIN..MAX                            */
	int8_t  saturation;  /* CAM_LEVEL_MIN..MAX                            */
	int8_t  hue;         /* CAM_HUE_MIN..MAX, units of 30 deg             */
	uint8_t awb;         /* enum camera_awb                              */
	uint8_t effect;      /* enum camera_effect                          */
	uint8_t flip;        /* enum camera_flip                            */
	uint8_t zoom;        /* digital zoom factor: 1, 2, 4 or 8           */
	uint8_t night;       /* 0 = off, 1 = night mode on                  */
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

/** Nonzero while a stream is running (a destructive `.sdram` op must refuse:
 *  the ring is a live DMA target).  See camera_stream_start() (#46). */
int camera_streaming(void);

/* ---- streaming (issue #46): DCMI continuous + DMA double-buffer ----------- */
/** Live FPS / overrun snapshot for `camera stream stats`. */
struct camera_stream_info {
	int      active;     /* a stream is in progress                          */
	int      err;        /* stopped by a DCMI overrun (terminal)             */
	uint32_t frames;     /* frames published since start                     */
	uint32_t delivered;  /* frames delivered to the stats sink               */
	uint32_t dropped;    /* frames the sink dropped (busy)                   */
	uint32_t dcmi_ovr;   /* DCMI FIFO overruns                               */
	uint32_t ring_ovr;   /* ring exhaustion / lost completions               */
	uint32_t dma_fe;     /* DMA FIFO/DME errors tolerated (non-fatal, #56)   */
	uint32_t elapsed_ms; /* run duration (live, or frozen at stop)           */
};

/**
 * Start continuous capture into the SDRAM frame ring (DCMI continuous + DMA
 * double-buffer).  **Non-blocking**: a dedicated producer thread runs the
 * capture and this returns at once.  @p colorbar selects the OV5640 test
 * pattern.  The stream auto-stops after @p frames frames or @p secs seconds
 * (0 = unbounded); camera_stream_stop() or a DCMI overrun also stop it.
 * Mutually exclusive with camera_capture() (both own the DCMI/DMA).
 */
int camera_stream_start(int colorbar, uint32_t frames, uint32_t secs);

/** Request the running stream to stop (non-blocking; the producer tears down). */
int camera_stream_stop(void);

/** Fill @p out with the current or last stream statistics (any time). */
int camera_stream_stats(struct camera_stream_info *out);

/* ---- GUIX live preview ownership (issue #56) ----------------------------- */
struct frame_sink;       /* svc/frame_pipeline.h */
struct frame_desc;       /* svc/frame.h          */

/**
 * Start streaming for a GUIX live preview and attach @p s as an external push
 * sink (in addition to the internal stats sink).  Equivalent to
 * camera_stream_start(colorbar=0, unbounded) but also takes **preview
 * ownership**: while it holds, the public `camera stream start/stop` are
 * refused (CAM_ERR_STATE).  Returns 0 on success or a negative CAM_ERR_* (e.g.
 * a plain stream / preview already running, no sensor, SDRAM down).  The
 * producer's async teardown (DCMI overrun) also releases ownership and detaches
 * @p s, so the slot/pin contract survives a later frame_pipeline_init().
 */
int camera_preview_start(struct frame_sink *s);

/**
 * Release preview ownership taken by camera_preview_start(@p s): detach @p s and
 * stop the stream -- but ONLY while @p s is still the owner.  If an async
 * teardown already released ownership, this is a no-op (it must not stop a
 * different stream started since).  Returns the pipeline's in-flight pin count
 * for @p s at detach time (0 when not the owner): a nonzero value means a
 * consume() was pre-pinned and may not have started yet, so the caller must
 * drain @p s (wait for that consume() to finish) AFTER this returns.
 */
int camera_preview_stop(struct frame_sink *s);

/**
 * Release one pre-pinned slot on behalf of preview sink @p s -- the sink's
 * consume() calls this exactly once per delivered frame.  Wraps the pipeline
 * put() so the GUIX glue never holds the camera's internal pipeline handle,
 * eliminating any race where the producer consumes against @p s before a
 * returned handle could be stored.
 */
void camera_frame_put(struct frame_sink *s, const struct frame_desc *f);

/** Copy the current quality settings into @p out (never touches the sensor). */
int camera_get_settings(struct camera_settings *out);

/*
 * Quality setters.  Each validates its argument (CAM_ERR_PARAM out of range),
 * updates the RAM cache, and -- when the sensor is already configured --
 * re-applies the full settings block so the controls coexist (the OV5640
 * setters each clobber the shared SDE_CTRL0 enable register; the driver fixes
 * that up).  When the sensor is not yet configured the value is only cached.
 */
int camera_set_brightness(int level);          /* CAM_LEVEL_MIN..MAX        */
int camera_set_contrast(int level);            /* CAM_LEVEL_MIN..MAX        */
int camera_set_saturation(int level);          /* CAM_LEVEL_MIN..MAX        */
int camera_set_hue(int degrees);               /* -180..150, multiple of 30 */
int camera_set_awb(enum camera_awb mode);
int camera_set_effect(enum camera_effect effect);
int camera_set_flip(enum camera_flip flip);
int camera_set_zoom(int factor);               /* 1, 2, 4 or 8              */
int camera_set_night(int on);                  /* 0 = off, nonzero = on     */

/** Reset every quality setting to its neutral default and re-apply. */
int camera_set_defaults(void);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_H */
