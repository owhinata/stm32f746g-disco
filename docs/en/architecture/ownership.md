<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 ThreadX Shell Project
-->

# Ownership & state model (display / DCMI / touch)

Several subsystems share three pieces of hardware: the **LTDC display**, the
**DCMI / camera** capture path, and the **FT5336 touch** controller on I2C3. The
shell, the `lcd`/`camera`/`touch` commands, and the GUIX camera UI all want them.
This page is the single reference for **who owns what in each state, and which
shell commands are allowed** — the model behind the cross-subsystem guards in
`shell/cmds/cmd_*.c`.

- The **LTDC display** and the **FT5336 touch** are **single-owner**: only
  **one** user can drive each at a time.
- The **DCMI / camera** dropped single ownership in Epic #99 (#100/#101) and
  became an **explicit base capture (`camera stream`) + subscriber cascade**: a
  single capture path (the producer) that an internal stat sink and up to three
  external subscribers (GUI preview / AI inference (nncam) / MJPEG) can **attach
  to at the same time**.

## Shared resources and their owners

| Resource | Owner / state flag (where) | Taken by | Released by |
|---|---|---|---|
| **LTDC display** (draw + scanout, single-owner) | `ltdc_gui_owned` (`port/ltdc/ltdc_display.c`) | `gui start` → `ltdc_gui_take(true)` | `gui stop` |
| **DCMI / camera** (base capture + subscriber cascade) | `cam_stream_active` + the subscriber registry `cam_subs[]` (`{sink, fmt, enabled, attached, oneshot}` under `cam_lock`, `port/camera/camera.c`) | shell `camera stream start` (the sole starter of the base) | `camera stream stop` / `camera off` (cascade) |
| **FT5336 touch** (I2C3, single-owner) | *no flag* — `guix_is_up()` is the proxy | the GUIX touch thread (`port/guix/guix_touch.c`) while `gui` runs | `gui stop` |

Storage media (`sd`/`fs`/`qspi`) has its own `fs_raw_begin`/`fs_raw_end` slot
(see [Filesystem](../rtos/filesystem.md)); it is not a display/input concern and
is omitted here.

## Base capture + subscriber cascade (DCMI)

The DCMI is physically constrained to one path, one format, one resolution
(RM0385 §14). The old "mode ownership" (a higher feature swallows lower ones via
a single `cam_ext_sink`) is gone, replaced by this model:

- **base = `camera stream`**: the sole DCMI producer, turned ON by `camera
  stream start`. format/res are properties of the base (set with `camera
  format`/`camera res`/`camera fps` **while the base is OFF**). While ON, the
  internal `cam_stat_sink` (FPS / overrun counters) is always attached.
- **features = subscribers**: GUI preview / AI (nncam) / MJPEG each **enable**
  themselves (`enabled`) via their own start/stop, and attach their sink to the
  pipeline **only while the base is running AND the format matches**. A
  subscriber's enabled state is **orthogonal** to base on/off (a subscriber
  enabled while the base is OFF attaches at the next base start).
- **format-class arbitration**: the base is one format. The subscribers that
  consume **RGB565 raster** (GUI preview / AI / stat) can coexist. **JPEG** is
  an exclusive class (MJPEG only). An incompatible explicit attach request is
  refused with an explicit error stating "the base's current format" (e.g. `net
  mjpeg start` while the base is RGB565).
- **`camera stream stop` / `camera off` = cascade**: every attached subscriber
  is detached via `close()` and the producer stops (a master switch, not a BUSY
  refusal). There is **no auto-stop** (stopping every subscriber leaves the base
  ON until an explicit stop). On the cascade the GUI preview **freezes** on its
  last frame (persistent, re-attaches), AI PAUSEs (keeps its run/session), and
  MJPEG **stops** (oneshot, fully released).
- **camera power is orthogonal**: `camera on`/`off` is a separate axis from the
  base. A `camera off` while the base is ON cascades a stop first, then cuts
  power.
- **DCMI overrun auto-recovery lives in the producer** (`cam_stream_recover`,
  escalating backoff): a transient overrun keeps the subscribers and re-attaches
  them; giving up turns the base OFF (the old GUI-overlay-specific backoff is
  gone).

