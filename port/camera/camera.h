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
#define CAM_ERR_BUSY      -7   /* streaming or preview already owns the DCMI    */

/* Default capture geometry (issue #41): QVGA RGB565, little-endian 16-bit
   pixels (R5 in bits 15..11, G6 in 10..5, B5 in 4..0).  Issue #45 makes the
   live geometry variable (struct camera_mode); these remain the power-on
   default and the QVGA reference used by the snapshot pixel-stat helpers. */
#define CAMERA_FRAME_WIDTH   320u
#define CAMERA_FRAME_HEIGHT  240u
#define CAMERA_FRAME_BYTES   (CAMERA_FRAME_WIDTH * CAMERA_FRAME_HEIGHT * 2u)

/* ---- capture mode: resolution / pixel format (issue #45) ----------------- */
/*
 * Port-neutral resolution and pixel-format enums, translated to the lib/ov5640
 * OV5640 resolution / pixel-format constants by res_to_ov() / the format helpers
 * (NOT raw-value compatible -- e.g. dropping 480x272 in #84 shifted the VGA/WVGA
 * ordinals away from OV5640_R*, so never cast between the two).  The shell never
 * sees the lib values.  Ceilings:
 *   - snapshot supports every resolution (DCMI/DMA intra-frame banding);
 *   - streaming supports a mode only while its frame fits one DMA NDTR
 *     (frame_words <= 65535); larger modes are capture-only;
 *   - JPEG is snapshot-only (variable length) and gated to <= VGA.
 */
enum camera_res {
	CAM_RES_QQVGA = 0,  /* 160x120  */
	CAM_RES_QVGA,       /* 320x240  (power-on default) */
	CAM_RES_VGA,        /* 640x480  */
	CAM_RES_WVGA,       /* 800x480  */
	CAM_RES__COUNT
};
enum camera_format {
	CAM_FMT_RGB565 = 0, /* 2 bytes/pixel, little-endian R5G6B5 */
	CAM_FMT_YUV422,     /* 2 bytes/pixel, packed YUYV          */
	CAM_FMT_Y8,         /* 1 byte/pixel, greyscale            */
	CAM_FMT_JPEG,       /* variable length, snapshot-only     */
	CAM_FMT__COUNT
	/* RGB888 is intentionally unsupported in #45 (3 bpp, no consumer). */
};

/* Why the selected fps (camera fps 15|30) is or is not in effect right now (#67).
   30 fps = 48 MHz PCLK is applied only for a small streamable mode while the LTDC
   is not scanning out; otherwise the sensor is clamped to 24 MHz (15 fps) so the
   48 MHz DCMI burst never overruns the 16-bit SDRAM that the LTDC also reads. */
enum camera_fps_clamp {
	CAM_FPS_OK = 0,        /* selected fps is in effect (fps_eff == fps_sel)  */
	CAM_FPS_CLAMP_SIZE,    /* clamped to 15: res is VGA/WVGA (snapshot-only)  */
	CAM_FPS_CLAMP_LTDC,    /* clamped to 15: LTDC scanout active (lcd on) */
};

/**
 * Live capture mode -- the single source of truth for geometry/format/timing
 * (owned by the driver; read via camera_get_mode()).  @ref frame_bytes is the
 * raster size for fixed formats (w*h*bpp) or the JPEG capture-budget capacity;
 * the JPEG *valid* length comes back through camera_frame_read()'s sizing.
 */
struct camera_mode {
	uint8_t  res;             /* enum camera_res                            */
	uint8_t  format;          /* enum camera_format                         */
	uint16_t width;
	uint16_t height;
	uint8_t  bytes_per_pixel; /* 2 RGB565/YUV422, 1 Y8, 0 = variable (JPEG) */
	uint8_t  is_jpeg;
	uint32_t frame_bytes;     /* raster bytes, or JPEG budget capacity      */
	uint32_t frame_words;     /* DMA NDTR for fixed formats (frame_bytes/4) */
	uint16_t hts;             /* OV5640 line total (fps table)              */
	uint16_t vts;             /* OV5640 frame total (fps table)             */
	uint32_t pclk_hz;         /* sensor PCLK -- live effective (#67)        */
	uint16_t fps_target_x10;  /* effective pclk_hz/(hts*vts) * 10, not live  */
	uint8_t  streamable;      /* frame_words <= 65535 && !is_jpeg           */
	uint8_t  fps_sel;         /* selected fps knob bucket: 15 or 30 (#67)   */
	uint8_t  fps_eff;         /* effective knob bucket now: 15 or 30 -- the
	                             PCLK selection (24/48 MHz), not the exact
	                             target rate (see fps_target_x10) (#67)     */
	uint8_t  fps_clamp;       /* enum camera_fps_clamp: why eff != sel      */
};

