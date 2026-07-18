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
#include "ltdc_display.h"   /* ltdc_scanout_active() -- 30 fps clamp guard (#67) */

#include "stm32f7xx_hal.h"
#include "tx_api.h"

#include <string.h>

#include "ov5640.h"

#include "frame.h"
#include "frame_pipeline.h"

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

/*
 * SDE_CTRL0 (0x5580) -- the Special-Digital-Effects master enable register
 * (OV5640 datasheet table 7-26).  Each lib/ov5640 quality setter *overwrites*
 * this register with only its own enable bit, so applying brightness then
 * saturation leaves just the saturation bit set and brightness silently dies.
 * The fixup in settings_apply_all_locked() OR-s the right bits back together.
 *   b7 Fixed Y  b6 Negative  b5 Gray  b4 Fixed V  b3 Fixed U
 *   b2 Contrast (shared by brightness: it rides the contrast Y-bright path)
 *   b1 Saturation  b0 Hue
 */
#define OV5640_REG_SDE_CTRL0  0x5580u
#define SDE0_HUE_EN       0x01u
#define SDE0_SAT_EN       0x02u
#define SDE0_CONTRAST_EN  0x04u   /* brightness + contrast (both Y path)    */
#define SDE0_FIXED_U_EN   0x08u
#define SDE0_FIXED_V_EN   0x10u
#define SDE0_NEG_EN       0x40u
#define SDE0_TINT         (SDE0_FIXED_U_EN | SDE0_FIXED_V_EN)  /* 0x18      */
/* Bits this driver owns; bits we never touch (b5 Gray, b7 Fixed Y) are kept. */
#define SDE0_MANAGED      (SDE0_NEG_EN | SDE0_TINT | SDE0_CONTRAST_EN | \
                           SDE0_SAT_EN | SDE0_HUE_EN)          /* 0x5F      */

/*
 * SDE_CTRL8 (0x5588) carries the sign / UV-adjust-manual-enable bits the
 * brightness/contrast/saturation/hue setters poke via the vendored
 * ov5640_modify_reg -- which is OR-only (ov5640.c: TempData1 |= data & mask)
 * and so can SET but never CLEAR a masked bit.  Worse, the setters' masks
 * overlap on bit0: contrast/saturation force it (mask 0x41) while hue owns it
 * (mask 0x33), so OR-only leaves bit0 stuck for hue degrees whose table entry
 * lacks it (e.g. -180 deg wants 0x32 but ends 0x33).  Rather than fight the
 * OR-only helper, settings_apply_all_locked() rewrites the managed bits of
 * SDE_CTRL8 with an EXACT value computed from the cache after the setters run.
 *   b6 UV-adjust-manual-enable (the contrast/saturation path needs it)
 *   b3 Y-bright sign for contrast (set when brightness < 0)
 *   b5 b4 b1 b0 hue sin/cos sign bits (from the OV5640 hue table)
 */
#define OV5640_REG_SDE_CTRL8  0x5588u
#define SDE8_UV_MANUAL    0x40u
#define SDE8_YBRIGHT_SIGN 0x08u
#define SDE8_HUE_SIGNS    0x33u
#define SDE8_MANAGED      (SDE8_UV_MANUAL | SDE8_YBRIGHT_SIGN | SDE8_HUE_SIGNS)
                                                              /* 0x7B */

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
static uint32_t cam_frame_gen;         /* #102: bumped whenever the stable cam_frame
                                          buffer is refreshed -- by `camera capture`
                                          or by camera_snapshot_latest() (base ON).
                                          save/send compare it across chunks to detect
                                          a frame replaced mid-read. */
static struct camera_info info;

/* Quality settings (issue #44): RAM cache, neutral by default EXCEPT flip, which
   defaults to CAM_FLIP_FLIP so the live preview / capture comes out upright for
   this board's camera-module mounting orientation (issue #68).  Survives power
   cycles so a `camera set` made while powered off still takes effect at the next
   capture; cleared back to these defaults only by camera_set_defaults. */
static struct camera_settings settings = {
	.brightness = 0, .contrast = 0, .saturation = 0, .hue = 0,
	.awb        = CAM_AWB_AUTO, .effect = CAM_FX_NONE,
	.flip       = CAM_FLIP_FLIP, .zoom = 1, .night = 0,
};
/* Set whenever the SDE/flip/zoom/night cache changes; cleared only after a
   successful settings_apply_all_locked().  Lets a live capture re-apply when a
   previous immediate apply failed (I2C glitch), so the cache and the sensor never
   silently diverge. */
static int settings_dirty;

/* Companion to settings_dirty, but for the sensor *timing* (AEC max-exposure
   ceiling restore + VTS reclamp).  SDE/geometry changes do NOT touch timing, so
   their no-timing apply must not clear a pending timing retry -- a separate flag
   (#70) keeps "SDE applied OK but the VTS/ceiling step failed" recoverable.  Set
   only inside settings_and_timing_apply_locked() on any failure of that full
   timing apply and cleared there on full success; apply_if_live_locked() falls
   back to a full timing apply while it is set, and camera_configure_locked()
   retries on it. */
static int timing_dirty;

/* ---- capture mode: resolution / pixel format / fps (issue #45) ----------- */
/*
 * The single source of truth for the live capture geometry/format/timing.  The
 * snapshot/stream paths read it instead of the old fixed CAMERA_FRAME_* macros;
 * camera_set_format() re-programs the sensor and updates it.  Defaults to the
 * power-on QVGA RGB565 mode so the driver behaves exactly as before #45 until a
 * mode switch.  Timing (hts/vts/pclk) is the OV5640 common-table reality at
 * power-on; the fps table (mode_fps[]) overrides it on a set_format. */
static struct camera_mode mode = {
	.res = CAM_RES_QVGA, .format = CAM_FMT_RGB565,
	.hts = 1600u, .vts = 1000u, .pclk_hz = 24000000u,   /* QVGA fps-table base */
};

/* fps knob (#67): the selected frame rate (15 or 30), a user preference mapped to
   a 24/48 MHz OV5640 PCLK.  48 MHz (30 fps) takes effect only for a small mode
   while the LTDC is not scanning out (effective_ov_pclk()); otherwise the sensor
   is clamped to 24 MHz so the 48 MHz DCMI burst never overruns the 16-bit SDRAM
   the LTDC also reads.  Default 15 (safe with the display on). */
static uint8_t cam_fps_sel = 15u;

/* The PCLK actually programmed into the OV5640 (Hz), or 0 when unknown/desynced.
   Distinct from mode.pclk_hz (the displayed/effective value): this is the truth
   about the sensor.  Reset to 0 on every re-init (mode_reset_default / each
   OV5640_Init) so apply_effective_pclk_locked() re-writes SetPCLK after a reset
   even when mode.pclk_hz happens to match what we would select (#67). */
static uint32_t cam_sensor_pclk_hz;

/* Pure geometry per resolution (independent of format/timing). */
static const uint16_t res_wh[CAM_RES__COUNT][2] = {
	{ 160u, 120u },   /* QQVGA   */
	{ 320u, 240u },   /* QVGA    */
	{ 640u, 480u },   /* VGA     */
	{ 800u, 480u },   /* WVGA    */
};

/* JPEG single-shot capture budget: <= 65535 words so HAL_DCMI_Start_DMA runs a
   plain (non-banded) DMA and the valid length is simply budget - NDTR (#45). */
#define CAM_JPEG_BUDGET_WORDS  65535u
#define CAM_JPEG_BUDGET_BYTES  (CAM_JPEG_BUDGET_WORDS * 4u)

static uint32_t fmt_bytes_per_pixel(uint8_t fmt)
{
	switch (fmt) {
	case CAM_FMT_RGB565:
	case CAM_FMT_YUV422: return 2u;
	case CAM_FMT_Y8:     return 1u;
	default:             return 0u;   /* JPEG: variable */
	}
}

/* Recompute the geometry-derived fields after a res/format change.  Timing
   (hts/vts/pclk_hz) is set separately from the fps table; fps_target_x10 is the
   theoretical target pclk/(hts*vts), not a measured rate. */
static void mode_recompute(struct camera_mode *m)
{
	m->width  = res_wh[m->res][0];
	m->height = res_wh[m->res][1];
	m->is_jpeg = (m->format == CAM_FMT_JPEG) ? 1u : 0u;
	m->bytes_per_pixel = (uint8_t)fmt_bytes_per_pixel(m->format);
	if (m->is_jpeg) {
		m->frame_bytes = CAM_JPEG_BUDGET_BYTES;
		m->frame_words = CAM_JPEG_BUDGET_WORDS;
		m->streamable  = 0u;        /* JPEG is snapshot-only */
	} else {
		/* All supported (even-width, even-height) raster modes give a frame_bytes
		   that is a multiple of 4, so frame_words = frame_bytes/4 is exact and the
		   HAL banding splits evenly -- keep that invariant if a mode is added. */
		m->frame_bytes = (uint32_t)m->width * m->height * m->bytes_per_pixel;
		m->frame_words = m->frame_bytes / 4u;
		m->streamable  = (m->frame_words <= 65535u) ? 1u : 0u;
	}
	if (m->hts != 0u && m->vts != 0u)
		m->fps_target_x10 = (uint16_t)((uint64_t)m->pclk_hz * 10u /
		                               ((uint32_t)m->hts * m->vts));
	else
		m->fps_target_x10 = 0u;
}

/* Largest fixed-format snapshot frame = WVGA RGB565 (800x480x2).  cam_frame is
   sized for it (one contiguous region, required by the HAL intra-frame banding)
   and also serves as the JPEG capture budget buffer (<= CAM_JPEG_BUDGET_BYTES).
   The active mode uses a prefix; the rest is unused slack (#45). */
#define CAM_FRAME_MAX_BYTES  (800u * 480u * 2u)   /* 768000, 32-aligned */

/* Snapshot frame buffer in external SDRAM (.sdram: NOLOAD, MPU non-cacheable,
   #40).  DMA-written by DCMI, CPU-read by camera_frame_read -- coherent with no
   cache maintenance because the region never allocates cache lines. */
static uint8_t cam_frame[CAM_FRAME_MAX_BYTES]
	__attribute__((aligned(32), section(".sdram.fixed")));

/* ---- streaming (issue #46): DCMI continuous + DMA double-buffer ---------- */
/*
 * The continuous producer feeds a frame pipeline (svc/frame_pipeline.c, the #47
 * design).  The ring slots are carved at stream start from cam_arena (below): at
 * any instant the DMA double-buffer (DBM) targets two of them (M0AR/M1AR), one
 * holds the latest published frame, and one is free to be acquired (so >= 4 slots
 * is the comfortable case; >= 2 is the hard minimum for the DBM pair).  On each
 * frame the dedicated cam_producer thread (NOT an ISR, NOT the shell thread)
 * reads CT to find the just-completed buffer, acquires a free slot, repoints that
 * buffer's M-register away (HAL_DMAEx_ChangeMemory) and only THEN publishes -- so
 * a slot handed to a sink is never a live DMA target (tear-free).  The DMA TC ISR
 * only posts cam_stream_sem.  Streaming and the snapshot path share hdcmi/
 * hdma_dcmi and are mutually exclusive (cam_stream_active gate).
 */
#define CAM_PRODUCER_PRIO  10u     /* below IWDG petter (5), like LED (10)      */
/* Sized from the measured high-water-mark (`thread` peak = 508 B, #93); 1024
 * keeps ~2x margin for this well-exercised DCMI sem-wait producer.             */
#define CAM_PRODUCER_STACK 1024u
#define CAM_PRODUCER_TICK  10u     /* bounded sem wait so --secs/stop fire even
                                      when no frames arrive (sync lost), ms      */

/* Camera DMA ring arena (issue #65): one fixed 2 MB buffer pinned by the linker
   to FMC SDRAM internal bank1 (0xC0200000, .sdram.cam) so the DCMI ring WRITE
   target stays in a different internal bank from the LTDC scan-out READ surface
   (ltdc_fb, bank0) -- both banks keep a row open, cutting precharge/activate
   thrashing (FE).  Replaces the old fixed cam_ring[4][256 KB]: at stream start
   the slot stride is sized to the current mode (align32(frame_bytes)) and the
   arena is partitioned into as many slots as fit, capped at
   FRAME_PIPELINE_MAX_SLOTS -- so small modes get a deeper ring.  2 MB gives the
   full FRAME_PIPELINE_MAX_SLOTS ring even for the largest streamable slot
   (QVGA RGB565 = 153600, rounded up) and keeps cam_arena filling bank1 for the
   #65 FE bank-isolation win. */
#define CAM_ARENA_BYTES    (2u * 1024u * 1024u)

static uint8_t cam_arena[CAM_ARENA_BYTES]
	__attribute__((aligned(32), section(".sdram.cam")));

static struct frame_pipeline cam_pipe;
static TX_MUTEX     cam_pipe_lock;        /* frame_os mutex for the pipeline    */
static TX_SEMAPHORE cam_stream_sem;       /* DMA TC ISR -> producer (completions) */
static TX_SEMAPHORE cam_start_sem;        /* start -> producer idle wakeup       */
static TX_THREAD    cam_producer;
static UCHAR        cam_producer_stack[CAM_PRODUCER_STACK];
static struct frame_sink cam_stat_sink;   /* counting sink (FPS / OVR source)   */