## States

The DCMI is not an "exclusive 3-mode" machine; it is decided by the orthogonal
state of **the base's on/off (+ format)** and **each subscriber's
enabled/attached**. Camera **power** (on/off) is a further orthogonal axis.

```
   camera format {rgb565|jpeg|...}         camera stream start
   camera res / fps  (only while base OFF) ──────────────────────┐
        ▲                                                        ▼
┌───────────────────┐                          ┌────────────────────────────────┐
│  base OFF          │   camera stream start    │  base ON  (one format)          │
│  (DCMI idle)       │ ───────────────────────▶ │  producer + cam_stat_sink       │
│  camera capture ok │ ◀─────────────────────── │  + attaches enabled & matching  │
│  format/res edit ok│   camera stream stop      │    subscribers (cascade)        │
└───────────────────┘   camera off (cascade)    └────────────────────────────────┘
                                                     │  ▲ subscribers enable via their
   subscriber (gui / ai / net mjpeg):                │  │ own start/stop. attach on base
   enabling is orthogonal to the base. attach with ──┘  │ ON & fmt match; stay enabled+idle
   base ON & fmt match, else stay enabled+idle.         │ on base OFF / mismatch.
   base stop = cascade: gui freezes / ai PAUSE / mjpeg stops ┘

  • RGB565 raster (gui/ai/stat) coexist. JPEG (mjpeg) is an exclusive class.
  • Camera power is orthogonal: while base ON, off/probe/res/format/fps are refused BUSY.
```

## What each command can do

`OK` = succeeds · `✗` = refused (reason) · `read-only` = reads existing
state/frame only (allowed). Columns are the base's state (OFF / ON=RGB565 raster
/ ON=JPEG). GUI/AI/MJPEG act orthogonally as subscribers.

