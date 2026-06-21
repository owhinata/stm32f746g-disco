# Camera (B-CAMS-OMV / OV5640)

ST's camera module bundle **B-CAMS-OMV** (MB1683 adapter + MB1379 camera module, **OV5640** 5MP sensor) connects to the board's **P1** (30-pin ZIF) connector. The driver lives in `port/camera/camera.{c,h}`, the shell command is `camera` (`shell/cmds/cmd_camera.c`).

Camera epic (#22): Phase 1 (#39) = sensor detection (PWR_EN + I2C1/SCCB chip ID); Phase 2 (#41) = **DCMI + DMA2 single-frame QVGA RGB565 snapshot into SDRAM** (`camera capture`); Phase 3 (#42) = **raw frame export + PNG conversion on the PC** (`camera save` + `scripts/rgb565_to_png.py`).

## Wiring (P1 ↔ B-CAMS-OMV CN5)

The board's P1 (UM1907 §7.2 Table 5) and the B-CAMS-OMV 30-pin ZIF CN5 (UM2779 Table 7) match **1:1 for every signal** over the bundled FFC (the two connectors number their pins in opposite order).

| Signal | MCU pin | Notes |
|--------|---------|-------|
| I2C1_SCL (SCCB) | PB8 | AF4, open-drain. Bus shared with the CN2 extension connector |
| I2C1_SDA (SCCB) | PB9 | AF4, open-drain |
| DCMI_PWR_EN | PH13 | **Low = powered** (the module's POWER_DOWN, active-high) |
| DCMI_NRST | -    | tied to the board **NRST net** (no GPIO control) |
| DCMI_HSYNC | PA4 | AF13 |
| DCMI_PIXCLK | PA6 | AF13 |
| DCMI_VSYNC | PG9 | AF13 |
| DCMI D0-D4 | PH9-PH12, PH14 | AF13 |
| DCMI D5 / D6 / D7 | PD3 / PE5 / PE6 | AF13 |

No conflict with the existing peripherals (USART1=PA9/PB7, QSPI=PB2/PB6/PD11-13/PE2, SDMMC1=PC8-13/PD2, LED=PI1).

!!! note "Clocking (XCLK)"
    The MB1379 module clocks the OV5640 from its **own 24 MHz crystal** (MB1379/X1, UM2779 §3.2). The host supplies **no MCO**. P1 pin 21 (Camera_CLK, OSC_24M from the board's X1) is wired but unused.

## I2C (SCCB)

| Item | Value | Source |
|------|-------|--------|
| Instance | **I2C1** (PB8/PB9) | UM1907 CN2 / P1 |
| Address | **0x78** (8-bit write) | OV5640 / H747I BSP |
| Register addresses | **16-bit** (`I2C_MEMADD_SIZE_16BIT`) | OV5640 datasheet |
| Chip ID | 0x300A/0x300B = **0x5640** | OV5640 datasheet |
| Speed | ~100 kHz standard mode | SCCB |

TIMINGR is recomputed for **PCLK1 = 54 MHz** (RM0385 §30.4.10): PRESC=11 / SCLL=24 / SCLH=19 / SCLDEL=5 / SDADEL=2 → SCL ≈ 99 kHz. The ST BSP constant `0x40912732` assumes APB1 = 50 MHz and is deliberately not reused (it would land at ~118 kHz on this clock tree).

## Power and reset

- `camera_init()` writes the OFF level to PWR_EN (PH13) **before** switching the pin to output, so the module never sees a power glitch
- Every probe starts with a **power cycle** (high 100 ms → low → 20 ms settle, the H747I BSP HwReset timing)
- DCMI_NRST cannot be driven from a GPIO, so a **PWR_EN cycle plus the OV5640 software reset** (`OV5640_ReadID` writes 0x3008=0x80 and waits 500 ms) stands in for it

## OV5640 component driver (submodule)

Sensor register control uses ST's official **OV5640 BSP component driver**:

- submodule: `lib/ov5640` = [STMicroelectronics/stm32-ov5640](https://github.com/STMicroelectronics/stm32-ov5640) **v4.0.3** (BSD-3-Clause, see `NOTICE`)
- BSP v2 API (`OV5640_Object_t` + `OV5640_IO_t`), pure-I2C and self-contained (no DCMIPP dependencies)
- The bus glue is `cam_io_*` in `port/camera/camera.c` (`HAL_I2C_Mem_Write/Read`). `OV5640_ReadID()` calls `IO.Init()` unconditionally, so **no-op Init/DeInit stubs must be registered**

!!! note "Why not stm32-mw-camera"
    ST's Camera Middleware (`stm32-mw-camera`) and X-CUBE-ISP target **DCMIPP** (STM32N6 family) and cannot drive the F7's classic DCMI. Only the standalone OV5640 component driver is pulled in.

## Capture path (DCMI + DMA2, #41)

| Item | Value | Source |
|------|-------|--------|
| Resolution / format | **QVGA 320x240 RGB565** (153,600 B, little-endian) | `OV5640_Init(R320x240, RGB565)` |
| DCMI | 8-bit parallel, HW sync, **HSYNC=HIGH / VSYNC=HIGH / PCLK=RISING** | H747I-DISCO BSP's proven OV5640 values (`OV5640_Init` programs the matching sensor side) |
| DMA | **DMA2 Stream1 / Ch1** (RM0385 Table 26; SD owns Stream3/6 -- no conflict), `DMA_NORMAL` single-shot, word, FIFO full + MBURST INC4 | 38,400 words ≤ the 65,535 NDTR ceiling → one plain transfer |
| Frame buffer | `.sdram` section (8 MB SDRAM, **MPU non-cacheable** → no cache maintenance) | see [SDRAM](sdram.md) |
| NVIC | DCMI=8, DMA2_Stream1=8 (below USART1=5 / SDMMC=6 / SD-DMA=7, above SysTick=14) | ThreadX masks with PRIMASK, so `tx_semaphore_put` is ISR-priority-agnostic |

Completion model (HAL): once the DMA has moved the frame's words, `DCMI_DMAXferCplt` arms the **FRAME interrupt**; the FRAME ISR calls `HAL_DCMI_FrameEventCallback` → `tx_semaphore_put`. Sync errors, overrun and DMA errors all funnel into `HAL_DCMI_ErrorCallback`. The same **drain + `cam_xfer_active` gate + finite timeout (1 s)** discipline as the SD driver suppresses stale callbacks.

Sensor setup is lazy, at capture time: probe if needed → `OV5640_Init` (once per power-up, 300 ms AEC/AWB settle) → `OV5640_ColorbarModeConfig` (100 ms settle on a live/colorbar switch) → snapshot. `sdram test` clobbers `.sdram`, so it calls `camera_frame_invalidate()` first to drop the captured-frame flag.

## Locking and thread context

- Public API calls serialize on an internal ThreadX mutex (TX_INHERIT). The real work lives in `*_locked()` helpers, so no public entry can **re-acquire the mutex it already holds** (Phase 2's capture will probe on demand through the `_locked` variant)
- **Thread context only** -- never call the API from an ISR
- Initialization runs once from `tx_application_define()` (`src/main.c`) and performs **no sensor I/O** (GPIO/I2C1 setup plus the mutex). The first sensor access happens lazily on `camera probe`

## The `camera` shell command

```
camera probe             power cycle + read the OV5640 chip ID (~1 s)
camera info              driver / sensor state + current mode / quality settings
camera res <resolution>  switch resolution qqvga|qvga|480x272|vga|wvga (#45)
camera format <fmt>      switch pixel format rgb565|yuv422|y8|jpeg (#45)
camera fps <15|30>       frame rate 15 (PCLK 24M) / 30 (PCLK 48M); 30 needs lcd disable (#67)
camera capture [test]    snapshot one frame in the current mode (test = colorbar)
camera save <sd|fs> <p>  write the captured frame to a file, raw (mode's format)
camera set [<name> <v>]  OV5640 image-quality controls (no arg = show)
camera off               cut module power
```

`camera capture` prints per-channel (R5/G6/B5) min/max/mean statistics and a hexdump of the first 16 pixels. `camera capture test` grabs the OV5640's built-in **8-bar colorbar** (white/yellow/cyan/green/magenta/red/blue/black), validating DCMI polarity, timing and wiring **independent of optics** (expect mins near 0, maxes near saturation, flat values within each band). On a live capture, covering the lens must pull the means down.

Example session:

```
sh> camera capture test
camera: capturing colorbar test frame ...
frame: 320x240 RGB565 (153600 bytes)
R5: min  0  max 31  mean 15.4
G6: min  0  max 63  mean 31.2
B5: min  0  max 31  mean 15.5
row0[0..15]:
00000000: ff ff ff ff ff ff ff ff  ff ff ff ff ff ff ff ff
00000010: ff ff ff ff ff ff ff ff  ff ff ff ff ff ff ff ff
sh> camera capture
camera: capturing live frame ...
frame: 320x240 RGB565 (153600 bytes)
...
```

## Saving frames and viewing them on a PC (#42)

`camera save <sd|fs> <path>` writes the captured frame **in the current mode's raw format** (RGB565/YUV422/Y8 raster, or a JPEG stream) to the chosen medium (`sd` = microSD FAT32, `fs` = QSPI NOR), streaming in fixed chunks (512 B, sized for the shell's 2 KB stack) through `camera_frame_read` into a FileX write -- no staging buffer. The valid length comes from `camera_get_info`'s `frame_bytes` (for JPEG the stream trimmed to the EOI). It runs under the same **shared op slot** as the fs/sd command bodies, so `sd format`/`umount` cannot yank the media mid-save. Ctrl+C cancels (a partial file is reported as such). Per-format PC conversion is in the [#45 section](#resolution-pixel-format-frame-rate-45).

Read the microSD card on the PC (card reader) and convert with the bundled script:

```
sh> camera capture test
sh> camera save sd /bar.raw
wrote 153600 bytes (320x240 RGB565 raw) to sd:/bar.raw
PC: python3 scripts/rgb565_to_png.py <file> out.png
```

```console
$ pip install Pillow
$ python3 scripts/rgb565_to_png.py bar.raw bar.png
wrote bar.png (320x240)
```

Channels are expanded to 8 bits by bit replication (full-scale 5/6-bit values map to exactly 255). Use `--width/--height` for other geometries.

## Image-quality settings (camera set, #44)

`camera set` adjusts the OV5640's built-in ISP (the B-CAMS-OMV has no module-side LED/AF, so only the sensor ISP is controllable). Settings live in a **RAM cache** in `port/camera`: applied over I2C immediately when the sensor is live, or cached and applied in one pass by the next capture's lazy configure (`OV5640_Init` rewrites the SDE register block, so the cache must be re-applied after every init). This is a **shared control layer**: both the `camera set` shell commands and the GUIX camera UI settings screen (#68, reached by tapping the live image) call the same `camera_set_*`. **flip defaults to `flip`** (`CAM_FLIP_FLIP`, #68) for this board's camera-module mounting, so the boot preview / capture come out upright (the other settings default to neutral); use `camera set flip none` to disable it.

| Setting | Value | Meaning |
|---------|-------|---------|
| `brightness` | -4..4 | brightness |
| `contrast` | -4..4 | contrast |
| `saturation` | -4..4 | saturation |
| `hue` | -180..150 (30° steps) | hue (converted to a -6..5 index internally) |
| `awb` | auto / sunny / office / home / cloudy | white balance (light mode) |
| `effect` | none / bw / sepia / negative / blue / red / green | special color effect |
| `flip` | none / mirror / flip / both | mirror / vertical flip |
| `zoom` | 1 / 2 / 4 / 8 | digital zoom (ISP scaling, QVGA-capable) |
| `night` | on / off | night mode (AEC stretches 15→3.75 fps) |
| `default` | — | reset to neutral (flip defaults to `flip`, #68) |

Each setting is a **subcommand** of `camera set` (`camera_set_subcmds` in `shell/cmds/cmd_camera.c`), so the hierarchical help (#37) lists them all and gives per-setting usage; omitting the value auto-prints that subcommand's usage.

```
sh> help camera set       # recursively lists the settings
camera set -- OV5640 image quality (no arg = show current)
Subcommands:
  brightness <-4..4>
  contrast   <-4..4>
  saturation <-4..4>
  hue        <-180..150> in 30 deg steps
  awb        <auto|sunny|office|home|cloudy>
  effect     <none|bw|sepia|negative|blue|red|green>
  flip       <none|mirror|flip|both>
  zoom       <1|2|4|8>
  night      <on|off>
  default    reset all settings to neutral
Type 'help camera set <subcommand>' for details.
sh> camera set brightness 2
camera: brightness = 2
sh> camera set brightness         # omitting the value prints usage
camera set brightness: invalid number of arguments
usage: camera set brightness  (<-4..4>)
sh> camera set                    # no arg lists the current values
brightness: 2
contrast:   0
saturation: 0
hue:        0 deg
awb:        auto
effect:     none
flip:       none
zoom:       x1
night:      off
type 'help camera set' for the list of settings
```

!!! warning "SDE_CTRL0 / SDE_CTRL8 overwrite bug and fixup"
    Each `lib/ov5640` (read-only submodule) setter **overwrites the SDE master-enable register `SDE_CTRL0` (0x5580) with only its own enable bit**, so with the ST driver as-is only the **last-applied** of brightness/contrast/saturation/hue takes effect. Worse, the sign / UV bits in **`SDE_CTRL8` (0x5588)** are updated through the vendored `ov5640_modify_reg`, which is **OR-only (can set but never clear a bit)**.

    Without editing the submodule, `port/camera` absorbs this: it applies the setters in a **fixed order**, then read-modify-writes `SDE_CTRL0` to the OR of every active function's enable bit and `SDE_CTRL8` to the **exact** value derived from the cache. All four controls then coexist, with no stale hue / negative-brightness sign bits (per OV5640 datasheet table 7-26 bit definitions).

    **Tint effects (bw/sepia/blue/red/green) own `SDE_CTRL3/4` via fixed U/V**, which collides with saturation/hue, so saturation/hue are skipped while a tint effect is active (they have no visible effect while U/V is fixed anyway).

## Resolution / pixel format / frame rate (#45)

`camera res` / `camera format` switch the capture mode. The driver's single source of truth `struct camera_mode` (`port/camera/camera.c`) holds the geometry / format / timing, and `camera_set_format()` re-programs the OV5640 (resolution and format scalers + the per-mode HTS/VTS/PCLK fps table). `camera info` shows the current `WxH`, format, target fps and whether the mode is streamable.

| Axis | Values |
|------|--------|
| Resolution | `qqvga`(160x120) / `qvga`(320x240) / `480x272` / `vga`(640x480) / `wvga`(800x480) |
| Format | `rgb565`(2 B/px) / `yuv422`(2 B/px, YUYV) / `y8`(1 B/px greyscale) / `jpeg`(variable) |

### Snapshot vs stream asymmetry (a hardware constraint)

- **Snapshot supports every mode**: a VGA/WVGA frame exceeds the DMA NDTR limit (65535 words), but `HAL_DCMI_Start_DMA` transparently **bands it intra-frame** (DBM into one contiguous buffer: VGA = 4×38400w, WVGA = 4×48000w), firing FRAME once at the end.
- **Raster streaming only supports modes with frame_words ≤ 65535** (QQVGA/QVGA/480x272 in RGB565/YUV422). The producer's manual DBM points each M-register at a whole slot via NDTR, so larger raster modes (VGA/WVGA) have `mode.streamable=0` — flagged `capture only` in `camera info` and refused by `camera stream start` (`mode.streamable=0` means "outside the raster DBM path").
- **JPEG (≤ VGA) streams via a separate path** (#63, see "JPEG streaming" below). Being variable-length it uses a **snapshot-loop** (one DCMI SNAPSHOT per frame + the FRAME interrupt) rather than DBM/TC. JPEG snapshot still works as before.

### Frame rate (per-mode HTS/VTS + 15/30 fps knob, #45/#67)

The `lib/ov5640` common table sets HTS=1936/VTS=1088 for **every** resolution at a fixed 24 MHz PCLK, so the default is **~11fps**. The timing table (`mode_fps[]`) applied by `camera res` gives the streamable small modes a tightened HTS/VTS (1600/1000 → **~15fps**, 14.9fps measured) and keeps the snapshot-only VGA/WVGA on the full common timing (1936/1088, ~11fps). `fps = PCLK/(HTS×VTS)`.

**`camera fps <15|30>` switches the PCLK between 24 and 48 MHz (#67).** The small modes (QQVGA/QVGA/480x272) keep HTS/VTS=1600/1000, so `48e6/(1600×1000)=30.0fps`. The default is **15fps (safe)**. `camera fps` re-applies to the sensor at once like `camera res/format`, and is refused while a stream/preview owns the DCMI (its PLL must not be retuned under a live DMA target).

!!! danger "30fps (48 MHz) requires LTDC scanout to be stopped"
    48 MHz doubles the peak DCMI burst rate and, while the LTDC continuously reads the framebuffer from the 16-bit SDRAM, **even the smallest QQVGA overruns within a few hundred ms** (`camera stream stats` shows `stopped (overrun)`). Measured: with **`lcd disable` (LTDC scanout OFF, #66)** all of qqvga/qvga/480x272 RGB565 run at ~30fps with 0 overrun and 0 FE (even 480x272 = 7.8 MB/s is perfectly clean).

    So the effective PCLK is decided by a **single predicate**: 48 MHz **only when "fps 30 selected ∧ small mode ∧ `ltdc_scanout_active()`==false"** all hold; otherwise it **auto-clamps to 24 MHz** (a clamp, not a reject). This predicate is applied at every path that programs the sensor PCLK (`camera_set_format` / before a stream arm / before a snapshot arm), so selecting fps 30 while `lcd enable` is active or a GUIX preview is running safely falls back to 15fps. `camera info`'s `fps select` line shows the selection and the clamp reason (`lcd scanout active` / `snapshot-only mode`), and `camera stream start` prints a clamp note.

    **Remaining constraint (TOCTOU):** if you start a 30fps stream under `lcd disable` and then `lcd enable`, the contention returns and overruns -- the stream **gracefully auto-stops** (counted in `stopped (overrun)` / `ovr dcmi`). Do not `lcd enable` during a 30fps stream. Cutting the LTDC scanout bandwidth to get "30fps even with preview" is a separate scope (#59/#65).

!!! warning "VTS vs AEC max exposure clamp"
    If VTS drops below the AEC max-exposure line count (`0x3A02/03` and `0x3A14/15`, default 0x3D8=984) the exposure clips and the frame darkens / bands. `camera_set_format` reads **both** max-exposure pairs back **after re-applying the quality settings** (night mode rewrites AEC) and guarantees `VTS ≥ max + margin`, raising VTS (and lowering fps below target) if needed. The fps table values are a **starting point to tune against on-hardware `camera stream stats`** (contention tuning is #59).

```
sh> camera res vga
camera: res = vga
sh> camera format yuv422
camera: format = yuv422
sh> camera info
...
mode:       640x480 yuv422
fps target: 11.4  (capture only -- too large to stream)
sh> camera save fs /shot.raw
wrote 614400 bytes (640x480 yuv422) to fs:/shot.raw
PC: python3 scripts/yuv422_to_png.py --width 640 --height 480 /shot.raw out.png
```

JPEG saves the variable-length stream as-is (no conversion -- open it as `.jpg`):

```
sh> camera format jpeg
sh> camera capture
frame: 320x240 JPEG (12784 bytes)
SOI: ok (FFD8)
EOI: ok (FFD9)
sh> camera save sd /shot.jpg
PC: /shot.jpg is a JPEG stream -- open it directly
```

### PC conversion scripts

| Format | Conversion |
|--------|------------|
| RGB565 | `python3 scripts/rgb565_to_png.py --width W --height H in.raw out.png` |
| YUV422 | `python3 scripts/yuv422_to_png.py --width W --height H in.raw out.png` |
| Y8 | `python3 scripts/y8_to_png.py --width W --height H in.raw out.png` |
| JPEG | none needed (open the `.jpg` directly) |

`camera save` / `camera capture` print the right conversion command for the current mode.

### SDRAM budget

`cam_frame` is sized for the largest fixed format, WVGA RGB565 (768,000 B), as one contiguous region (also the JPEG budget buffer). The stream ring is **partitioned at runtime from the camera arena `cam_arena` (2 MB, FMC internal bank1 @0xC0200000) as of #65** (replacing the old fixed `cam_ring[4][256KB]`): slot stride = `align32(frame_bytes)`, slot count = `min(2MB/stride, 8)`, so small modes get a deeper ring. Bank placement and the `ASSERT`s are in [SDRAM](sdram.md#fmc-internal-bank-placement-65).

## Continuous capture (streaming, #46)

Where `camera capture` takes a single snapshot (`DCMI_MODE_SNAPSHOT` + `DMA_NORMAL`), `camera stream` captures continuously with **DCMI continuous + DMA double-buffer (DBM)**. Captured frames flow into the [frame pipeline](../architecture/frame-pipeline.md) (one source → many sinks, #47); the primary #46 deliverable is a display-independent **FPS / overrun measurement**. LTDC display and burst saving plug in later as sinks.

### Ring and DBM

An **N-slot ring** partitioned at runtime from `cam_arena` (bank1; deeper for small modes, capped at 8) is injected into `frame_pipeline_init` (#65; a different bank from `cam_frame[]`). DBM always keeps two slots as DMA targets (M0AR/M1AR), one holds the latest published frame, and one is free to acquire (N≥4 is comfortable, N≥2 is the DBM minimum). Observe the live value via `camera stream stats`'s `ring: N slots x M B`. The HAL's internal `HAL_DCMI_Start_DMA` DBM split only triggers for `Length>0xFFFF` and is *intra-frame* banding, so it is not used; instead the producer drives an *inter-frame* N-slot ring explicitly with **`HAL_DMAEx_MultiBufferStart_IT` + `HAL_DMAEx_ChangeMemory`**.

### Threading (the ISR only notifies)

| Layer | Role |
|---|---|
| DMA TC ISR (`DMA2_Stream1`, prio 8) | posts `cam_stream_sem` only; never touches the ring / CT |
| **producer thread (prio 10, dedicated)** | identifies the completed buffer via `CT` → `acquire`s a free slot → repoints the completed M-register (`HAL_DMAEx_ChangeMemory`) → **then publishes**. Also owns auto-stop (--frames/--secs/OVR) and teardown |
| CLI command (prio 16) | issues `start`/`stop`/`stats` and returns at once; never touches a frame |

**Tear-free invariant**: the "acquire → repoint → publish" order means a slot handed to a sink is never a live DMA target. With no free slot the frame is dropped, not published (`ring_ovr`).

### Commands (non-blocking)

```
camera stream start [test] [--frames N] [--secs S]   returns at once (capture runs in the background)
camera stream stop                                    stop
camera stream stats                                   FPS / frames / overruns
```

`start` returns immediately and never occupies the CLI prompt; the producer thread runs the capture and auto-stops on `--frames`/`--secs`, `stream stop`, or a **DMA transfer error (TE)**. Streaming and `camera capture` share one DCMI/DMA and are mutually exclusive (capture is rejected as busy while streaming).

**DMA error handling (#56)**: the double-buffer arm (`HAL_DMAEx_MultiBufferStart_IT`) enables the **FIFO-error interrupt** the snapshot path (`HAL_DMA_Start_IT`) leaves off. With an incompatible FIFO-threshold/burst an FE can hardware-disable the stream, but this implementation's `FIFO_THRESHOLD_FULL + MBURST_INC4` is a valid combination, so the FE (FIFO overrun/underrun) / DME observed under the SDRAM contention from the LTDC continuously reading the framebuffer do **not** halt the stream (the snapshot path simply never observes them). They are therefore **counted and tolerated** (`stats` `dma fe`, thousands/s at QVGA), and only a **TE — which the hardware uses to actually stop the stream — is terminal**. A DCMI FIFO overrun (`ovr dcmi`) is a separate path and should stay near zero. **#59** mitigates this preview-time FE (LCD_CLK 9.6→4.8 MHz + dropping the DMA2D copy-forward; `stats` `dma fe/s` is the figure of merit — see [GUIX](../rtos/guix.md) / [display](display.md)).

### JPEG streaming (snapshot-loop, #63)

Raster is fixed-length, so the **DBM + TC interrupt** delimits each frame; **JPEG is variable-length** and ends short of the DMA budget, so TC never fires at the frame boundary. JPEG therefore takes a separate path — a **snapshot-loop**:

- Each frame is **one DCMI SNAPSHOT (CM=1)** armed into a ring slot (`HAL_DCMI_Start_DMA`). As in the snapshot path, the **FRAME interrupt** is explicitly enabled and becomes the variable-length frame boundary (`HAL_DCMI_FrameEventCallback` posts `cam_stream_sem` while `cam_stream_active`).
- On the FRAME wake the producer thread does **`HAL_DCMI_Stop` → read NDTR → `eff=(budget−NDTR)×4` → SOI/EOI trim** (shared with the snapshot finalize via `jpeg_trim`) → **publish** at that length → **re-arm** into a free slot. The DCMI stops capturing after each frame, so the FIFO cannot overrun during the re-arm gap (a CM=0 continuous re-point would).
- A failed `HAL_DCMI_Stop` / re-arm is **terminal** (`cam_stream_err` → teardown). `--frames`/`--secs` targets are re-checked after publishing so no extra capture is armed.
- Frames with **no SOI/EOI** are dropped and counted in `camera stream stats` as **`jpeg trunc`**; free-slot exhaustion is `ovr ring`. Frames that arrive in the Stop..re-arm gap are not captured (DCMI is disabled) and **by design are not counted**.
- **Retrieval**: each JPEG publish mirrors the finalised frame into the (idle during streaming) `cam_frame`, so `camera save` / `camera send` can pull the latest JPEG frame during or after the stream.
- **No preview**: the F746 has no JPEG decode path, so the camera UI live preview (boot / `gui start`) is explicitly refused while `camera format jpeg` (`camera format rgb565` first). The intended consumer is #49 (network streaming — JPEG compression keeps high resolutions within bandwidth).

```
camera format jpeg; camera res qvga
camera stream start --secs 5      # continuous JPEG (snapshot-loop)
camera stream stats               # frames/fps, jpeg trunc
camera save fs /shot.jpg          # save the latest frame as JPEG (SOI/EOI consistent)
```

### GUIX live preview (#56/#61)

The GUIX camera UI (#61) attaches a GUIX push sink to this streaming pipeline at **boot / `gui start`** and shows the QVGA frames **at native scale** on the LTDC (GUIX) screen (since #61 this is the default UI, so live video shows right after power-on). Preview **forces QVGA RGB565** (`camera_preview_start` calls `camera_set_format_locked(QVGA,RGB565)` under the same lock), so it always matches the #56 sink (QVGA RGB565 only) regardless of the last `camera res/format`. While the preview owns the stream, `camera stream start/stop` and `camera res/format/set` are refused (escape hatch: `gui stop`). See [GUIX › Camera live preview](../rtos/guix.md#camera-live-preview-5661) for details.

## References

- UM2779 — B-CAMS-OMV user manual (CN5 pinout, MB1379/X1 crystal, JP1)
- UM1907 §7.2 — P1 camera connector pinout
- RM0385 §30 — I2C (TIMINGR)
- `_ref/STM32Cube_FW_H7_V1.13.0/Drivers/BSP/STM32H747I-DISCO/stm32h747i_discovery_camera.c` — reference for the OV5640 power sequence / polarities