/* ---- subscriber registry (Epic #99 Phase 1, #100) ------------------------
   External push sinks (GUIX preview #56 / nncam inference #81 / MJPEG #49 P5)
   register here as *subscribers*.  camera.c owns their pipeline membership
   under cam_lock: a subscriber is linked into cam_pipe only while the base
   capture (`camera stream`) is active AND its format class matches the base
   format.  The internal cam_stat_sink is attached directly (always
   compatible), outside this table.  `attached == 1` iff the sink is currently
   in cam_pipe.sinks (registry invariant); a detach clears it in the same
   cam_lock section.  A subscriber's own module keeps only its `enabled` intent
   and feature resources -- never `attached` (so a stale attach cannot diverge
   from the pipeline).  This replaces the old single `cam_ext_sink` owner
   pointer: the base is now a cascade of stat + N subscribers, not one owner.

   `fmt` is the exact pixel format the subscriber consumes (enum camera_format):
   a subscriber attaches only while the base publishes that format (RGB565 for
   gui/nncam, JPEG for mjpeg).  The DCMI is one format at a time, so an
   incompatible subscriber stays enabled + idle rather than failing the base. */
#define CAM_MAX_SUBS 3   /* gui + nncam + mjpeg registrations; at most
                            FRAME_PIPELINE_MAX_SINKS-1 attach at once (stat
                            takes one of the 4 pipeline slots) */
struct cam_sub {
	struct frame_sink *sink;    /* NULL = empty slot                       */
	uint8_t            fmt;      /* enum camera_format consumed             */
	uint8_t            enabled;  /* subscriber intent (registered)          */
	uint8_t            attached; /* linked in cam_pipe.sinks (cam_lock)     */
	uint8_t            oneshot;  /* #101: non-persistent (MJPEG) -- a non-recover
	                               stop fully releases it (see
	                               cam_subs_release_oneshot); persistent subs
	                               (gui/nncam) stay enabled and re-attach. */
};
static struct cam_sub cam_subs[CAM_MAX_SUBS];

/* #101: release oneshot subs on a non-recover base stop -- forward-declared here
   because camera_power_off() (above the registry helpers) calls it. */
static void cam_subs_release_oneshot(void);

static volatile int      cam_stream_active;  /* streaming mode gate             */
static volatile int      cam_stop_req;       /* stop requested (producer drains)*/
static volatile int      cam_stream_err;     /* DCMI OVR -> terminal stop       */
static volatile uint32_t cam_stream_ovr;     /* DCMI overrun count              */
static volatile uint32_t cam_ring_ovr;       /* free-slot exhaustion / lost     */
static volatile uint32_t cam_stream_fe;      /* DMA FIFO/DME errors tolerated (#56) */
static volatile uint32_t cam_ring_slots;     /* ring depth this stream (#65 arena) */
static volatile uint32_t cam_ring_slot_bytes;/* ring slot stride this stream (#65) */
static uint32_t cam_start_tick;              /* HAL tick at start (--secs)       */
static uint32_t cam_target_frames;           /* 0 = unbounded                   */
static uint32_t cam_target_secs;             /* 0 = unbounded                   */
static uint32_t cam_last_ct;                 /* CT at last serviced completion  */
static struct frame_desc *cam_m0, *cam_m1;   /* slots currently in M0AR/M1AR    */
static struct frame_desc *cam_jpeg_slot;     /* JPEG stream: the single DMA target (#63) */
static volatile uint32_t cam_jpeg_trunc;     /* JPEG stream: frames with no SOI/EOI (#63) */

/* ---- base capture overrun auto-recovery (#100, contract 6) ----------------
   A DCMI overrun is a producer-general fault, not a subscriber concern: the base
   tears down (cascade close() to every subscriber) and the producer re-arms
   itself at the same mode; subscribers re-attach via cam_subs_attach_all() and
   just saw a close()/open() pair.  An escalating rapid-overrun counter breaks a
   persistent overrun loop -- after CAM_RECOVER_GIVEUP rapid recoveries the base
   stays off so the user can remove the bandwidth culprit (e.g. `ai stream stop`)
   and restart.  This replaces the old GUI-overlay-specific backoff (#83). */
#define CAM_RECOVER_WINDOW  8000u   /* ms: window for "rapid" successive recovery */
#define CAM_RECOVER_GIVEUP  6       /* rapid count -> stop auto-restarting        */
static volatile int cam_recover_pending;   /* last teardown was an overrun (recover) */
static int          cam_recover_rapid;     /* consecutive rapid recoveries           */
static uint32_t     cam_recover_last;      /* HAL tick of the last recovery          */
static int          cam_start_colorbar;    /* colorbar of the running base (recover) */

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

/* ---- mode timing / DCMI helpers (issue #45) ------------------------------ */

/* Toggle the DCMI hardware JPEG mode.  JPEGMode latches at HAL_DCMI_Init, so a
   switch needs a DeInit/Init with the new value; the cached hdcmi.Init keeps the
   sync/polarity/EDM config, and the DMA link + NVIC are restored after.  Only
   called under cam_lock with no capture in flight (stream/preview gate). */
static int dcmi_set_jpeg(int on)
{
	if (HAL_DCMI_DeInit(&hdcmi) != HAL_OK)
		return -1;
	hdcmi.Init.JPEGMode = on ? DCMI_JPEG_ENABLE : DCMI_JPEG_DISABLE;
	if (HAL_DCMI_Init(&hdcmi) != HAL_OK)
		return -1;
	__HAL_LINKDMA(&hdcmi, DMA_Handle, hdma_dcmi);
	return 0;
}

static int read_reg_u16(uint16_t reg_hi, uint16_t reg_lo, uint16_t *out)
{
	uint8_t hi, lo;

	if (cam_io_read(CAM_I2C_ADDR, reg_hi, &hi, 1) != OV5640_OK ||
	    cam_io_read(CAM_I2C_ADDR, reg_lo, &lo, 1) != OV5640_OK)
		return -1;
	*out = (uint16_t)(((uint16_t)hi << 8) | lo);
	return 0;
}

static int write_hts_vts(uint16_t hts, uint16_t vts)
{
	uint8_t b;

	b = (uint8_t)(hts >> 8);
	if (cam_io_write(CAM_I2C_ADDR, OV5640_TIMING_HTS_HIGH, &b, 1) != OV5640_OK)
		return -1;
	b = (uint8_t)hts;
	if (cam_io_write(CAM_I2C_ADDR, OV5640_TIMING_HTS_LOW, &b, 1) != OV5640_OK)
		return -1;
	b = (uint8_t)(vts >> 8);
	if (cam_io_write(CAM_I2C_ADDR, OV5640_TIMING_VTS_HIGH, &b, 1) != OV5640_OK)
		return -1;
	b = (uint8_t)vts;
	if (cam_io_write(CAM_I2C_ADDR, OV5640_TIMING_VTS_LOW, &b, 1) != OV5640_OK)
		return -1;
	return 0;
}

/* OV5640 daylight default for both AEC max-exposure pairs (common table value,
   ov5640.c).  OV5640_NightModeConfig(ENABLE) raises both to ~0x0B88; its DISABLE
   only clears the auto-frame-rate bit and does NOT restore this, so night-off
   would otherwise leave the ceiling (and thus the VTS reclamp) stuck high (#70). */
#define CAM_AEC_MAX_DEFAULT  0x03D8u

/* Restore the two AEC max-exposure ceilings (0x3A02/03 and 0x3A14/15) to the
   daylight default.  Only the VTS-driving ceilings are reset; the night banding
   steps / max-bands (0x3A08-0E) do not affect VTS and are out of scope (#70).
   Caller holds cam_lock and the sensor is configured.  Returns 0 on success. */
static int restore_aec_max_default_locked(void)
{
	uint8_t b;

	b = (uint8_t)(CAM_AEC_MAX_DEFAULT >> 8);
	if (cam_io_write(CAM_I2C_ADDR, OV5640_AEC_CTRL02, &b, 1) != OV5640_OK)
		return -1;
	if (cam_io_write(CAM_I2C_ADDR, OV5640_AEC_MAX_EXPO_HIGH, &b, 1) != OV5640_OK)
		return -1;
	b = (uint8_t)CAM_AEC_MAX_DEFAULT;
	if (cam_io_write(CAM_I2C_ADDR, OV5640_AEC_CTRL03, &b, 1) != OV5640_OK)
		return -1;
	if (cam_io_write(CAM_I2C_ADDR, OV5640_AEC_MAX_EXPO_LOW, &b, 1) != OV5640_OK)
		return -1;
	return 0;
}

/* AEC needs VTS to cover the max exposure (in lines) or frames clip / band.  The
   OV5640 holds two max-exposure pairs (0x3A02/03 and 0x3A14/15, default 0x3D8 =
   984; night mode raises BOTH to ~0x0B88).  Write the mode's HTS and an effective
   VTS = max(mode.vts base, max exposure + margin) so the auto-exposure always has
   room, and refresh the live fps target.  mode.vts stays the fps-table base, and
   settings_and_timing_apply_locked() restores the ceiling on night-off (#70), so
   turning night mode off drops the effective VTS back to the table value.  Caller
   holds cam_lock and the sensor is configured. */
#define CAM_VTS_EXP_MARGIN  16u
static int apply_vts_locked(void)
{
	uint16_t e1, e2, eff;

	if (read_reg_u16(OV5640_AEC_CTRL02, OV5640_AEC_CTRL03, &e1) != 0 ||
	    read_reg_u16(OV5640_AEC_MAX_EXPO_HIGH, OV5640_AEC_MAX_EXPO_LOW, &e2) != 0)
		return -1;
	eff = (uint16_t)((e1 > e2 ? e1 : e2) + CAM_VTS_EXP_MARGIN);
	if (eff < mode.vts)
		eff = mode.vts;
	if (write_hts_vts(mode.hts, eff) != 0)
		return -1;
	if (mode.hts != 0u && eff != 0u)
		mode.fps_target_x10 = (uint16_t)((uint64_t)mode.pclk_hz * 10u /
		                                 ((uint32_t)mode.hts * eff));
	return 0;
}

/* Reset the mode descriptor to the power-on QVGA RGB565 default (common-table
   timing) and put the DCMI back to raster.  A power cycle / re-probe / failed
   mode switch returns here, so mode / sensor / DCMI stay mutually consistent. */
static void mode_reset_default(void)
{
	struct camera_mode d = {
		.res = CAM_RES_QVGA, .format = CAM_FMT_RGB565,
		.hts = 1600u, .vts = 1000u, .pclk_hz = 24000000u,   /* QVGA fps-table */
	};

	if (hdcmi.Init.JPEGMode == DCMI_JPEG_ENABLE)
		(void)dcmi_set_jpeg(0);
	/* Invalidate the lib's init cache too: OV5640_Init only re-writes the common
	   table / resolution / format while IsInitialized == 0 (ov5640.c sticky
	   guard), so without this a re-configure after a power cycle / failed switch
	   would be a no-op and leave the sensor unprogrammed (mode/sensor mismatch). */
	ov5640.IsInitialized = 0U;
	cam_sensor_pclk_hz = 0u;   /* sensor PLL back at power-on default (#67) */
	mode_recompute(&d);
	mode = d;
}

/* ---- power + probe (locked helpers) -------------------------------------- */