| Command | base OFF | base ON (RGB565) | base ON (JPEG) |
|---|---|---|---|
| `camera probe` / `on` / `off` | OK | ✗ BUSY *(off cascades a stop, then cuts power)* | ✗ BUSY |
| `camera res` / `format` / `fps` / `set`*(timing)* | OK | ✗ BUSY *(`camera stream stop` first)* | ✗ BUSY |
| `camera capture` | OK | ✗ BUSY | ✗ BUSY |
| `camera stream start` | OK → base ON | *(already running)* | *(already running)* |
| `camera stream stop` | ✗ not streaming | OK → base OFF *(cascade)* | OK → base OFF *(cascade)* |
| `camera stream stats` / `info` | read-only | read-only | read-only |
| `camera save` / `send` | last `camera capture` (or no frame) | **latest raster frame** | **latest JPEG frame** |
| `gui start` | starts preview idle ("no capture") | live preview (attaches) | preview unavailable (JPEG, "needs RGB565") |
| `gui stop` | unsubscribe (base/AI keep running) | unsubscribe | unsubscribe |
| `ai stream start` / `stop` | enable/disable (idle while base off) | attach / detach (independent of gui) | idle *(wants RGB565; not attached on a JPEG base)* |
| `net mjpeg start` | ✗ "no camera capture" | ✗ "raster; needs JPEG" | attach (JPEG-class subscriber) |
| `net mjpeg stop` | ✗ not running | detach | detach |
| `lcd` draw | OK *(✗ under GUI)* | OK *(SDRAM contention)* / ✗ under GUI | same as left |
| `touch probe` / `read` | OK *(✗ under GUI, #73)* | same as left | same as left |
| `sd` / `fs` / `qspi` | OK | OK | OK |

(`gui` also takes single ownership of the LTDC + touch at the same time; the GUI
row above is only its DCMI-subscriber facet.)

## Interlock primitives (where the guards live)

- **Display** — `ltdc_gui_owns()` gates every public draw/flip
  (`ltdc_fill`/`ltdc_flip`/…), and the shell `lcd` draw commands refuse up-front
  via `lcd_can_draw()` (`shell/cmds/cmd_lcd.c`). GUIX presents through the
  owner-only `ltdc_gui_flip()`. `ltdc_set_scanout()` is refused while GUIX owns
  the display, so `lcd on`/`off` toggle only the backlight in that state.
- **DCMI / camera** — while the base is ON (`cam_stream_active`),
  `camera_set_format`/`fps`, `camera_capture`, `camera_probe`, and
  `camera_power_off` return `CAM_ERR_BUSY` (shown as `busy (streaming or preview
  active)`) = turn the base OFF before configuring / operating power. Subscriber
  membership is managed centrally by the `cam_subs[]` registry under `cam_lock`,
  entered and left via `camera_subscribe` (persistent: gui/nncam) /
  `camera_subscribe_oneshot` (non-persistent: mjpeg) / `camera_unsubscribe`
  (attach requires base running && enabled && `sub.fmt==mode.format`). The
  read-only paths (`info`/`save`/`send`/`stream stats`) take only the lock and
  are always allowed.
- **NET-MJPEG** (#49 P5, made a subscriber in #101) — `net mjpeg start` does NOT
  own the base; it is a **JPEG-class subscriber that attaches to a running JPEG
  base** (`camera_subscribe_oneshot(&eth_sink, CAM_FMT_JPEG)`). With the base off
  or an RGB565 base it returns an explicit error (#97 fixed). A `camera stream
  stop` / `camera off` cascade (a non-recover stop) fully releases it =
  auto-stopped; a transient overrun pauses then resumes it (`camera_subscribed()`
  is the single source of truth). `net mjpeg stop` only detaches its own sink
  (the base keeps running). The eth_sink is a synchronous copy sink (in-flight 0),
  so the producer's async teardown stays safe.
- **Touch** — there is no touch ownership flag (`guix_is_up()` is the proxy).
  Two things protect the shell: (1) `touch_read()` (`port/touch/touch.c`) drops
  the FT5336 all-ones "not touched" sentinel — an out-of-panel `0xFFF/0xFFF`
  point that an idle or just-released controller reports with a *nonzero* status
  count (the same point the GUIX driver filters) — so a poll never prints the
  phantom `P15 x=4095 y=4095 event=3`; and (2) the bus-touching `touch
  probe`/`read` refuse while `guix_is_up()` (`shell/cmds/cmd_touch.c`, #73),
  because a shell poll concurrent with the GUIX touch thread would be a second
  unsynchronised I2C3 master. `touch info` does no I/O and stays allowed.

**Lock order**: camera internals are consistent under `cam_lock → cam_pipe_lock`
(the pipeline mutex). Because a subscriber's `close()` can be called from the
producer thread, it must not re-enter the camera API (non-blocking).

## Notes & caveats

- **`lcd on`/`off` under GUI** toggles the backlight but leaves scanout running
  (GUIX owns it) — deliberately allowed (blanking the panel is harmless to the
  UI). Everything *visible* belongs to GUIX until `gui stop`.
- **Read-only commands** (`*info`, `camera save`/`send`, `camera stream stats`)
  work in every state; they never re-arm hardware. **`camera save`/`send` (the
  #102 redefinition)**: **while the base is ON they always snapshot the latest
  published frame** and save/send it (raster or JPEG, even with MJPEG/GUI
  subscribers attached). While the base is OFF they use the last `camera capture`
  frame (or `CAM_ERR_NO_FRAME` if none). The #82 mechanism that latched a
  two-value mirror on whether an external sink was present is gone; the producer
  does no per-frame copy (save/send pins one copy via `frame_pipeline_pin_latest`
  at invocation). The pin never stalls the producer's publish/DMA-repoint, so the
  **live preview keeps running at full fps**; but a SD/QSPI save while the base is ON
  is **slow** (tens of seconds for QVGA raster) because it contends with the
  continuous DCMI DMA for the single 16-bit FMC SDRAM (round-robin arbitration
  starves the low-priority CLI save -- the same bandwidth limit as #84/#90). Stop
  the stream first for a fast save. See [Camera](../hardware/camera.md) for details.
- See [Camera](../hardware/camera.md), [Display](../hardware/display.md),
  [Touch](../hardware/touch.md), [GUIX](../rtos/guix.md) and [Frame
  pipeline](frame-pipeline.md) for each subsystem's details.