/** Driver state snapshot for the `camera info` command. */
struct camera_info {
	uint32_t chip_id;     /* 0x5640 after a successful probe, else 0       */
	int      powered;     /* PWR_EN asserted and probe succeeded           */
	int      configured;  /* sensor programmed for a capture mode          */
	int      frame_valid; /* a captured frame is in the buffer             */
	uint32_t frame_bytes; /* valid captured length (#45): raster size, or
	                         the trimmed JPEG stream length; 0 when none    */
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

/** Current OV5640 quality settings (defaults are neutral except flip, which
 *  defaults to CAM_FLIP_FLIP for this board's camera mounting -- upright, #68). */
struct camera_settings {
	int8_t  brightness;  /* CAM_LEVEL_MIN..MAX, 0 = neutral               */
	int8_t  contrast;    /* CAM_LEVEL_MIN..MAX                            */
	int8_t  saturation;  /* CAM_LEVEL_MIN..MAX                            */
	int8_t  hue;         /* CAM_HUE_MIN..MAX, units of 30 deg             */
	uint8_t awb;         /* enum camera_awb                              */
	uint8_t effect;      /* enum camera_effect                          */
	uint8_t flip;        /* enum camera_flip (default CAM_FLIP_FLIP, #68)  */
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

/** Fill @p out with the live capture mode (geometry/format/timing, #45).
 *  Never touches the sensor.  Evaluates the fps clamp (fps_sel/fps_eff/fps_clamp,
 *  pclk_hz, fps_target_x10) live against the current LTDC scanout state (#67), so
 *  the reported rate reflects whether 30 fps is in effect right now. */
int camera_get_mode(struct camera_mode *out);

/**
 * Switch the capture resolution and/or pixel format (issue #45).  Re-programs
 * the OV5640 (resolution/format scalers, the per-mode HTS/VTS/PCLK fps table)
 * and resizes the live capture geometry.  Refused with CAM_ERR_BUSY while a
 * stream or GUIX preview is active (the ring slots are a live DMA target sized
 * for the current mode).  Probes/configures the sensor on demand.  On any I/O
 * failure the mode is left uncommitted and the sensor is marked unconfigured so
 * the next capture re-programs it from scratch.  Returns 0 or a negative
 * CAM_ERR_*.
 */
int camera_set_format(enum camera_res res, enum camera_format fmt);

/**
 * Select the streaming frame rate (issue #67): @p fps is 15 (24 MHz PCLK) or 30
 * (48 MHz PCLK).  Stored as a preference and re-applied to the live sensor at
 * once (same path as camera_set_format), so it is refused with CAM_ERR_BUSY while
 * a stream or GUIX preview owns the DCMI.  30 fps takes effect only for a small
 * streamable mode (QQVGA/QVGA) while the LTDC is not scanning out;
 * otherwise the sensor is clamped to 24 MHz so the 48 MHz DCMI burst never
 * overruns the SDRAM the LTDC also reads (use `lcd off` for 30 fps).  Returns
 * 0 or a negative CAM_ERR_* (CAM_ERR_PARAM for an fps other than 15/30).
 */
int camera_set_fps(unsigned fps);

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
	uint32_t jpeg_trunc; /* JPEG stream frames dropped: no SOI/EOI (#63)     */
	uint32_t slots;      /* ring depth carved from the arena this stream (#65) */
	uint32_t slot_bytes; /* ring slot stride this stream (align32(frame))    */
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

/* ---- base capture + subscribers (Epic #99 Phase 1, #100) ----------------- */
struct frame_sink;       /* svc/frame_pipeline.h */
struct frame_desc;       /* svc/frame.h          */

/**
 * Register push sink @p s as a *subscriber* of the base capture (`camera
 * stream`).  @p cls_fmt selects the sink's format class: CAM_FMT_JPEG for a JPEG
 * (compressed) sink, any raster format (e.g. CAM_FMT_RGB565) for a raster sink.
 * The subscriber's `enabled` intent is orthogonal to the base on/off: if the
 * base is already running AND its format class matches, @p s is attached
 * immediately (its open() negotiates the live geometry); otherwise it stays
 * enabled + idle and is attached at the next base start.  Idempotent re-enable.
 * Never starts the base (auto-start is intentionally absent).  Returns 0, or a
 * negative CAM_ERR_* (CAM_ERR_STATE registry full, CAM_ERR_BUSY the immediate
 * attach was rejected).  camera.c owns the sink's pipeline membership; the
 * subscriber module must NOT track `attached` itself.
 */
int camera_subscribe(struct frame_sink *s, enum camera_format cls_fmt);

/**
 * Unregister subscriber @p s: detach it from the base if attached (its close()
 * fires) and clear its enabled intent.  Does NOT stop the base -- other
 * subscribers and the producer keep running (a cascade stop is `camera stream
 * stop`).  Returns the pipeline's in-flight pin count for @p s at detach time (0
 * when not attached): a nonzero value means a consume() was pre-pinned and may
 * not have started yet, so the caller must drain @p s AFTER this returns.  Safe
 * when @p s is not registered (no-op, returns 0).
 */
int camera_unsubscribe(struct frame_sink *s);

/**
 * Start the base capture in JPEG at resolution @p res with @p s attached, for the
 * MJPEG-over-HTTP server (#49 P5).  A base-owning convenience entrypoint (Phase 1):
 * it registers @p s as a JPEG-class subscriber, sets JPEG (snapshot-loop,
 * res <= CAM_RES_VGA) and starts the base.  Refused (CAM_ERR_BUSY) if the base is
 * already streaming (the raster and JPEG format classes are exclusive -- one DCMI,
 * one format).  The sink then receives compressed frames from
 * frame_pipeline_publish(FRAME_FMT_JPEG).  Stop with camera_mjpeg_stop().  Returns
 * 0 or a negative CAM_ERR_*.
 */
int camera_mjpeg_start(struct frame_sink *s, enum camera_res res);

/**
 * Stop the MJPEG base owned by @p s: detach @p s and cascade-stop the base (the
 * producer tears the DCMI down).  If an async teardown (DCMI overrun) already
 * stopped the base, this is a bare unregister.  Returns the pipeline's in-flight
 * pin count for @p s at detach time (0 when not attached): a nonzero value means a
 * consume() was pre-pinned, so the caller must drain @p s AFTER this returns.
 */
int camera_mjpeg_stop(struct frame_sink *s);

/**
 * Release one pre-pinned slot on behalf of subscriber sink @p s -- the sink's
 * consume() calls this exactly once per delivered frame.  Wraps the pipeline
 * put() so a subscriber's glue never holds the camera's internal pipeline handle,
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