static void power_off_locked(void)
{
	HAL_GPIO_WritePin(CAM_PWR_PORT, CAM_PWR_PIN, GPIO_PIN_SET);
	info.chip_id     = 0;
	info.powered     = 0;
	info.configured  = 0;
	info.frame_valid = 0;
	info.frame_bytes = 0;
	cam_colorbar     = -1;
	/* The sensor lost power: any selected mode no longer matches it, so reset
	   the descriptor (and the DCMI JPEG state) to the QVGA RGB565 default that
	   the next lazy OV5640_Init will program -- keeps mode / sensor / DCMI
	   consistent across a power cycle (#45). */
	mode_reset_default();
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
	info.frame_bytes = 0;
	cam_colorbar     = -1;   /* power cycle reset the sensor registers */
	mode_reset_default();    /* sensor is back at power-on defaults (#45) */

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

/* ---- quality settings (locked helpers, issue #44) ------------------------ */

/* Map the port-neutral enums to the lib/ov5640 constants. */
static uint32_t awb_to_ov(uint8_t a)
{
	switch (a) {
	case CAM_AWB_SUNNY:  return OV5640_LIGHT_SUNNY;
	case CAM_AWB_OFFICE: return OV5640_LIGHT_OFFICE;
	case CAM_AWB_HOME:   return OV5640_LIGHT_HOME;
	case CAM_AWB_CLOUDY: return OV5640_LIGHT_CLOUDY;
	default:             return OV5640_LIGHT_AUTO;
	}
}

static uint32_t effect_to_ov(uint8_t e)
{
	switch (e) {
	case CAM_FX_BW:       return OV5640_COLOR_EFFECT_BW;
	case CAM_FX_SEPIA:    return OV5640_COLOR_EFFECT_SEPIA;
	case CAM_FX_NEGATIVE: return OV5640_COLOR_EFFECT_NEGATIVE;
	case CAM_FX_BLUE:     return OV5640_COLOR_EFFECT_BLUE;
	case CAM_FX_RED:      return OV5640_COLOR_EFFECT_RED;
	case CAM_FX_GREEN:    return OV5640_COLOR_EFFECT_GREEN;
	default:              return OV5640_COLOR_EFFECT_NONE;
	}
}

static uint32_t flip_to_ov(uint8_t f)
{
	switch (f) {
	case CAM_FLIP_MIRROR: return OV5640_MIRROR;
	case CAM_FLIP_FLIP:   return OV5640_FLIP;
	case CAM_FLIP_BOTH:   return OV5640_MIRROR_FLIP;
	default:              return OV5640_MIRROR_FLIP_NONE;
	}
}

static uint32_t zoom_to_ov(uint8_t z)
{
	switch (z) {
	case 2:  return OV5640_ZOOM_x2;
	case 4:  return OV5640_ZOOM_x4;
	case 8:  return OV5640_ZOOM_x8;
	default: return OV5640_ZOOM_x1;
	}
}

/* A tinting effect (bw/sepia/blue/red/green) fixes the U/V channels via
   SDE_CTRL3/4; saturation and hue write those same registers, so they fight.
   Negative and "none" leave U/V alone, so saturation/hue coexist with them. */
static int effect_is_tint(uint8_t e)
{
	return e == CAM_FX_BW || e == CAM_FX_SEPIA || e == CAM_FX_BLUE ||
	       e == CAM_FX_RED || e == CAM_FX_GREEN;
}

/*
 * Push the whole cached settings block to a powered+configured sensor and fix
 * up SDE_CTRL0 so the SDE controls coexist.  Applied in a fixed order so the
 * shared SDE_CTRL8 sign/enable bits (touched via the ST setters' read-modify-
 * writes) settle deterministically; SDE_CTRL0 is then rewritten as the OR of
 * every active function's enable bit (the ST setters each leave only their own).
 * Caller holds cam_lock and has info.configured == 1.
 */
static int settings_apply_all_locked(void)
{
	/* Mirrors lib/ov5640 hue_degree_ctrl8[] (OV5640 hue sin/cos sign bits,
	   masked to SDE8_HUE_SIGNS); indexed by settings.hue - CAM_HUE_MIN. */
	static const uint8_t hue_sign[CAM_HUE_MAX - CAM_HUE_MIN + 1] = {
		0x32u, 0x32u, 0x32u, 0x02u, 0x02u, 0x02u,
		0x01u, 0x01u, 0x01u, 0x31u, 0x31u, 0x31u
	};
	int tint = effect_is_tint(settings.effect);
	uint8_t sde0, sde8, want = 0, want8;

	if (OV5640_SetLightMode(&ov5640, awb_to_ov(settings.awb)) != OV5640_OK)
		goto io_err;
	if (OV5640_SetColorEffect(&ov5640, effect_to_ov(settings.effect))
	    != OV5640_OK)
		goto io_err;
	if (OV5640_SetBrightness(&ov5640, settings.brightness) != OV5640_OK)
		goto io_err;
	if (OV5640_SetContrast(&ov5640, settings.contrast) != OV5640_OK)
		goto io_err;
	if (!tint) {
		if (OV5640_SetSaturation(&ov5640, settings.saturation) != OV5640_OK)
			goto io_err;
		if (OV5640_SetHueDegree(&ov5640, settings.hue) != OV5640_OK)
			goto io_err;
	}

	/* SDE_CTRL8 fixup: the OR-only setters above cannot clear their shared
	   sign bits, so rewrite the managed bits with the exact value the cache
	   implies (see OV5640_REG_SDE_CTRL8 comment). */
	want8 = SDE8_UV_MANUAL;
	if (settings.brightness < 0)
		want8 |= SDE8_YBRIGHT_SIGN;
	if (!tint)
		want8 |= (uint8_t)(hue_sign[settings.hue - CAM_HUE_MIN] &
		                   SDE8_HUE_SIGNS);
	if (cam_io_read(CAM_I2C_ADDR, OV5640_REG_SDE_CTRL8, &sde8, 1) != OV5640_OK)
		goto io_err;
	sde8 = (uint8_t)((sde8 & (uint8_t)~SDE8_MANAGED) | want8);
	if (cam_io_write(CAM_I2C_ADDR, OV5640_REG_SDE_CTRL8, &sde8, 1) != OV5640_OK)
		goto io_err;

	/* SDE_CTRL0 fixup -- combine the enable bits the setters above each wiped. */
	if (settings.effect == CAM_FX_NEGATIVE)
		want |= SDE0_NEG_EN;
	else if (tint)
		want |= SDE0_TINT;
	want |= SDE0_CONTRAST_EN;             /* brightness + contrast (Y path) */
	if (!tint)
		want |= SDE0_SAT_EN | SDE0_HUE_EN;

	if (cam_io_read(CAM_I2C_ADDR, OV5640_REG_SDE_CTRL0, &sde0, 1) != OV5640_OK)
		goto io_err;
	sde0 = (uint8_t)((sde0 & (uint8_t)~SDE0_MANAGED) | want);
	if (cam_io_write(CAM_I2C_ADDR, OV5640_REG_SDE_CTRL0, &sde0, 1) != OV5640_OK)
		goto io_err;

	/* Geometry / exposure controls are independent of the SDE block. */
	if (OV5640_MirrorFlipConfig(&ov5640, flip_to_ov(settings.flip)) != OV5640_OK)
		goto io_err;
	if (OV5640_ZoomConfig(&ov5640, zoom_to_ov(settings.zoom)) != OV5640_OK)
		goto io_err;
	if (OV5640_NightModeConfig(&ov5640,
	        settings.night ? NIGHT_MODE_ENABLE : NIGHT_MODE_DISABLE)
	    != OV5640_OK)
		goto io_err;

	settings_dirty = 0;
	return 0;
io_err:
	LOG_ERR("OV5640 settings apply failed (I2C)");
	return CAM_ERR_HAL;
}

/* Apply the cached quality settings AND refresh the exposure-aware VTS in one
   shot.  This is the only path that touches sensor timing, so it is used by the
   operations that actually change the AEC ceiling or base VTS: night toggle,
   resolution/format change, and configure (SDE/geometry-only setters use the
   no-timing apply_if_live_locked()).  night ON raised both AEC max-exposure pairs
   to ~0x0B88 inside settings_apply_all_locked(); when night is OFF the vendored
   DISABLE leaves them stuck high, so restore the daylight ceiling here before the
   VTS clamp reads it -- otherwise the effective VTS (and fps) never come back down
   (#70).  Timing failures re-arm timing_dirty (NOT settings_dirty, which the SDE
   block already cleared) so a later live setter / configure retries just the
   timing step without an SDE-only apply silently dropping it.  Caller holds
   cam_lock with the sensor configured and the mode committed. */
static int settings_and_timing_apply_locked(void)
{
	int rc = settings_apply_all_locked();   /* clears settings_dirty on success */

	if (rc != 0) {
		timing_dirty = 1;
		return rc;
	}
	if (settings.night == 0 && restore_aec_max_default_locked() != 0) {
		timing_dirty = 1;     /* do NOT clamp VTS off a stale night ceiling */
		LOG_ERR("OV5640 AEC ceiling restore failed (I2C)");
		return CAM_ERR_HAL;
	}
	rc = apply_vts_locked();
	if (rc != 0) {
		timing_dirty = 1;
		return rc;
	}
	timing_dirty = 0;         /* SDE and timing both match the sensor now */
	return 0;
}

/* Apply now if the sensor is live; otherwise the cache is enough.  Cache-only
   when not configured (the next capture's lazy configure re-applies it after
   OV5640_Init wipes the SDE block) or while the colorbar test pattern is up
   (returning to live re-applies, and we must not disturb its SDE_CTRL4).

   For SDE/geometry-only setters (brightness..zoom) this does NOT reclamp the VTS:
   those settings cannot change the AEC max exposure, so re-running the exposure-
   aware VTS clamp on every tweak only risked dropping fps for no reason (#70).
   The one exception is a pending timing retry (timing_dirty): then fall back to
   the full timing apply so a prior failed ceiling-restore / VTS clamp recovers on
   the next live operation instead of waiting for the next configure. */
static int apply_if_live_locked(void)
{
	if (!info.configured || cam_colorbar != 0)
		return 0;
	if (timing_dirty)
		return settings_and_timing_apply_locked();
	return settings_apply_all_locked();
}

/* Like apply_if_live_locked() but always refreshes the timing (AEC ceiling +
   VTS): used by the operations that change the AEC max exposure / base VTS --
   night toggle and defaults reset (resolution/format and configure call
   settings_and_timing_apply_locked() directly). */
static int apply_with_timing_if_live_locked(void)
{
	if (!info.configured || cam_colorbar != 0)
		return 0;
	return settings_and_timing_apply_locked();
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
		cam_colorbar    = -1;   /* forces the mode block below to run */
		cam_sensor_pclk_hz = 0u;   /* Init reset the PLL; apply_effective re-sets (#67) */
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
		tx_thread_sleep(CAM_SETTLE_MODE_MS);

		/* Re-apply the cached quality settings LAST, in live mode only:
		   OV5640_Init and ColorbarModeConfig(DISABLE) both write the SDE
		   register block (the latter sets SDE_CTRL4), so the settings must
		   land after them or they get clobbered (issue #44).  The colorbar
		   test pattern owns its own SDE_CTRL4 and ignores quality, so skip
		   it there.  cam_colorbar is committed only after a successful apply
		   so a failed apply re-runs this block on the next capture. */
		if (!colorbar) {
			int rc = settings_and_timing_apply_locked();

			if (rc != 0)
				return rc;
		}
		cam_colorbar = colorbar;
	}

	/* Mode unchanged but a prior live apply failed: retry so the sensor matches
	   the cache.  settings_dirty covers the SDE/geometry/night cache; timing_dirty
	   covers a failed AEC-ceiling restore / VTS clamp (which an SDE-only apply does
	   not re-run, #70).  Either one means a full timing apply is owed.  Skipped in
	   colorbar mode -- quality is irrelevant there and would disturb the test
	   pattern's SDE_CTRL4. */
	if (!colorbar && (settings_dirty || timing_dirty)) {
		int rc = settings_and_timing_apply_locked();

		if (rc != 0)
			return rc;
	}
	return 0;
}

/* ---- mode switch: resolution / pixel format / fps (issue #45) ------------- */

/*
 * Per-resolution timing table.  fps = pclk_hz / (hts * vts); the PCLK is no longer
 * a fixed column -- it is the runtime fps knob (#67), see effective_ov_pclk().
 * HTS/VTS is tightened on the small streamable modes (1600x1000) so 24 MHz gives a
 * clean ~15 fps and 48 MHz exactly doubles it to ~30 fps (48e6/(1600*1000)=30.0);
 * VGA/WVGA keep the full common timing (snapshot-only, frame rate irrelevant, and
 * pinned to 24 MHz).  VTS is the BASE: apply_vts_locked() raises the effective VTS
 * when the AEC max exposure needs more lines and drops it back to this base.
 */
static const struct {
	uint16_t hts;
	uint16_t vts;
} mode_fps[CAM_RES__COUNT] = {
	[CAM_RES_QQVGA]   = { 1600u, 1000u },
	[CAM_RES_QVGA]    = { 1600u, 1000u },
	[CAM_RES_VGA]     = { 1936u, 1088u },
	[CAM_RES_WVGA]    = { 1936u, 1088u },
};

/* Only the small modes (QQVGA/QVGA) are streamable and worth 48 MHz; the larger
   VGA/WVGA modes are snapshot-only and stay at 24 MHz regardless of fps. */
static int res_is_small(uint8_t res)
{
	return res <= CAM_RES_QVGA;
}

struct cam_pclk_sel { uint8_t ov; uint32_t hz; };

/*
 * The single safety predicate (#67): 48 MHz (30 fps) only when 30 fps is selected
 * AND the mode is small AND the LTDC is NOT scanning out.  Any missing condition
 * clamps to 24 MHz (15 fps) so the 48 MHz DCMI burst cannot overrun the 16-bit
 * SDRAM that the LTDC reads continuously.  ltdc_scanout_active() is false when the
 * LTDC never came up (no display = no contention), which correctly allows 48 MHz.
 */
static struct cam_pclk_sel effective_ov_pclk(uint8_t res)
{
	struct cam_pclk_sel s = { OV5640_PCLK_24M, 24000000u };

	if (cam_fps_sel == 30u && res_is_small(res) && !ltdc_scanout_active()) {
		s.ov = OV5640_PCLK_48M;
		s.hz = 48000000u;
	}
	return s;
}

/*
 * Re-apply the effective PCLK to the live sensor if it differs from what is
 * actually programmed (cam_sensor_pclk_hz).  Called before arming a stream or a
 * snapshot so the sensor PCLK always matches (fps_sel, res, scanout) even when the
 * scanout state changed since the last set_format -- and it fixes the old #45
 * asymmetry where the snapshot lazy-configure never set PCLK at all.  Caller holds
 * cam_lock with the sensor configured.
 */
static int apply_effective_pclk_locked(void)
{
	struct cam_pclk_sel sel = effective_ov_pclk(mode.res);

	if (sel.hz == cam_sensor_pclk_hz)
		return 0;                         /* already programmed (scanout unchanged) */
	if (OV5640_SetPCLK(&ov5640, sel.ov) != OV5640_OK) {
		cam_sensor_pclk_hz = 0u;          /* desynced: force a re-write next time */
		LOG_ERR("OV5640_SetPCLK failed");
		return CAM_ERR_HAL;
	}
	cam_sensor_pclk_hz = sel.hz;
	mode.pclk_hz       = sel.hz;
	if (mode.hts != 0u && mode.vts != 0u)
		mode.fps_target_x10 = (uint16_t)((uint64_t)sel.hz * 10u /
		                                 ((uint32_t)mode.hts * mode.vts));
	/* SetPCLK writes 0x3036/0x3037 (PLL multiplier / root divider) with no internal
	   delay, so the PLL must re-lock: wait >1 frame before arming (#67, codex). */
	tx_thread_sleep(CAM_SETTLE_MODE_MS);
	return 0;
}

static uint32_t res_to_ov(uint8_t r)
{
	switch (r) {
	case CAM_RES_QQVGA:   return OV5640_R160x120;
	case CAM_RES_VGA:     return OV5640_R640x480;
	case CAM_RES_WVGA:    return OV5640_R800x480;
	default:              return OV5640_R320x240;   /* QVGA */
	}
}

static uint32_t fmt_to_ov(uint8_t f)
{
	switch (f) {
	case CAM_FMT_YUV422: return OV5640_YUV422;
	case CAM_FMT_Y8:     return OV5640_Y8;
	case CAM_FMT_JPEG:   return OV5640_JPEG;
	default:             return OV5640_RGB565;
	}
}

/* Map the port-neutral format to the pipeline's frame_format (for publish). */
static enum frame_format fmt_to_frame(uint8_t f)
{
	switch (f) {
	case CAM_FMT_YUV422: return FRAME_FMT_YUV422;
	case CAM_FMT_Y8:     return FRAME_FMT_Y8;
	case CAM_FMT_JPEG:   return FRAME_FMT_JPEG;
	default:             return FRAME_FMT_RGB565;
	}
}

/*
 * Switch the live resolution / pixel format and apply the per-mode fps timing.
 * Refused while streaming/preview owns the DCMI (the ring is a live target).
 * On any I/O failure the sensor is marked unconfigured and the mode is reset to
 * the QVGA RGB565 default, so a half-applied sensor/DCMI state never persists --
 * the next capture full-re-inits to a state that matches the descriptor (#45).
 * cam_lock held by the caller.
 */
static int camera_set_format_locked(uint8_t res, uint8_t fmt)
{
	struct camera_mode m;
	int jpeg = (fmt == CAM_FMT_JPEG);
	int rc;

	if (cam_stream_active)
		return CAM_ERR_BUSY;           /* base capture owns the DCMI/DMA (#100) */
	if (!sdram_is_up())
		return CAM_ERR_STATE;
	if (res >= CAM_RES__COUNT || fmt >= CAM_FMT__COUNT)
		return CAM_ERR_PARAM;
	if (jpeg && res > CAM_RES_VGA)
		return CAM_ERR_PARAM;          /* JPEG is snapshot-only, gated <= VGA */

	/* Candidate mode: geometry from res + HTS/VTS from the timing table + the
	   effective PCLK from the fps knob (#67).  Validate capacity before the sensor. */
	struct cam_pclk_sel psel = effective_ov_pclk(res);

	m = (struct camera_mode){ .res = res, .format = fmt,
	    .hts = mode_fps[res].hts, .vts = mode_fps[res].vts,
	    .pclk_hz = psel.hz };
	mode_recompute(&m);
	if (m.frame_bytes > CAM_FRAME_MAX_BYTES)
		return CAM_ERR_PARAM;

	if (!info.powered) {
		rc = camera_probe_locked(NULL);
		if (rc != 0)
			return rc;
	}

	/* A JPEG <-> raster change cannot be applied incrementally: the lib's
	   OV5640_SetPixelFormat SETS the JPEG datapath bits (TIMING_TC_REG21 /
	   SYSREM_RESET02 / CLOCK_ENABLE02) but never clears them, so a raster capture
	   after a JPEG one wedges (no sync -> timeout).  Force a clean sensor re-init:
	   clearing IsInitialized lets OV5640_Init re-run the common table, whose first
	   write is a software reset (SYSTEM_CTROL0=0x82) that restores those bits.
	   Toggle the DCMI JPEGMode to match. */
	if (jpeg != (int)mode.is_jpeg) {
		ov5640.IsInitialized = 0U;
		info.configured = 0;
		if (dcmi_set_jpeg(jpeg) != 0) {
			LOG_ERR("DCMI JPEG re-init failed");
			goto fail;
		}
	}

	if (!info.configured) {
		if (OV5640_Init(&ov5640, OV5640_R320x240, OV5640_RGB565) != OV5640_OK) {
			LOG_ERR("OV5640_Init failed");
			goto fail;
		}
		info.configured = 1;
		cam_sensor_pclk_hz = 0u;   /* Init reprogrammed the PLL to its default (#67) */
		tx_thread_sleep(CAM_SETTLE_INIT_MS);
	}

	if (OV5640_SetResolution(&ov5640, res_to_ov(res)) != OV5640_OK)
		goto fail;
	if (OV5640_SetPixelFormat(&ov5640, fmt_to_ov(fmt)) != OV5640_OK)
		goto fail;
	if (OV5640_SetPCLK(&ov5640, psel.ov) != OV5640_OK)      /* fps knob (#67) */
		goto fail;
	cam_sensor_pclk_hz = psel.hz;   /* sensor now matches the effective selection */

	/* Commit the base mode now: the quality re-apply + timing below needs the
	   committed mode (ZoomConfig reads the new DVPHO/DVPVO, night mode rewrites
	   AEC, and apply_vts writes the effective VTS from mode.hts/vts).  A failure
	   after this lands in the fail path, which resets to the default. */
	mode = m;
	rc = settings_and_timing_apply_locked();   /* SDE settings + HTS/VTS clamp */
	if (rc != 0)
		goto fail;

	info.frame_valid = 0;              /* geometry changed: old frame is stale */
	info.frame_bytes = 0;
	cam_colorbar     = -1;             /* next capture re-establishes live/colorbar */
	tx_thread_sleep(CAM_SETTLE_MODE_MS);
	return 0;

fail:
	/* Consistent fallback: mode + DCMI reset to the QVGA RGB565 default and the
	   sensor flagged for re-init, so a half-applied sensor/DCMI/mode state never
	   persists -- the next capture re-programs to a state matching the descriptor. */
	info.configured  = 0;
	info.frame_valid = 0;
	info.frame_bytes = 0;
	cam_colorbar     = -1;
	mode_reset_default();
	LOG_ERR("camera_set_format(res=%u fmt=%u) failed", (unsigned)res,
	        (unsigned)fmt);
	return CAM_ERR_HAL;
}

/* Rebuild the DMA stream after an aborted snapshot.  A VGA/WVGA snapshot runs
   the HAL intra-frame banding path, which sets CR.DBM; a normal completion
   self-heals (the next Start clears or re-sets DBM as needed), but an aborted
   banded transfer (timeout / DCMI error) can leave DBM/CT stale, so re-init the
   stream to a clean DMA_NORMAL state for the next capture (#45 R8). */
static void snapshot_dma_reinit(void)
{
	(void)HAL_DMA_DeInit(&hdma_dcmi);
	(void)HAL_DMA_Init(&hdma_dcmi);
	__HAL_LINKDMA(&hdcmi, DMA_Handle, hdma_dcmi);
}

/* Validate + EOI-trim a JPEG frame in @p buf.  @p eff_bytes is how many bytes the
   DMA actually wrote (budget - NDTR, in bytes); the real stream is shorter because
   the DCMI zero-pads the last 32-bit word.  Verify the SOI (0xFFD8) at the start
   and scan backward within eff_bytes for the EOI marker (0xFFD9); on success
   *valid receives the trimmed length (EOI+2).  No SOI/EOI -> truncated (CAM_ERR_HAL).
   Only the captured range is scanned (the .sdram buffer is NOLOAD: it can hold an
   older/garbage tail beyond eff_bytes).  Silent: the caller logs / counts -- this
   serves both the snapshot finalize and the JPEG streaming producer (#63). */
static int jpeg_trim(const uint8_t *buf, uint32_t eff_bytes, uint32_t *valid)
{
	uint32_t i;

	if (eff_bytes < 4u || buf[0] != 0xFFu || buf[1] != 0xD8u)
		return CAM_ERR_HAL;                    /* no SOI */
	for (i = eff_bytes; i >= 2u; i--) {
		if (buf[i - 2u] == 0xFFu && buf[i - 1u] == 0xD9u) {
			*valid = i;                        /* include the EOI marker */
			return 0;
		}
	}
	return CAM_ERR_HAL;                        /* no EOI: truncated */
}

static int camera_capture_locked(int colorbar)
{
	int rc;

	if (!sdram_is_up())
		return CAM_ERR_STATE;

	if (cam_stream_active)
		return CAM_ERR_BUSY;    /* streaming owns the DCMI/DMA (issue #46) */

	if (!info.powered) {
		rc = camera_probe_locked(NULL);
		if (rc != 0)
			return rc;
	}

	rc = camera_configure_locked(colorbar);
	if (rc != 0)
		return rc;

	/* Program the effective PCLK for the fps knob + current scanout state (#67);
	   also covers the old #45 asymmetry (lazy configure never set PCLK). */
	rc = apply_effective_pclk_locked();
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
	/* JPEG is a variable-length frame: the DMA never completes (the budget
	   exceeds the stream), so the DMA-complete path that arms the FRAME
	   interrupt for a raster snapshot never runs.  Enable FRAME explicitly so the
	   hardware end-of-frame (RM0385 17.4) still wakes us. */
	__HAL_DCMI_ENABLE_IT(&hdcmi, DCMI_IT_ERR | DCMI_IT_OVR |
	                             (mode.is_jpeg ? DCMI_IT_FRAME : 0u));

	/* One contiguous transfer of mode.frame_words.  QVGA = 38400 words runs a
	   plain single DMA; VGA/WVGA exceed 65535 words and HAL_DCMI_Start_DMA
	   transparently bands them (intra-frame DBM into this one buffer), firing
	   FRAME once at the end either way.  No cache maintenance -- cam_frame is in
	   the MPU non-cacheable SDRAM. */
	if (HAL_DCMI_Start_DMA(&hdcmi, DCMI_MODE_SNAPSHOT, (uint32_t)cam_frame,
	                       mode.frame_words) != HAL_OK) {
		cam_xfer_active = 0;
		(void)HAL_DCMI_Stop(&hdcmi);
		drain_done();
		LOG_ERR("DCMI start failed (err 0x%lx)",
		        (unsigned long)HAL_DCMI_GetError(&hdcmi));
		return CAM_ERR_HAL;
	}

	/* The banded (>64 KB) path goes through HAL_DMAEx_MultiBufferStart_IT, which
	   -- unlike the single-transfer HAL_DMA_Start_IT -- enables the DMA FIFO/direct
	   -mode error interrupts.  Under SDRAM contention these FE/DME fire and HAL's
	   DCMI_DMAError aborts the whole snapshot, so VGA/WVGA captures spuriously fail
	   with CAM_ERR_HAL.  Per RM0385 8.5.5 FE/DME do NOT stop the stream, so silence
	   them; a real transfer error (TE) and the DCMI overrun/sync error (DCMI_IT_OVR
	   /ERR) stay terminal.  Harmless on the single-transfer path: FE is off there,
	   and DME (enabled by HAL_DMA_Start_IT too) is benign to silence for a snapshot. */
	__HAL_DMA_DISABLE_IT(&hdma_dcmi, DMA_IT_FE);
	__HAL_DMA_DISABLE_IT(&hdma_dcmi, DMA_IT_DME);

	if (tx_semaphore_get(&cam_done, CAM_XFER_TIMEOUT_TICKS) != TX_SUCCESS) {
		cam_xfer_active = 0;
		(void)HAL_DCMI_Stop(&hdcmi);
		snapshot_dma_reinit();   /* clear any stale banding DBM/CT */
		drain_done();
		LOG_ERR("frame timed out (no DCMI sync? check wiring)");
		return CAM_ERR_TIMEOUT;
	}
	cam_xfer_active = 0;

	if (cam_xfer_err) {
		(void)HAL_DCMI_Stop(&hdcmi);
		snapshot_dma_reinit();   /* clear any stale banding DBM/CT */
		drain_done();
		LOG_ERR("capture error (HAL err 0x%lx)",
		        (unsigned long)HAL_DCMI_GetError(&hdcmi));
		return CAM_ERR_HAL;
	}

	/* Snapshot auto-cleared CAPTURE; Stop also disables the DCMI, aborts the DMA
	   and leaves the HAL in a clean READY state for the next capture. */
	(void)HAL_DCMI_Stop(&hdcmi);

	if (mode.is_jpeg) {
		/* The DMA was aborted mid-stream by Stop; NDTR holds the untransferred
		   word count, so (budget - NDTR) words were written.  Read it AFTER Stop
		   (not before -- the abort settles NDTR), then trim to the JPEG EOI. */
		uint32_t ndtr = __HAL_DMA_GET_COUNTER(&hdma_dcmi);
		uint32_t eff  = (mode.frame_words - ndtr) * 4u;
		uint32_t valid;

		rc = jpeg_trim(cam_frame, eff, &valid);
		if (rc != 0) {
			LOG_ERR("JPEG finalize failed (eff=%lu)", (unsigned long)eff);
			info.frame_valid = 0;
			return rc;
		}
		info.frame_bytes = valid;              /* trimmed JPEG stream length */
	} else {
		info.frame_bytes = mode.frame_bytes;   /* raster: full frame valid */
	}
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

int camera_set_format(enum camera_res res, enum camera_format fmt)
{
	int rc = op_lock();

	if (rc != 0)
		return rc;
	rc = camera_set_format_locked((uint8_t)res, (uint8_t)fmt);
	op_unlock();
	return rc;
}

int camera_set_fps(unsigned fps)
{
	uint8_t prev;
	int rc;

	if (fps != 15u && fps != 30u)
		return CAM_ERR_PARAM;
	rc = op_lock();
	if (rc != 0)
		return rc;
	/* Re-apply to the live sensor the same way `camera res` / `camera format` do:
	   rebuild the current mode so the new effective PCLK is reprogrammed at once.
	   Inherits the set_format BUSY gate (refused while a stream/preview owns the
	   DCMI) -- the sensor PLL must not be retuned under a live DMA target (#67).
	   On any failure (BUSY / I/O) restore the previous selection so the stored
	   preference never diverges from what the sensor actually got -- otherwise
	   camera_get_mode() would report an fps the live sensor never received. */
	prev = cam_fps_sel;
	cam_fps_sel = (uint8_t)fps;
	rc = camera_set_format_locked(mode.res, mode.format);
	if (rc != 0)
		cam_fps_sel = prev;
	op_unlock();
	return rc;
}

int camera_frame_read(uint32_t offset, void *dst, uint32_t len,
                      uint32_t *gen)
{
	int rc;

	if (dst == NULL || len == 0u)
		return CAM_ERR_PARAM;

	rc = op_lock();
	if (rc != 0)
		return rc;
	if (!info.frame_valid) {
		op_unlock();
		return CAM_ERR_NO_FRAME;
	}
	/* Bound against the captured frame's valid length under the lock: the live
	   mode (hence the valid byte count, and for JPEG the trimmed stream length)
	   can change between captures.  Subtraction form so offset+len cannot wrap. */
	if (offset >= info.frame_bytes ||
	    len > info.frame_bytes - offset) {
		op_unlock();
		return CAM_ERR_PARAM;
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

int camera_snapshot_latest(void)
{
	int rc = op_lock();

	if (rc != 0)
		return rc;
	/* #102: base ON -> refresh the stable cam_frame buffer from the latest published
	   ring frame so save/send always get the newest frame (any format, and even while
	   MJPEG/GUI subscribers are attached).  The pin keeps the slot out of the
	   producer's acquire() while we copy OUTSIDE the pipeline lock (pin held only across
	   a brief refcount++/--), so the producer's publish/DMA-repoint is never stalled.
	   base OFF -> keep the last `camera capture` frame (non-destructive, #102 decision);
	   no captured frame at all -> CAM_ERR_NO_FRAME. */
	if (cam_stream_active) {
		const struct frame_desc *f = frame_pipeline_pin_latest(&cam_pipe);

		if (f == NULL) {
			op_unlock();
			return CAM_ERR_NO_FRAME;   /* stream up but no frame published yet */
		}
		if (f->bytes <= CAM_FRAME_MAX_BYTES) {
			memcpy(cam_frame, f->data, f->bytes);   /* pin-protected, lock-free */
			info.frame_bytes = f->bytes;
			info.frame_valid = 1;
			cam_frame_gen++;
			rc = 0;
		} else {
			rc = CAM_ERR_PARAM;        /* cannot happen: budget < cam_frame */
		}
		frame_pipeline_put(&cam_pipe, NULL, f);     /* release the pin */
	} else {
		rc = info.frame_valid ? 0 : CAM_ERR_NO_FRAME;
	}
	op_unlock();
	return rc;
}

int camera_streaming(void)
{
	return cam_stream_active;   /* volatile; a destructive .sdram op must refuse */
}

int camera_probe(uint32_t *chip_id)
{
	int rc = op_lock();
	if (rc != 0)
		return rc;
	if (cam_stream_active) {          /* streaming owns the sensor/DCMI (#46) */
		op_unlock();
		return CAM_ERR_BUSY;
	}
	rc = camera_probe_locked(chip_id);
	op_unlock();
	return rc;
}

int camera_power_off(void)
{
	int rc = op_lock();
	if (rc != 0)
		return rc;
	if (cam_stream_active) {          /* cutting power mid-stream wrecks the HW */
		op_unlock();
		return CAM_ERR_BUSY;
	}
	cam_recover_pending = 0;          /* do not auto-recover into a powered-off sensor */
	cam_subs_release_oneshot();       /* #101: power off cancels recovery -> release oneshot */
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

int camera_get_mode(struct camera_mode *out)
{
	struct cam_pclk_sel sel;
	int rc;

	if (out == NULL)
		return CAM_ERR_PARAM;
	rc = op_lock();
	if (rc != 0)
		return rc;
	*out = mode;
	/* Evaluate the fps clamp live against the current LTDC scanout state (#67), so
	   `camera info` reflects whether 30 fps is actually in effect right now -- the
	   sensor PCLK is only re-applied at the next capture/stream arm, but the report
	   must be current.  Report the live-effective PCLK / fps target, not a stale
	   last-programmed value. */
	sel = effective_ov_pclk(mode.res);
	out->fps_sel = cam_fps_sel;
	out->fps_eff = (sel.hz >= 48000000u) ? 30u : 15u;
	if (cam_fps_sel == 30u && out->fps_eff == 15u)
		out->fps_clamp = res_is_small(mode.res) ? CAM_FPS_CLAMP_LTDC
		                                        : CAM_FPS_CLAMP_SIZE;
	else
		out->fps_clamp = CAM_FPS_OK;
	out->pclk_hz = sel.hz;
	/* fps target = effective PCLK / (HTS x effective VTS).  Use the VTS actually
	   programmed in the sensor, not the fps-table base, so the report follows the
	   exposure-aware VTS reclamp (night mode raises VTS -> lower fps).  Otherwise
	   the field would mix a live PCLK with a base VTS and read 15.0 while the sensor
	   runs at ~5 fps in night mode (#71).  Read it back when configured (the sensor
	   is the source of truth); fall back to the base table value otherwise or on a
	   read error. */
	{
		uint16_t vts = mode.vts;

		if (info.configured)
			(void)read_reg_u16(OV5640_TIMING_VTS_HIGH, OV5640_TIMING_VTS_LOW,
			                   &vts);
		if (mode.hts != 0u && vts != 0u)
			out->fps_target_x10 = (uint16_t)((uint64_t)sel.hz * 10u /
			                                 ((uint32_t)mode.hts * vts));
	}
	op_unlock();
	return 0;
}

/* ---- quality settings public API (issue #44) ----------------------------- */

int camera_get_settings(struct camera_settings *out)
{
	int rc;

	if (out == NULL)
		return CAM_ERR_PARAM;
	rc = op_lock();
	if (rc != 0)
		return rc;
	*out = settings;
	op_unlock();
	return 0;
}

/* Shared tail for every level setter: takes the mutex, stashes the validated
   value (already range-checked) and re-applies if the sensor is live.
   @field points into `settings`. */
static int set_level(int8_t *field, int value)
{
	int rc = op_lock();

	if (rc != 0)
		return rc;
	*field = (int8_t)value;
	settings_dirty = 1;
	rc = apply_if_live_locked();
	op_unlock();
	return rc;
}

int camera_set_brightness(int level)
{
	if (level < CAM_LEVEL_MIN || level > CAM_LEVEL_MAX)
		return CAM_ERR_PARAM;
	return set_level(&settings.brightness, level);
}

int camera_set_contrast(int level)
{
	if (level < CAM_LEVEL_MIN || level > CAM_LEVEL_MAX)
		return CAM_ERR_PARAM;
	return set_level(&settings.contrast, level);
}

int camera_set_saturation(int level)
{
	if (level < CAM_LEVEL_MIN || level > CAM_LEVEL_MAX)
		return CAM_ERR_PARAM;
	return set_level(&settings.saturation, level);
}

int camera_set_hue(int degrees)
{
	if (degrees < CAM_HUE_MIN * 30 || degrees > CAM_HUE_MAX * 30 ||
	    (degrees % 30) != 0)
		return CAM_ERR_PARAM;
	return set_level(&settings.hue, degrees / 30);
}

int camera_set_awb(enum camera_awb mode)
{
	int rc;

	if ((unsigned)mode > CAM_AWB_CLOUDY)
		return CAM_ERR_PARAM;
	rc = op_lock();
	if (rc != 0)
		return rc;
	settings.awb = (uint8_t)mode;
	settings_dirty = 1;
	rc = apply_if_live_locked();
	op_unlock();
	return rc;
}

int camera_set_effect(enum camera_effect effect)
{
	int rc;

	if ((unsigned)effect > CAM_FX_GREEN)
		return CAM_ERR_PARAM;
	rc = op_lock();
	if (rc != 0)
		return rc;
	settings.effect = (uint8_t)effect;
	settings_dirty = 1;
	rc = apply_if_live_locked();
	op_unlock();
	return rc;
}

int camera_set_flip(enum camera_flip flip)
{
	int rc;

	if ((unsigned)flip > CAM_FLIP_BOTH)
		return CAM_ERR_PARAM;
	rc = op_lock();
	if (rc != 0)
		return rc;
	settings.flip = (uint8_t)flip;
	settings_dirty = 1;
	rc = apply_if_live_locked();
	op_unlock();
	return rc;
}

int camera_set_zoom(int factor)
{
	int rc;

	if (factor != 1 && factor != 2 && factor != 4 && factor != 8)
		return CAM_ERR_PARAM;
	rc = op_lock();
	if (rc != 0)
		return rc;
	settings.zoom = (uint8_t)factor;
	settings_dirty = 1;
	rc = apply_if_live_locked();
	op_unlock();
	return rc;
}

int camera_set_night(int on)
{
	int rc = op_lock();

	if (rc != 0)
		return rc;
	settings.night = on ? 1u : 0u;
	settings_dirty = 1;
	rc = apply_with_timing_if_live_locked();   /* night changes the AEC ceiling (#70) */
	op_unlock();
	return rc;
}

int camera_set_defaults(void)
{
	int rc = op_lock();

	if (rc != 0)
		return rc;
	settings.brightness = 0;
	settings.contrast   = 0;
	settings.saturation = 0;
	settings.hue        = 0;
	settings.awb        = CAM_AWB_AUTO;
	settings.effect     = CAM_FX_NONE;
	settings.flip       = CAM_FLIP_FLIP;   /* upright for this board's mounting (#68) */
	settings.zoom       = 1;
	settings.night      = 0;
	settings_dirty      = 1;
	rc = apply_with_timing_if_live_locked();   /* night->off must restore ceiling/VTS (#70) */
	op_unlock();
	return rc;
}

/* ---- streaming producer (issue #46) -------------------------------------- */

/* frame_os: the pipeline core's injected mutual exclusion (a TX_MUTEX, distinct
   from the driver cam_lock).  The core is only ever entered from the producer
   thread (and start/stats under cam_lock), never from an ISR. */
static void cam_pipe_os_lock(void *ctx)
{
	(void)ctx;
	(void)tx_mutex_get(&cam_pipe_lock, TX_WAIT_FOREVER);
}

static void cam_pipe_os_unlock(void *ctx)
{
	(void)ctx;
	(void)tx_mutex_put(&cam_pipe_lock);
}

static const struct frame_os cam_pipe_os = {
	NULL, cam_pipe_os_lock, cam_pipe_os_unlock
};

/* Counting sink: the display-independent FPS / throughput consumer.  DROP
   policy and it returns each frame's pin immediately (no holding), so the
   producer is never blocked -- exactly the #46 measurement target.  The core
   keeps delivered/dropped. */
static int cam_stat_open(void *ctx, enum frame_format fmt, uint16_t w, uint16_t h)
{
	(void)ctx;
	(void)fmt;
	(void)w;
	(void)h;
	return 0;   /* counting sink: accepts whatever the producer publishes (#45) */
}

static int cam_stat_consume(void *ctx, const struct frame_desc *f)
{
	(void)ctx;
	frame_pipeline_put(&cam_pipe, &cam_stat_sink, f);
	return 0;
}

/* DMA transfer-complete (ISR context): one ring buffer just filled.  Only wakes
   the producer; it touches no ring / pipeline / CT state (the #47 ISR rule). */
static void cam_stream_dma_cb(DMA_HandleTypeDef *h)
{
	(void)h;
	if (!cam_stream_active)
		return;
	(void)tx_semaphore_put(&cam_stream_sem);
}

/* DMA error callback.  The double-buffer arm (HAL_DMAEx_MultiBufferStart_IT)
   enables the FIFO-error interrupt the snapshot path (HAL_DMA_Start_IT) leaves
   off.  FE (FIFO) and DME (direct-mode) errors are transient under SDRAM
   contention (LTDC continuously reads the framebuffer) and, per RM0385 8.5.5,
   do NOT disable the stream -- the snapshot path simply never sees them.  So
   count and ignore them (cam_stream_fe); only a transfer error (TE), which the
   hardware uses to actually halt the stream, is terminal (#56). */
static void cam_stream_dma_err_cb(DMA_HandleTypeDef *h)
{
	if (!cam_stream_active)
		return;
	if (!(h->ErrorCode & HAL_DMA_ERROR_TE)) {
		cam_stream_fe++;                    /* FE/DME: non-fatal, keep streaming */
		/* HAL clears the FE/DME flags but not h->ErrorCode; clear the tolerated
		   bits ourselves so HAL_DMA_IRQHandler does not re-enter this callback on
		   every following (TC) interrupt while ErrorCode stays nonzero. */
		h->ErrorCode &= ~(HAL_DMA_ERROR_FE | HAL_DMA_ERROR_DME);
		return;
	}
	cam_stream_err = 1;
	(void)tx_semaphore_put(&cam_stream_sem);
}

static void drain_stream_sem(void)
{
	while (tx_semaphore_get(&cam_stream_sem, TX_NO_WAIT) == TX_SUCCESS)
		;
	/* A start kick that landed while the producer was already active lingers on
	   cam_start_sem; clear it so a later idle wait blocks instead of spinning
	   once. */
	while (tx_semaphore_get(&cam_start_sem, TX_NO_WAIT) == TX_SUCCESS)
		;
}

static uint32_t cam_elapsed_ms;   /* frozen at teardown for post-stop `stats` */

/* ---- subscriber registry helpers (all under cam_lock) --------------------- */

/* Find the registry slot for sink @p s, or NULL if not registered.  Passing NULL
   returns the first free slot (used to claim one), or NULL when the table is full. */
static struct cam_sub *cam_sub_find(struct frame_sink *s)
{
	for (int i = 0; i < CAM_MAX_SUBS; i++)
		if (cam_subs[i].sink == s)
			return &cam_subs[i];
	return NULL;
}

/* Claim/find the registry slot for @p s and mark it enabled for pixel format
   @p fmt (no attach).  @p oneshot selects the persistence policy (0 = persistent
   gui/nncam, 1 = non-persistent MJPEG, #101).  Returns the slot, or NULL if the
   table is full.  cam_lock held.  A subscriber's own module owns only this
   `enabled` intent; membership (`attached`) is set later by the attach path.
   `oneshot` is set on EVERY register so a reused slot never inherits a stale
   policy from a previous subscriber. */
static struct cam_sub *cam_sub_register(struct frame_sink *s, enum camera_format fmt,
                                        int oneshot)
{
	struct cam_sub *sub = cam_sub_find(s);

	if (sub == NULL)
		sub = cam_sub_find(NULL);      /* first free slot */
	if (sub == NULL)
		return NULL;
	sub->sink    = s;
	sub->fmt     = (uint8_t)fmt;
	sub->enabled = 1;
	sub->oneshot = oneshot ? 1u : 0u;
	return sub;
}

/* Attach every enabled subscriber whose format class matches the (freshly
   inited) base to cam_pipe.  Rollback: on any attach failure detach the ones
   done this pass and return <0, leaving no partial membership (contract
   #100.8).  On success returns the count of external subscribers attached
   (0 => a plain `camera stream` with no subscriber).  cam_lock held; the base
   was off on entry, so every subscriber starts detached. */
static int cam_subs_attach_all(void)
{
	int n = 0;

	for (int i = 0; i < CAM_MAX_SUBS; i++) {
		struct cam_sub *sub = &cam_subs[i];

		if (sub->sink == NULL || !sub->enabled || sub->attached)
			continue;
		if (sub->fmt != mode.format)
			continue;                     /* format mismatch: stay enabled + idle */
		if (frame_pipeline_attach(&cam_pipe, sub->sink) != 0) {
			/* out of pipeline slots / open() rejected: unwind this pass so the
			   base start fails clean with no half-attached subscriber. */
			for (int k = 0; k < CAM_MAX_SUBS; k++) {
				if (cam_subs[k].attached) {
					(void)frame_pipeline_detach(&cam_pipe, cam_subs[k].sink);
					cam_subs[k].attached = 0;
				}
			}
			return -1;
		}
		sub->attached = 1;
		n++;
	}
	return n;
}

/* Detach every attached subscriber (cascade: each frame_pipeline_detach() calls
   the sink's close()).  Producer-thread teardown and explicit stop both converge
   here.  A subscriber's close() is non-blocking and must not re-enter the camera
   API (contract #100.2), so this is safe with cam_lock held. */
static void cam_subs_detach_all(void)
{
	for (int i = 0; i < CAM_MAX_SUBS; i++) {
		if (cam_subs[i].attached) {
			(void)frame_pipeline_detach(&cam_pipe, cam_subs[i].sink);
			cam_subs[i].attached = 0;
		}
	}
}

/* #101: fully release non-persistent (oneshot, e.g. MJPEG) subscribers -- clear
   the registration so a later base start's cam_subs_attach_all() cannot ghost
   re-attach a subscriber whose owning feature has no consumer anymore.  Called
   under cam_lock on a NON-recover base stop (explicit stop / --frames|--secs
   target done / overrun recover giveup): base off for good, so a oneshot sub must
   go idle in the registry too.  Persistent subs (gui/nncam, oneshot=0) are
   untouched -- they keep their enabled intent and re-attach at the next base start
   (contract #100.1).  The oneshot sub is already detached (attached=0) by
   cam_subs_detach_all before this runs; a MJPEG thread observing this via
   camera_subscribed() then fully stops rather than pausing for a re-open. */
static void cam_subs_release_oneshot(void)
{
	for (int i = 0; i < CAM_MAX_SUBS; i++) {
		if (cam_subs[i].sink != NULL && cam_subs[i].oneshot) {
			cam_subs[i].enabled = 0;
			cam_subs[i].sink    = NULL;
			cam_subs[i].oneshot = 0;
		}
	}
}

/* Stop the stream and restore a clean snapshot-ready DCMI/DMA.  Producer-thread
   only (single owner): start / stop / auto-stop / OVR all converge here, so the
   HW teardown never races the producer's repoint.  Short cam_lock for the state
   transition (serialises with start / stats). */
static void cam_stream_teardown(void)
{
	(void)tx_mutex_get(&cam_lock, TX_WAIT_FOREVER);
	if (cam_stream_active) {
		/* Auto-recover only an overrun/DMA-error stop that was NOT an explicit
		   stop or a --frames/--secs target completion (#100 contract 6). */
		cam_recover_pending = cam_stream_err && !cam_stop_req;
		cam_stream_active = 0;
		cam_elapsed_ms = HAL_GetTick() - cam_start_tick;
		(void)HAL_DCMI_Stop(&hdcmi);          /* aborts the DMA internally */
		/* The JPEG snapshot-loop arms DCMI_IT_FRAME, but HAL_DCMI_Stop does NOT
		   clear interrupt-enables (HAL only disables FRAME inside the FRAME IRQ).
		   A timeout-driven stop (--secs / stop) tears down between FRAMEs with
		   FRAME still armed, so a later raster stream -- which assumes FRAME is
		   disabled -- would take a spurious cam_stream_sem.  Disable it here (#63;
		   a no-op for the raster path that never enabled it). */
		__HAL_DCMI_DISABLE_IT(&hdcmi, DCMI_IT_FRAME);
		frame_pipeline_detach(&cam_pipe, &cam_stat_sink);
		/* Cascade: detach every attached subscriber too (each detach fires the
		   sink's close(), the "base detached" notification -- NOT feature stop),
		   so the next frame_pipeline_init() (a later start) cannot memset a
		   still-linked sink.  Subscriber sinks are synchronous (no cross-thread
		   pin) and their close() is non-blocking, so a producer-thread detach is
		   safe here.  This is the master-switch teardown for both an explicit
		   stop and an async OVR/auto-stop (contract #100.2). */
		cam_subs_detach_all();
		/* #101: a non-recover stop (explicit / target done) leaves the base off
		   for good -> fully release oneshot (MJPEG) subscribers so a later start
		   cannot ghost re-attach them.  An overrun (cam_recover_pending) keeps them
		   enabled: cam_stream_recover() re-attaches them at the same mode. */
		if (!cam_recover_pending)
			cam_subs_release_oneshot();
		drain_stream_sem();
		/* HAL_DCMI_Stop leaves CR.DBM set on the stream; re-init the DMA so the
		   next snapshot's HAL_DCMI_Start_DMA runs a plain single transfer. */
		(void)HAL_DMA_DeInit(&hdma_dcmi);
		hdma_dcmi.Init.Mode = DMA_NORMAL;
		(void)HAL_DMA_Init(&hdma_dcmi);
		__HAL_LINKDMA(&hdcmi, DMA_Handle, hdma_dcmi);
		LOG_INF("stream stop (%lu frames, ovr dcmi=%lu ring=%lu fe=%lu)",
		        (unsigned long)cam_pipe.stats.published,
		        (unsigned long)cam_stream_ovr, (unsigned long)cam_ring_ovr,
		        (unsigned long)cam_stream_fe);
	}
	(void)tx_mutex_put(&cam_lock);
}

/* Service one completed frame: find the completed buffer via CT, secure a free
   slot, repoint the completed M-register away, THEN publish.  This ordering
   keeps a published slot off the live DMA target list, so a sink never reads a
   buffer the DMA is about to overwrite (tear-free).  No free slot -> drop. */
static void cam_stream_service(int had_sem)
{
	uint32_t ct, seen, extra = 0;
	struct frame_desc *done, *freed;
	int published = 0;

	/* Stop conditions (also handles the bounded-timeout wake with no frame). */
	if (cam_stop_req || cam_stream_err ||
	    (cam_target_frames && cam_pipe.stats.published >= cam_target_frames) ||
	    (cam_target_secs &&
	     (HAL_GetTick() - cam_start_tick) >= cam_target_secs * 1000u)) {
		cam_stream_teardown();
		return;
	}

	/* Completions observed this pass (one semaphore each): the wake plus any
	   drained extras.  We publish at most the most-recent buffer flip; every
	   other observed completion was overwritten -> ring overrun. */
	while (tx_semaphore_get(&cam_stream_sem, TX_NO_WAIT) == TX_SUCCESS)
		extra++;
	seen = (had_sem ? 1u : 0u) + extra;

	ct = (DMA2_Stream1->CR & DMA_SxCR_CT) ? 1u : 0u;
	if (ct != cam_last_ct) {
		cam_last_ct = ct;
		/* CT==1 -> M0 just completed; CT==0 -> M1 just completed (the HAL
		   HAL_DMA_IRQHandler DBM dispatch mirrors this). */
		done = ct ? cam_m0 : cam_m1;
		freed = frame_pipeline_acquire(&cam_pipe);
		if (freed != NULL) {
			if (ct) {
				(void)HAL_DMAEx_ChangeMemory(&hdma_dcmi,
				        (uint32_t)freed->data, MEMORY0);
				cam_m0 = freed;
			} else {
				(void)HAL_DMAEx_ChangeMemory(&hdma_dcmi,
				        (uint32_t)freed->data, MEMORY1);
				cam_m1 = freed;
			}
			frame_pipeline_publish(&cam_pipe, done, mode.frame_bytes,
			                       fmt_to_frame(mode.format), mode.width,
			                       mode.height,
			                       (uint16_t)(mode.width *
			                                  mode.bytes_per_pixel));
			published = 1;
		}
		/* freed == NULL: no free slot, drop (do not publish); the M-reg keeps
		   pointing at `done`, so the DMA simply refills it next cycle. */
	}
	if (seen > (uint32_t)published)
		cam_ring_ovr += seen - (uint32_t)published;
}

/* ---- JPEG variable-length streaming (issue #63) -------------------------- */
/*
 * JPEG cannot ride the raster DBM/TC path: a JPEG frame is shorter than the DMA
 * budget, so the transfer-complete interrupt never fires at the frame boundary.
 * Instead each frame is a single DCMI SNAPSHOT (CM=1) into one ring slot, and the
 * DCMI FRAME interrupt (the same end-of-frame the snapshot path uses) delimits it.
 * After each frame the DCMI stops capturing on its own, so re-arming into the next
 * slot cannot overrun the FIFO mid-gap (a CM=0 continuous re-point would).  The
 * producer thread owns all of Stop / finalize / publish / re-arm; the ISR only
 * posts cam_stream_sem.  Frames that fall in the Stop..re-arm gap are dropped.
 */

/* Arm one JPEG snapshot DMA into @p slot and enable the FRAME interrupt (the
   variable-length boundary).  Mirrors the snapshot arm sequence; returns the HAL
   status so the caller can treat a failed (re-)arm as terminal. */
static HAL_StatusTypeDef cam_jpeg_arm(struct frame_desc *slot)
{
	HAL_StatusTypeDef st;

	__HAL_DCMI_CLEAR_FLAG(&hdcmi, DCMI_FLAG_ERRRI | DCMI_FLAG_OVRRI |
	                              DCMI_FLAG_FRAMERI | DCMI_FLAG_LINERI |
	                              DCMI_FLAG_VSYNCRI);
	__HAL_DCMI_ENABLE_IT(&hdcmi, DCMI_IT_ERR | DCMI_IT_OVR | DCMI_IT_FRAME);
	st = HAL_DCMI_Start_DMA(&hdcmi, DCMI_MODE_SNAPSHOT, (uint32_t)slot->data,
	                        CAM_JPEG_BUDGET_WORDS);
	if (st != HAL_OK)
		return st;
	/* The banding path enables the DMA FIFO/direct-mode error interrupts; under
	   SDRAM contention HAL's DCMI_DMAError would abort the capture, so silence the
	   non-fatal FE/DME the way the snapshot path does (#45 R8). */
	__HAL_DMA_DISABLE_IT(&hdma_dcmi, DMA_IT_FE);
	__HAL_DMA_DISABLE_IT(&hdma_dcmi, DMA_IT_DME);
	return HAL_OK;
}

/* Stop conditions shared by both stream services. */
static int cam_stream_should_stop(void)
{
	return cam_stop_req || cam_stream_err ||
	       (cam_target_frames && cam_pipe.stats.published >= cam_target_frames) ||
	       (cam_target_secs &&
	        (HAL_GetTick() - cam_start_tick) >= cam_target_secs * 1000u);
}

/* Service one JPEG frame: finalise the current slot (Stop -> NDTR -> EOI trim),
   publish it at its real length, then re-arm into a free slot.  save/send pull the
   latest published frame on demand (camera_snapshot_latest, #102), so this hot path
   does no cam_frame mirror.  Every HAL failure (Stop / re-arm) is terminal ->
   teardown, so the stream never sticks "active" with no frame coming.
   Producer-thread only. */
static void cam_stream_service_jpeg(int had_sem)
{
	uint32_t ndtr, eff, valid;
	struct frame_desc *freed;

	/* Stop conditions FIRST (stop/error/target share cam_stream_sem with FRAME). */
	if (cam_stream_should_stop()) {
		cam_stream_teardown();
		return;
	}
	if (!had_sem)
		return;                          /* bounded-timeout wake, no frame yet */

	/* FRAME arrived: the slot is no longer a live DMA target once stopped. */
	if (HAL_DCMI_Stop(&hdcmi) != HAL_OK) {
		cam_stream_err = 1;
		cam_stream_teardown();
		return;
	}
	ndtr = __HAL_DMA_GET_COUNTER(&hdma_dcmi);
	eff  = (CAM_JPEG_BUDGET_WORDS - ndtr) * 4u;
	if (jpeg_trim(cam_jpeg_slot->data, eff, &valid) == 0) {
		freed = frame_pipeline_acquire(&cam_pipe);
		if (freed != NULL) {
			frame_pipeline_publish(&cam_pipe, cam_jpeg_slot, valid,
			                       FRAME_FMT_JPEG, mode.width, mode.height, 0u);
			/* #102: the producer no longer mirrors into cam_frame.  camera
			   save/send pull the latest published frame on demand via
			   camera_snapshot_latest() (frame_pipeline_pin_latest), so this hot
			   path stays copy-free for every format -- raster and JPEG alike. */
			cam_jpeg_slot = freed;       /* fill a fresh slot next */
		} else {
			cam_ring_ovr++;              /* no free slot: drop, reuse this slot */
		}
	} else {
		cam_jpeg_trunc++;                /* no SOI/EOI: drop, reuse this slot */
	}

	/* Re-evaluate stop after publishing so a --frames/--secs target does not arm
	   one extra capture. */
	if (cam_stream_should_stop()) {
		cam_stream_teardown();
		return;
	}
	if (cam_jpeg_arm(cam_jpeg_slot) != HAL_OK) {
		cam_stream_err = 1;
		cam_stream_teardown();
	}
}

static int stream_start_locked(int colorbar, uint32_t frames, uint32_t secs);

/* Re-arm the base after an overrun teardown (producer thread; entered when
   cam_recover_pending is set and no stream is active).  Escalating backoff breaks
   a persistent overrun loop.  Re-takes cam_lock like a public start, and aborts
   if an explicit stop/power-off cleared the pending flag (or a start re-armed the
   base) while we were between the teardown and here. */
static void cam_stream_recover(void)
{
	uint32_t now = HAL_GetTick();
	int rc;

	(void)tx_mutex_get(&cam_lock, TX_WAIT_FOREVER);
	if (!cam_recover_pending || cam_stream_active) {
		cam_recover_pending = 0;
		/* #101: recovery cancelled (explicit stop/start raced in) and the base is
		   off -> release oneshot subs so they cannot ghost re-attach.  Defensive:
		   the cancelling entry point already released them; idempotent here. */
		if (!cam_stream_active)
			cam_subs_release_oneshot();
		(void)tx_mutex_put(&cam_lock);
		return;                       /* explicit stop/start raced in: give up */
	}
	cam_recover_pending = 0;

	if (now - cam_recover_last < CAM_RECOVER_WINDOW)
		cam_recover_rapid++;
	else
		cam_recover_rapid = 0;
	cam_recover_last = now;

	if (cam_recover_rapid >= CAM_RECOVER_GIVEUP) {
		/* #101: auto-recovery abandoned -> base stays off, so oneshot (MJPEG)
		   subscribers must go idle too (a paused MJPEG thread sees this via
		   camera_subscribed() and fully stops instead of waiting forever). */
		cam_subs_release_oneshot();
		(void)tx_mutex_put(&cam_lock);
		LOG_WRN("base overruns persist; auto-recovery stopped -- use "
		        "'camera stream start'");
		return;
	}

	/* Re-arm at the same mode; cam_subs_attach_all() re-attaches every enabled
	   subscriber (each sees a close()/open() pair). */
	rc = stream_start_locked(cam_start_colorbar, cam_target_frames, cam_target_secs);
	if (rc != 0)
		cam_subs_release_oneshot();    /* #101: re-arm failed, base off -> release */
	(void)tx_mutex_put(&cam_lock);
	if (rc != 0)
		LOG_WRN("base auto-recovery failed (%d) -- use 'camera stream start'", rc);
	else
		LOG_INF("base capture auto-recovered after overrun");
}

/* Dedicated producer (priority 10).  When no stream is running it sleeps
   indefinitely (0 CPU) on the start semaphore; while active it bounded-waits on
   the completion semaphore so --secs / stop / OVR fire even if frames stop
   arriving.  The two signals are SEPARATE semaphores on purpose: the start
   wakeup must never be consumed by the active bounded wait (it would be
   miscounted as a frame completion -> a spurious ring overrun). */
static void cam_producer_entry(ULONG arg)
{
	(void)arg;
	for (;;) {
		if (!cam_stream_active) {
			if (cam_recover_pending) {     /* overrun: re-arm the base (#100) */
				cam_stream_recover();
				continue;
			}
			(void)tx_semaphore_get(&cam_start_sem, TX_WAIT_FOREVER);
			continue;
		}
		UINT got = tx_semaphore_get(&cam_stream_sem, CAM_PRODUCER_TICK);

		if (mode.is_jpeg)
			cam_stream_service_jpeg(got == TX_SUCCESS);
		else
			cam_stream_service(got == TX_SUCCESS);
	}
}

/* Start the base capture with the ring/pipeline built fresh, attaching the
   counting sink plus every enabled + format-compatible subscriber (cam_subs[]).
   cam_lock MUST be held; returns 0 or CAM_ERR_* and NEVER unlocks (the public
   wrappers own the lock).  A subscriber enabled *after* the base is running
   attaches immediately in camera_subscribe(), not here. */
static int stream_start_locked(int colorbar, uint32_t frames, uint32_t secs)
{
	int rc, nx;
	uint32_t slot_size, nslots;

	if (cam_stream_active)
		return CAM_ERR_BUSY;           /* already streaming */
	if (!sdram_is_up())
		return CAM_ERR_STATE;
	if (!info.powered) {
		rc = camera_probe_locked(NULL);
		if (rc != 0)
			return rc;
	}
	/* A streamed frame must fit one DMA NDTR (frame_words <= 65535); bigger modes
	   and JPEG are snapshot-only (mode.streamable is 0). */
	if (!mode.streamable && !mode.is_jpeg)
		return CAM_ERR_STATE;          /* large raster modes are capture-only */

	/* Partition the camera arena (cam_arena, bank1) into ring slots sized to the
	   current mode: slot stride = align32(frame_bytes), as many slots as fit,
	   capped at FRAME_PIPELINE_MAX_SLOTS -- so small modes get a deeper ring
	   (#65).  REJECT (not clamp) when fewer than 2 slots fit: 0/1 slot cannot run
	   the DBM pair, and clamping up to 2 would overflow the arena since
	   frame_pipeline_publish() does not validate bytes <= slot_size (#45). */
	slot_size = (mode.frame_bytes + 31u) & ~31u;
	if (slot_size == 0u || CAM_ARENA_BYTES / slot_size < 2u)
		return CAM_ERR_STATE;
	nslots = CAM_ARENA_BYTES / slot_size;
	if (nslots > FRAME_PIPELINE_MAX_SLOTS)
		nslots = FRAME_PIPELINE_MAX_SLOTS;

	rc = camera_configure_locked(colorbar != 0);
	if (rc != 0)
		return rc;

	/* Program the effective PCLK for the fps knob + current scanout state (#67):
	   30 fps (48 MHz) only takes effect here when the LTDC is not scanning out, so
	   a plain `camera stream start` after `lcd off` runs at 30 fps while one
	   started with the display on (or a GUIX preview) clamps to 15 fps. */
	rc = apply_effective_pclk_locked();
	if (rc != 0)
		return rc;

	/* Build the ring + pipeline fresh over the arena and attach the counting sink
	   (+ ext).  slot_size is the ring slot STRIDE (align32(frame_bytes)), not the
	   frame length -- the DMA writes mode.frame_bytes into each slot's prefix and
	   publish() stamps that as the valid length. */
	if (frame_pipeline_init(&cam_pipe, &cam_pipe_os, cam_arena,
	                        slot_size, nslots) != 0)
		return CAM_ERR_STATE;
	frame_pipeline_set_format(&cam_pipe, fmt_to_frame(mode.format),
	                          mode.width, mode.height);
	if (frame_pipeline_attach(&cam_pipe, &cam_stat_sink) != 0)
		return CAM_ERR_STATE;
	nx = cam_subs_attach_all();       /* enabled + compatible subscribers */
	if (nx < 0) {
		frame_pipeline_detach(&cam_pipe, &cam_stat_sink);
		return CAM_ERR_STATE;
	}
	(void)nx;                         /* #102: no longer used for a mirror latch */
	cam_stream_ovr    = 0;
	cam_ring_ovr      = 0;
	cam_stream_fe     = 0;
	cam_ring_slots    = nslots;       /* #65: observable arena partition */
	cam_ring_slot_bytes = slot_size;
	cam_jpeg_trunc    = 0;
	cam_stream_err    = 0;
	cam_stop_req      = 0;
	cam_last_ct       = 0;
	cam_elapsed_ms    = 0;
	cam_start_tick    = HAL_GetTick();
	cam_target_frames = frames;
	cam_target_secs   = secs;
	cam_start_colorbar = colorbar;    /* remembered for an overrun auto-recover */
	drain_stream_sem();

	if (mode.is_jpeg) {
		/* JPEG snapshot-loop (#63): one ring slot is the live DMA target and the
		   DCMI FRAME ISR delimits each variable-length frame (no DBM, no TC).  On
		   any failure path cascade-detach every subscriber attached above (the
		   MJPEG eth_sink is the only JPEG-class subscriber, #49 P5). */
		cam_jpeg_slot = frame_pipeline_acquire(&cam_pipe);
		if (cam_jpeg_slot == NULL) {
			cam_subs_detach_all();
			frame_pipeline_detach(&cam_pipe, &cam_stat_sink);
			return CAM_ERR_STATE;
		}
		cam_stream_active = 1;             /* arm the ISR + producer */
		if (cam_jpeg_arm(cam_jpeg_slot) != HAL_OK) {
			/* cam_lock is held, so cam_stream_teardown() (which re-takes it) must
			   not run here -- roll back by hand like the raster DMA-start failure. */
			cam_stream_active = 0;
			(void)HAL_DCMI_Stop(&hdcmi);
			__HAL_DCMI_DISABLE_IT(&hdcmi, DCMI_IT_FRAME);  /* cam_jpeg_arm armed it */
			(void)HAL_DMA_DeInit(&hdma_dcmi);
			hdma_dcmi.Init.Mode = DMA_NORMAL;
			(void)HAL_DMA_Init(&hdma_dcmi);
			__HAL_LINKDMA(&hdcmi, DMA_Handle, hdma_dcmi);
			cam_subs_detach_all();
			frame_pipeline_detach(&cam_pipe, &cam_stat_sink);
			drain_stream_sem();
			LOG_ERR("JPEG stream arm failed");
			return CAM_ERR_HAL;
		}
		(void)tx_semaphore_put(&cam_start_sem);
		LOG_INF("stream start jpeg (frames=%lu secs=%lu, subs=%d)",
		        (unsigned long)frames, (unsigned long)secs, nx);
		return 0;
	}

	cam_m0 = frame_pipeline_acquire(&cam_pipe);
	cam_m1 = frame_pipeline_acquire(&cam_pipe);
	if (cam_m0 == NULL || cam_m1 == NULL) {
		cam_subs_detach_all();
		frame_pipeline_detach(&cam_pipe, &cam_stat_sink);
		return CAM_ERR_STATE;
	}

	/* Manual DBM start (the HAL's own >64KB path is intra-frame banding, not
	   inter-frame double buffering).  Order: callbacks -> DMA DBM start ->
	   DCMI CM=0 (continuous, before enable) -> enable -> ERR/OVR IT -> CAPTURE. */
	hdma_dcmi.XferCpltCallback   = cam_stream_dma_cb;
	hdma_dcmi.XferM1CpltCallback = cam_stream_dma_cb;
	hdma_dcmi.XferErrorCallback  = cam_stream_dma_err_cb;
	__HAL_DCMI_CLEAR_FLAG(&hdcmi, DCMI_FLAG_ERRRI | DCMI_FLAG_OVRRI |
	                              DCMI_FLAG_FRAMERI | DCMI_FLAG_LINERI |
	                              DCMI_FLAG_VSYNCRI);

	cam_stream_active = 1;             /* arm the ISR + producer */
	if (HAL_DMAEx_MultiBufferStart_IT(&hdma_dcmi,
	        (uint32_t)&hdcmi.Instance->DR,
	        (uint32_t)cam_m0->data, (uint32_t)cam_m1->data,
	        mode.frame_words) != HAL_OK) {
		cam_stream_active = 0;
		cam_subs_detach_all();
		frame_pipeline_detach(&cam_pipe, &cam_stat_sink);
		LOG_ERR("stream DMA start failed");
		return CAM_ERR_HAL;
	}
	hdcmi.Instance->CR &= ~DCMI_CR_CM;            /* continuous */
	__HAL_DCMI_ENABLE(&hdcmi);
	__HAL_DCMI_ENABLE_IT(&hdcmi, DCMI_IT_ERR | DCMI_IT_OVR);
	hdcmi.Instance->CR |= DCMI_CR_CAPTURE;
	hdcmi.State = HAL_DCMI_STATE_BUSY;

	/* Wake the producer out of its idle FOREVER wait on the dedicated start
	   semaphore -- never the completion semaphore, so it cannot be miscounted. */
	(void)tx_semaphore_put(&cam_start_sem);
	LOG_INF("stream start (frames=%lu secs=%lu, subs=%d)",
	        (unsigned long)frames, (unsigned long)secs, nx);
	return 0;
}

int camera_stream_start(int colorbar, uint32_t frames, uint32_t secs)
{
	int rc = op_lock();

	if (rc != 0)
		return rc;
	/* Base capture start (#100): allowed unless already streaming.  Subscribers
	   attach inside stream_start_locked(); there is no single owner to refuse.
	   An explicit start resets the overrun-recovery backoff. */
	cam_recover_pending = 0;
	cam_recover_rapid   = 0;
	/* #101: a fresh explicit start supersedes any aborted overrun-recovery window --
	   drop orphaned oneshot (MJPEG) subs left enabled+detached so they cannot ghost
	   re-attach to this new base (possibly a different format).  Only when the base
	   is off: an active base keeps its attached oneshot and the start below
	   BUSY-fails without touching state. */
	if (!cam_stream_active)
		cam_subs_release_oneshot();
	rc = stream_start_locked(colorbar, frames, secs);
	op_unlock();
	return rc;
}

int camera_subscribe(struct frame_sink *s, enum camera_format fmt)
{
	struct cam_sub *sub;
	int rc;

	if (s == NULL)
		return CAM_ERR_PARAM;
	rc = op_lock();
	if (rc != 0)
		return rc;
	sub = cam_sub_register(s, fmt, 0);        /* persistent (gui / nncam) */
	if (sub == NULL) {
		op_unlock();
		return CAM_ERR_STATE;             /* registry full */
	}
	rc = 0;
	/* Attach now iff the base is already running and its format matches; otherwise
	   stay enabled + idle and attach at the next base start (contract #100.1: the
	   subscriber's enabled intent is orthogonal to base on/off).  A failed attach
	   (pipeline full / open() reject) leaves no registration. */
	if (cam_stream_active && !sub->attached && sub->fmt == mode.format) {
		if (frame_pipeline_attach(&cam_pipe, s) == 0) {
			sub->attached = 1;
		} else {
			sub->sink = NULL;
			sub->enabled = 0;
			sub->oneshot = 0;
			rc = CAM_ERR_BUSY;
		}
	}
	op_unlock();
	return rc;
}

int camera_subscribe_oneshot(struct frame_sink *s, enum camera_format fmt)
{
	struct cam_sub *sub;
	int rc;

	if (s == NULL)
		return CAM_ERR_PARAM;
	rc = op_lock();
	if (rc != 0)
		return rc;
	/* #101: STRICT attach.  Unlike camera_subscribe() (which registers enabled+idle
	   when the base is off / format-mismatched), a oneshot (MJPEG) subscriber must
	   bind to a LIVE, format-matching base under THIS cam_lock -- there is no
	   enabled-idle registration.  This makes the base-state check and the attach
	   atomic, closing the check-then-subscribe race: a base that stops between a
	   caller's pre-check and here cannot leave a ghost registration that a later
	   start would re-attach.  A oneshot only ever becomes enabled+detached via an
	   overrun teardown (which keeps it for auto-recovery), never via this path. */
	if (!cam_stream_active) {
		op_unlock();
		return CAM_ERR_STATE;             /* base not streaming */
	}
	if (mode.format != fmt) {
		op_unlock();
		return CAM_ERR_PARAM;             /* base format class mismatch (raster vs JPEG) */
	}
	sub = cam_sub_register(s, fmt, 1);
	if (sub == NULL) {
		op_unlock();
		return CAM_ERR_STATE;             /* registry full */
	}
	if (sub->attached) {                  /* idempotent re-subscribe */
		op_unlock();
		return 0;
	}
	if (frame_pipeline_attach(&cam_pipe, s) != 0) {
		sub->sink = NULL;
		sub->enabled = 0;
		sub->oneshot = 0;
		op_unlock();
		return CAM_ERR_BUSY;              /* pipeline full / open() rejected */
	}
	sub->attached = 1;
	op_unlock();
	return 0;
}

int camera_unsubscribe(struct frame_sink *s)
{
	struct cam_sub *sub;
	int inflight = 0;

	if (s == NULL || op_lock() != 0)
		return 0;
	sub = cam_sub_find(s);
	if (sub != NULL) {
		if (sub->attached) {
			/* Detach (no more new consume; close() fires) and report the pins the
			   sink still holds so the caller can drain its own thread.  The base
			   keeps running: unsubscribing never stops it (contract #100.2). */
			inflight = frame_pipeline_detach(&cam_pipe, s);
			sub->attached = 0;
		}
		sub->enabled = 0;
		sub->oneshot = 0;
		sub->sink    = NULL;              /* free the registry slot */
	}
	op_unlock();
	return inflight;
}

int camera_subscribed(struct frame_sink *s)
{
	struct cam_sub *sub;
	int enabled = 0;

	/* #101: single source of truth for "is @p s still a registered subscriber".
	   A oneshot (MJPEG) sink polls this after a base teardown (its close() only set
	   a flag): still enabled => an overrun recovery is in flight, keep the HTTP
	   session paused for the re-open; not enabled => a non-recover stop released it
	   (cam_subs_release_oneshot), so fully stop.  Snapshot under cam_lock so it
	   never observes a half-updated registry. */
	if (s == NULL || op_lock() != 0)
		return 0;
	sub = cam_sub_find(s);
	enabled = (sub != NULL && sub->enabled);
	op_unlock();
	return enabled;
}

int camera_other_subscribers_attached(struct frame_sink *self)
{
	int other = 0;

	/* #101: true if any attached external subscriber OTHER THAN @p self is live.
	   The GUI resolution button uses this to only reconfigure the base (stop ->
	   camera res -> restart, which cascades every subscriber) when it is the sole
	   attached subscriber.  Best-effort snapshot under cam_lock: a concurrent
	   attach after this returns is a benign user race (both are manual actions). */
	if (op_lock() != 0)
		return 0;
	for (int i = 0; i < CAM_MAX_SUBS; i++) {
		if (cam_subs[i].attached && cam_subs[i].sink != self) {
			other = 1;
			break;
		}
	}
	op_unlock();
	return other;
}

void camera_frame_put(struct frame_sink *s, const struct frame_desc *f)
{
	frame_pipeline_put(&cam_pipe, s, f);
}

int camera_stream_stop(void)
{
	int rc = op_lock();

	if (rc != 0)
		return rc;
	/* Master switch (#100): `camera stream stop` cascades -- the producer teardown
	   detaches every subscriber (each close()) and stops the DCMI.  Never refused
	   for having subscribers; auto-stop is intentionally absent (an idle base with
	   no subscribers stays ON until an explicit stop). */
	cam_recover_pending = 0;              /* explicit stop cancels any auto-recover */
	if (cam_stream_active) {
		cam_stop_req = 1;
		(void)tx_semaphore_put(&cam_stream_sem);  /* producer tears down (releases oneshot) */
	} else {
		/* #101: base already inactive -- an overrun tore it down and this stop
		   cancels the pending auto-recovery.  The producer teardown that would
		   normally release oneshot (MJPEG) subs will not run again, so release them
		   here (idempotent if none registered). */
		cam_subs_release_oneshot();
	}
	op_unlock();
	return 0;
}

int camera_stream_stats(struct camera_stream_info *out)
{
	int rc;

	if (out == NULL)
		return CAM_ERR_PARAM;
	rc = op_lock();
	if (rc != 0)
		return rc;
	/* active / err / elapsed transition under cam_lock; the DCMI/ring overrun
	   counters are volatile (atomic 32-bit reads). */
	out->active     = cam_stream_active;
	out->err        = cam_stream_err;
	out->dcmi_ovr   = cam_stream_ovr;
	out->ring_ovr   = cam_ring_ovr;
	out->dma_fe     = cam_stream_fe;
	out->jpeg_trunc = cam_jpeg_trunc;
	out->slots      = cam_ring_slots;
	out->slot_bytes = cam_ring_slot_bytes;
	out->elapsed_ms = cam_stream_active ? (HAL_GetTick() - cam_start_tick)
	                                    : cam_elapsed_ms;
	/* Coherent snapshot of the pipeline-owned counters (the producer writes them
	   under cam_pipe_lock).  Lock order is always cam_lock -> cam_pipe_lock
	   (matching start / teardown), so this nesting cannot deadlock. */
	(void)tx_mutex_get(&cam_pipe_lock, TX_WAIT_FOREVER);
	out->captured    = cam_pipe.stats.captured;    /* producer */
	out->frames      = cam_pipe.stats.published;    /* producer */
	out->delivered   = cam_stat_sink.delivered;     /* stat sink */
	out->dropped     = cam_stat_sink.dropped;       /* stat sink */
	out->stat_errors = cam_stat_sink.errors;        /* stat sink (#102) */
	(void)tx_mutex_put(&cam_pipe_lock);
	op_unlock();
	return 0;
}

int camera_subscribers_snapshot(struct camera_sub_stat *out, int max)
{
	int n = 0;

	if (out == NULL || max <= 0)
		return 0;
	if (op_lock() != 0)
		return 0;
	/* Consistent read of the per-sink counters (producer writes them under
	   cam_pipe_lock).  Lock order cam_lock -> cam_pipe_lock (as elsewhere). */
	(void)tx_mutex_get(&cam_pipe_lock, TX_WAIT_FOREVER);
	/* The internal stats sink is attached for the whole base lifetime (#46). */
	if (cam_stream_active && n < max) {
		out[n].name      = cam_stat_sink.name;
		out[n].fmt       = mode.format;
		out[n].enabled   = 1;
		out[n].attached  = 1;
		out[n].oneshot   = 0;
		out[n].delivered = cam_stat_sink.delivered;
		out[n].dropped   = cam_stat_sink.dropped;
		out[n].errors    = cam_stat_sink.errors;
		n++;
	}
	for (int i = 0; i < CAM_MAX_SUBS && n < max; i++) {
		const struct cam_sub *sub = &cam_subs[i];

		if (sub->sink == NULL)
			continue;
		out[n].name      = sub->sink->name;
		out[n].fmt       = sub->fmt;
		out[n].enabled   = sub->enabled;
		out[n].attached  = sub->attached;
		out[n].oneshot   = sub->oneshot;
		out[n].delivered = sub->sink->delivered;
		out[n].dropped   = sub->sink->dropped;
		out[n].errors    = sub->sink->errors;
		n++;
	}
	(void)tx_mutex_put(&cam_pipe_lock);
	op_unlock();
	return n;
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
	/* Streaming (issue #46): the pipeline's frame_os mutex and the DMA-TC ->
	   producer wakeup semaphore.  The producer thread is created at the end. */
	if (tx_mutex_create(&cam_pipe_lock, "campipe", TX_INHERIT) != TX_SUCCESS) {
		tx_semaphore_delete(&cam_done);
		tx_mutex_delete(&cam_lock);
		return CAM_ERR_STATE;
	}
	if (tx_semaphore_create(&cam_stream_sem, "cam_strm", 0) != TX_SUCCESS) {
		tx_mutex_delete(&cam_pipe_lock);
		tx_semaphore_delete(&cam_done);
		tx_mutex_delete(&cam_lock);
		return CAM_ERR_STATE;
	}
	if (tx_semaphore_create(&cam_start_sem, "cam_strt", 0) != TX_SUCCESS) {
		tx_semaphore_delete(&cam_stream_sem);
		tx_mutex_delete(&cam_pipe_lock);
		tx_semaphore_delete(&cam_done);
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
	   of the one DR register.  NB: streaming arms this in double-buffer mode,
	   which (unlike the snapshot HAL_DMA_Start_IT) enables the DMA FIFO-error
	   interrupt; FE/DME are non-fatal (RM0385 8.5.5 -- they do not disable the
	   stream) and transient under SDRAM contention, so cam_stream_dma_err_cb
	   treats only a transfer error (TE) as terminal (#56). */
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

	/* The streaming counting sink (issue #46) -- format-validated at attach. */
	cam_stat_sink.name    = "stats";
	cam_stat_sink.ctx     = NULL;
	cam_stat_sink.policy  = FRAME_POLICY_DROP;
	cam_stat_sink.open    = cam_stat_open;
	cam_stat_sink.consume = cam_stat_consume;
	cam_stat_sink.close   = NULL;

	/* Dedicated producer thread: idles until a stream starts. */
	if (tx_thread_create(&cam_producer, "cam_prod", cam_producer_entry, 0,
	                     cam_producer_stack, sizeof cam_producer_stack,
	                     CAM_PRODUCER_PRIO, CAM_PRODUCER_PRIO,
	                     TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS) {
		(void)HAL_DCMI_DeInit(&hdcmi);
		(void)HAL_DMA_DeInit(&hdma_dcmi);
		LOG_ERR("camera producer thread create failed");
		goto fail_i2c;
	}

	mode_recompute(&mode);    /* fill geometry/derived fields for the default */

	cam_ready = 1;
	LOG_INF("I2C1 + DCMI/DMA2-S1 up; sensor I/O is lazy");
	return 0;

fail_i2c:
	(void)HAL_I2C_DeInit(&hcam_i2c);
fail:
	tx_semaphore_delete(&cam_start_sem);
	tx_semaphore_delete(&cam_stream_sem);
	tx_mutex_delete(&cam_pipe_lock);
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
	/* JPEG streaming (#63) delimits each variable-length frame by FRAME (the
	   raster stream leaves FRAME disabled, so this only fires for a JPEG stream);
	   wake the producer to finalise + re-arm.  Mode-exclusive with the snapshot
	   gate below. */
	if (cam_stream_active) {
		(void)tx_semaphore_put(&cam_stream_sem);
		return;
	}
	if (!cam_xfer_active)
		return;
	(void)tx_semaphore_put(&cam_done);
}

/* Sync error, overrun and DMA errors all funnel here via the HAL. */
void HAL_DCMI_ErrorCallback(DCMI_HandleTypeDef *h)
{
	(void)h;
	/* Streaming (issue #46): a continuous-mode DCMI overrun is terminal -- the
	   HAL DCMI IRQ has already aborted the DMA -- so flag it and wake the
	   producer to tear down cleanly.  (Mode-exclusive with the snapshot gate.) */
	if (cam_stream_active) {
		cam_stream_err = 1;
		cam_stream_ovr++;
		(void)tx_semaphore_put(&cam_stream_sem);
		return;
	}
	if (!cam_xfer_active)
		return;
	cam_xfer_err = 1;
	(void)tx_semaphore_put(&cam_done);
}
