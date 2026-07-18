# GUIX (Eclipse ThreadX GUIX integration)

On top of the board's 4.3″ 480×272 LCD (LTDC, #52/#53) and the FT5336 capacitive touch panel (I2C3, #54), this brings up **Eclipse ThreadX GUIX** (the GUI framework). GUIX runs as a ThreadX thread; its 565rgb display driver is bound to the LTDC tear-free double buffer + DMA2D, its input driver to the FT5336. The UI has **two full-screen pages** (#61/#68): a **preview page** — just a `GX_ICON` drawing the camera frame (default QVGA, resolution-selectable #69/#84) native 1:1 centred, a clean live image — and a **settings page** reached by **tapping the live image** (a static panel, no live image, holding the 9 OV5640 image-quality controls, a preview-resolution selector (#69) and Back). Introduced as Phase 4 of the LTDC + GUIX epic (#48, #55), extended with the camera compositing (#56), boot-ON (#60), the camera-only UI (#61), the two-page control UI (#68), and selectable preview resolution (#69).

- Submodule: `lib/guix` ([eclipse-threadx/guix](https://github.com/eclipse-threadx/guix) v6.5.1, **MIT**). A read-only mirror like `threadx`/`filex`/`levelx`.
- Driver/glue: `port/guix/` (display / input / lifecycle). The camera UI app itself lives in the presentation layer `ui/guix_camera_ui.c` (#61, layering #43). Shell command `gui` (`shell/cmds/cmd_gui.c`).
- Started ON at boot (#60/#61): GUIX takes over the LCD and touch from `tx_application_define()` at boot, so **live video shows right after power-on**, resident and symmetric with the camera producer. Run `gui stop` to release the display so the `lcd`/`touch`/`camera` test commands work, and `gui start` to resume.

## Configuration

| Item | Value | Notes |
|------|-------|-------|
| GUIX | Eclipse ThreadX GUIX **v6.5.1** (MIT) | `lib/guix`, `common/src/*.c` globbed |
| Port | Cortex-M7 / GNU (`ports/cortex_m7/gnu/inc/gx_port.h`) | **no asm** (binds to ThreadX) |
| Config | `port/guix/gx_user.h` (`GX_INCLUDE_USER_DEFINE_FILE`) | same idiom as FX/LX |
| Display | 480×272 **RGB565**, 1 layer, LTDC double buffer | canvas = LTDC back buffer |
| Acceleration | **DMA2D**: solid fills (R2M) + double-buffer copy-forward (M2M) | RM0385 §9 |
| Input | FT5336 (I2C3) EXTI13 interrupt wake → GUIX pen events (#62) | `port/guix/guix_touch.c` |
| Threads | GUIX system = prio **14** / input = prio **13** | `gx_user.h` / `guix_touch.c` |

## Threads and priorities

GUIX's default `GX_SYSTEM_THREAD_PRIORITY` is 16, which collides with this project's shell instance (cli prio 16). `gx_user.h` moves it to **14**. The full picture (lower = higher priority):

```
IWDG petter (5)  >  camera/LED (10)  >  touch-input (13)  >  GUIX (14)  >  shell (16)  >  bg job (17)
```

GUIX is event-driven and sleeps on its event queue when there is no input or animation, so sitting above the shell does not starve the console. The input thread (13) is just above GUIX (14) so pen events are queued before GUIX wakes. These are **ThreadX thread priorities**, unrelated to the SysTick exception priority (14).

## Display driver (565rgb + double buffer + DMA2D)

`port/guix/guix_display.c`. `_gx_display_driver_565rgb_setup()` lays down GUIX's software 565rgb driver, then the following are overridden.

### Tear-free double buffer (buffer toggle)

The GUIX canvas memory is bound to the **LTDC back buffer** (`ltdc_back_buffer()`). Once GUIX has composited a frame's dirty region into the back buffer, `gx_display_driver_buffer_toggle` is called **once**. There it:

1. presents tear-free via `ltdc_gui_flip()` (SRCR.VBR confirmed, #53);
2. repoints the canvas memory at the new back buffer;
3. copies the dirty rectangle forward — **new front (just presented) → new back** — with a DMA2D M2M blit.

Both frame buffers then stay identical, so the next frame GUIX again only repaints the changed region on top of correct prior content. `ltdc_init()` clears both buffers to black, so the invariant holds from the first frame.

!!! note "Assumes a single visible canvas / partial frame buffer disabled"
    This toggle assumes "one toggle per accumulated-dirty frame". It holds when **`GX_ENABLE_CANVAS_PARTIAL_FRAME_BUFFER` is left undefined** (default off, in `gx_user.h`) and the UI uses only a **single managed + visible full-size canvas**.

### DMA2D acceleration scope

GUIX's software 565rgb driver function pointers are overridden with DMA2D for the paths it can accelerate (each override **guards the supported cases and falls back to the saved software routine** otherwise — software is the correctness baseline):

| GUIX driver | DMA2D | Accelerated when / fallback |
|---|---|---|
| `horizontal_line_draw` (solid fill) | **R2M** | always; native RGB565 colour expanded to ARGB8888 (F7 HAL R2M quirk, #53). Partial brush alpha → software |
| `pixelmap_draw` (image) | **M2M** | uncompressed, opaque, non-transparent **RGB565** with a DMA2D-coherent source (SDRAM/Flash); else software |
| `pixelmap_blend` | **M2M_BLEND** | RGB565 + global alpha (REPLACE) / ARGB4444 per-pixel×global (COMBINE); 565+alpha-plane, compressed, etc. → software |
| `canvas_copy` / `canvas_blend` | **M2M / M2M_BLEND** | multi-canvas compositing (not exercised by this single-canvas UI); a cacheable-SRAM canvas → software |
| buffer-toggle copy-forward | **M2M** | the double buffer (above) |
| `block_move` (scroll) | — | **always software**. Intra-canvas moves inherently overlap source/destination, and DMA2D M2M is not a memmove, so it cannot be safely accelerated |
| glyphs (text), lines | — | software (GUIX 565rgb) |

`gx_draw_context_pitch` is in **pixels** (= canvas width 480; distinct from the byte value `gx_display_driver_row_pitch_get` returns). `pixelmap_draw` is also the path #56 uses to blit camera frames (RGB565 in SDRAM). For verification, a small RGB565 colour-bar image is generated into SDRAM at startup and shown via a `GX_ICON`, golden-verifying the pixelmap_draw DMA2D M2M path on hardware.

### Cache coherency

The destination frame buffer lives in the MPU non-cacheable SDRAM region (#40), so CPU glyph rendering, DMA2D and the LTDC read DMA are coherent by construction — no cache maintenance. Font/colour tables are flash-resident `const` (read-only DMA2D sources need no clean). If an SRAM-resident pixelmap ever becomes a DMA2D source, clean it with a 32-byte-aligned `SCB_CleanDCache_by_Addr()` before the transfer, or fall back to software.

### Sharing the DMA2D engine

DMA2D is a single engine used by both the `lcd` command (`ltdc_display.c`) and GUIX. All GUIX DMA2D work runs under `ltdc_lock_frame()`, serialized on the same recursive `ltdc_lock` as the `lcd` path. While GUIX runs, the ownership interlock below also disables `lcd` drawing, so the two never fight for the screen.

!!! note "Polled completion → the calling thread spins (and why #59 lowered CPU)"
    Each DMA2D op finishes with **`HAL_DMA2D_PollForTransfer`** — a synchronous (polled) wait. The *copy* is done by the DMA2D hardware engine, but the **calling thread busy-waits (spins), consuming its CPU**, until the transfer completes. So removing a DMA2D op removes both its SDRAM traffic *and* the thread's spin time. This is why #59's B2 (dropping the camera **copy-forward**, a 320×240 ≈ 150 KB M2M blit that ran on the **GUIX thread** every frame) **roughly halved the GUIX thread's CPU (≈5.5% → 2.4%, measured via `thread`)** — the GUIX thread went from two polled DMA2D waits per frame (`pixelmap_draw` + copy-forward) to one. The polled model was chosen for simplicity and bounded `ltdc_lock` critical sections; the transfers are short (~tens of µs) and CPU headroom is large (~73% idle), so the spin cost is not a problem in practice. Making the completion **interrupt-driven** (`HAL_DMA2D_Start_IT` + the DMA2D IRQ → a semaphore) would free the waiting thread's CPU (to idle/WFI for power, or to lower-priority work) but does not increase DMA2D concurrency (single engine, serialized on `ltdc_lock`) and adds per-op context-switch/ISR overhead that rivals a short transfer — tracked as a future optimization in **#64**.

## LTDC ownership interlock

`ltdc_lock` only serializes individual accesses; it does not stop a shell `lcd fill`→`ltdc_flip()` from moving `ltdc_front` while the GUIX canvas points at the back buffer (which would leave GUIX drawing into the visible frame). So while GUIX runs it **takes ownership of the display** (`port/ltdc/ltdc_display.c`):

- `ltdc_gui_take(true/false)` sets/clears the flag under `ltdc_lock` (atomic against an in-flight draw).
- While owned, the public draw helpers (`ltdc_fill`, …) and the public `ltdc_flip()` become **no-ops / refusals** under `ltdc_lock`. GUIX presents through a dedicated owner-only `ltdc_gui_flip()`.
- `cmd_lcd`'s drawing subcommands check `ltdc_gui_owns()` up front and return "display owned by gui; run `gui stop` first". The long-running `lcd anim` re-checks each frame and exits.

This closes the foreground, background and in-flight `lcd` cases against GUIX's frame-buffer invariant.

## Input driver (FT5336 → GUIX pen events)

`port/guix/guix_touch.c`. A dedicated thread (prio 13, 1 KB stack) turns the first touch point into GUIX pen events (`GX_EVENT_PEN_DOWN` / `_PEN_DRAG` / `_PEN_UP`) via `gx_system_event_send()`. **#62** made it **EXTI13-wake driven**: with nothing touched it waits on the FT5336 INT wake (`touch_wait_event`) with `TX_WAIT_FOREVER` at **CPU ≈ 0 %**, and only while a finger is down does it poll at ~60 Hz to follow a drag (the controller emits no edges during contact — see [Touch](../hardware/touch.md)). Coordinates are the panel pixels the FT5336 reports directly (no scaling, #54). `gx_system_event_send()` uses a `TX_NO_WAIT` `tx_queue_send`, so a full queue just drops that sample (recovered on the next loop).

Stopping is **cooperative**: the thread checks an `active` flag and parks itself at the top of the loop — outside any I2C transaction. It never uses `tx_thread_suspend()` (which could stop it while it holds/awaits the touch mutex).

## Lifecycle (`gui` command)

!!! warning "#100 made the preview a subscriber of the base capture (this section's ownership/overlay text is rewritten in Phase 3, #102)"
    Epic #99 Phase 1 (#100) means the GUI preview no longer **owns** the DCMI.
    `camera_preview_start()` is gone; the preview attaches via
    `camera_subscribe(&guix_cam_sink, CAM_FMT_RGB565)` as a **subscriber of the base
    capture (`camera stream`)** (detached with `camera_unsubscribe`). Key points:

    - `gui start` opens the window and subscribes the preview but does **not** start
      the base. It attaches immediately if the base is streaming (RGB565), else stays
      enabled + idle until the next `camera stream start`. `gui stop` only
      unsubscribes (it stops **neither the base nor the AI**). While the base is off
      the last frame is **frozen**.
    - `cam_sink_open` adapts to the base's delivered geometry (QQVGA/QVGA): it arms
      `preview_begin` and, if the resolution differs, posts `GX_EVENT_CAMERA_GEOM` so
      the GUIX thread re-syncs the pixmap/widget.
    - **the `gui overlay` command is gone**: the preview draws face bboxes whenever
      `ai stream` is running (gated on `nn_camera_running()`; the FEED mode in the
      "Face-detect overlay" section below is removed).
    - **DCMI overrun auto-recovery moved into the base capture (producer)**; the GUI
      backoff / `GX_EVENT_CAMERA_RESTART` are gone.
    - boot subscribes the preview and starts the base once (a live preview out of the box).

    See [Ownership & state model](../architecture/ownership.md) for the new model
    (this section gets a full rewrite in #102).

GUIX itself is brought up in `port/guix/guix_glue.c`; the camera UI app (widget tree + sink + preview control) is `ui/guix_camera_ui.c`. `gx_system_initialize()` + display/canvas/widget creation + `gx_system_start()` happen **exactly once** (the GUIX system thread and its global objects are never torn down).

**Dependency inversion (#61)**: after creating the display/canvas/root, `guix_first_start()` delegates the widget-tree build to a builder the `ui/` layer registered (`guix_register_app_builder()`, arguments are `void*`). So `port/guix/guix_glue` never includes a `ui/` header, preserving the `port <- ui` direction.

**Started ON at boot (#60) + camera autostart (#61)**: the first `guix_start()` is called from **`tx_application_define()` (before the scheduler)**, not from `gui start`, right after the LTDC/touch bring-up. This is safe — `guix_first_start()` does **only memory setup + ThreadX object creation** with no blocking wait (`gx_system_initialize()` creates the GUIX system thread **`TX_DONT_START`**, `gx_system_start()` just `tx_thread_resume()`s it, and the LTDC mutex is uncontended at boot so it never suspends). The **camera probe is blocking I2C and cannot run in this init context**, so `camera_ui_start()` only brings GUIX up and posts `GX_EVENT_CAMERA_AUTOSTART`; the **probe + first paint run later on the GUIX thread** once scheduling is live (see below). Failure is fail-soft (the shell keeps running and `gui start` can retry).

- `gui start` / boot — `camera_ui_start()`: `guix_start()` (first time: init above + take ownership + `gx_widget_show(root)` + `gx_system_start()` + start the input thread; on a restart it re-takes ownership, resyncs the canvas, re-shows and posts one `GX_EVENT_REDRAW`) → post `GX_EVENT_CAMERA_AUTOSTART` → the GUIX thread starts the preview.
- `gui stop` — `camera_ui_stop()`: stop the preview (stop the stream + detach the sink + bounded drain) → park input → `gx_widget_hide(root)` → blank the screen to black via DMA2D (owner path) → release ownership (the `lcd` drawing path works again).
- `gui info` — state, GUIX system-thread priority, display handle, canvas address.

The UI (`ui/guix_camera_ui.c`) is hand-coded (no GUIX Studio). The colour/font/pixelmap tables are built by hand. It is **two screens** — sibling child windows of the root (the #55 multi-screen show/hide idiom): `preview_screen` (black fill) holding `cam_icon` (a `GX_ICON`, the camera frame native-scale centred, resolution-selectable #69), and `settings_screen` (dark fill) holding the 9 control rows + a Back button, hidden by default (#68). Exactly one is shown at a time. All widgets are static, so GUIX needs no memory allocator.

## Memory / flash

| Region | Increment | Notes |
|--------|-----------|-------|
| FLASH | ~107 KB (170→277 KB, 26.5% of 1 MB) | GUIX core (after gc-sections + binres exclusion) |
| RAM | GUIX thread stack 8 KB + input stack 1 KB + GUIX statics | ~32% of 256 KB |
| SDRAM | canvas: none (reuses the existing LTDC double buffer) | camera preview (#56) adds a +150 KB view buffer (320×240×2 QVGA, sized for the largest selectable preview, #69/#84) |

## Camera live preview (#56/#61)

`ui/guix_camera_ui.c` (relocated and merged from `port/guix/guix_camera.c` in #61). The QVGA RGB565 frames the DCMI streaming producer (#46, `port/camera`) publishes into the `frame_pipeline` are shown live on the GUIX **sole screen** at **native scale (no scaling)**. Since #61 this is the default UI.

Data flow:

1. A **push sink** (`guix_cam_sink`, `FRAME_POLICY_DROP`) is attached to the pipeline by `camera_preview_start()`; its `consume()` runs on the producer thread (prio 10).
2. `consume()` copies the ring slot into a **private view buffer `cam_view_buf` (max QVGA 320×240, non-cacheable `.sdram`; the live copy is the selected resolution, #69/#84) by DMA2D M2M** (`guix_display_copy_rgb565`, under ltdc_lock), then immediately `put`s the slot (synchronous — it never holds a pin across threads). Only on a successful copy does it set a coalesce flag and post `GX_EVENT_CAMERA_FRAME` to the root (`gx_system_event_send`, thread-safe). A failed send leaves the flag clear so the next frame retries (no freeze).
3. The GUIX system thread (prio 14) **root event handler** receives `GX_EVENT_CAMERA_FRAME` and calls `gx_system_dirty_mark(cam_icon)` (dirty marking is GUIX-thread-only — other threads only send events).
4. Redrawing `cam_icon` (a `GX_ICON` on the sole screen at (80,16)) calls `guix_pixelmap_draw`, which **blits the view buffer into the back buffer by DMA2D M2M at native scale**; `guix_buffer_toggle` then presents it tear-free via SRCR.VBR.

The **single view buffer** decouples GUIX redraws (touch, screen change, first show) from the ring slot lifetime. The slot→view and view→back blits are both **serialized DMA2D under ltdc_lock**, so even though the producer (prio 10) can preempt GUIX (prio 14) there is no intra-frame tear (ltdc_lock is TX_INHERIT, which also prevents priority inversion).

**Ownership model** (same shape as `ltdc_gui_take`): while the preview runs, `cam_ext_sink` is set and the public `camera stream start/stop` are refused (`CAM_ERR_STATE`), and `camera res/format/set` return `CAM_ERR_BUSY`. The **async teardown** (DCMI overrun etc.) also detaches and releases `cam_ext_sink`, so a later `frame_pipeline_init` never memsets a still-attached sink. **Escape hatch**: to run those shell commands, `gui stop` tears the UI down (releasing ownership) → run the command → `gui start` resumes (#61).

**Autostart and start/stop race (#61)**: the preview probe/configure is blocking I2C, so `camera_ui_start()` posts `GX_EVENT_CAMERA_AUTOSTART` and the **GUIX thread** runs `camera_preview_start()` in that handler (shared by boot and `gui start`). A volatile-flag protocol guards the GUIX-thread start against a shell-thread `gui stop`:

- the AUTOSTART handler no-ops on `stop_requested || !guix_is_up() || start_in_progress || preview_running` (`stop_requested` is cleared only in `camera_ui_start` before the post; the handler never clears it).
- `camera_ui_stop()` sets `stop_requested=1` then **always** calls `camera_preview_stop()` (which serialises on `cam_lock`, waiting out a probe in progress, and stops only if we own the stream) + a bounded drain.
- if a stop races a start mid-probe, the handler re-checks `stop_requested` after a successful start and rolls back immediately (without latching `preview_running`).

During the probe (~150-250 ms with a camera, ~1 s without) GUIX dispatch stalls, but the LTDC frame buffer was blanked black at boot so it reads as black→live; the iwdg (prio 5) and touch (prio 13) threads run independently, so the watchdog is unaffected.

**Bandwidth**: even at QVGA, the LTDC continuous read + DCMI write + the DMA2D blits per displayed frame all share the SDRAM (16-bit FMC @108 MHz). The average fits the budget, but on hardware monitor `camera stream stats` `dcmi_ovr` / `cam_ring_ovr` / `dma fe/s` and the LTDC underrun and confirm they stay near 0.

!!! note "#59: fewer DMA2D round-trips (FE mitigation)"
    It originally ran DMA2D three times per displayed frame (slot→view, view→back, toggle copy-forward), which contributed to the SDRAM contention and the DCMI DMA FIFO errors (FE).  #59 removed two of them:

    - **B1**: `consume()` **coalesces** the slot→view copy while `cam_redraw_pending` is set (the previous frame is not yet drawn); the slot is always `put`.  It helps exactly when frames outpace the redraw — the contended case.
    - **B2**: the camera rect is fully repainted every frame, so the `guix_buffer_toggle` **copy-forward is eliminated in steady state**.  Consistency is held by a **per-buffer stale flag (`cam_buf_stale[2]`, invariant "false ⟺ that buffer's camera rect == latest view") plus a pre-flip corrective copy**.  Each stale-flag transition stays in the same `ltdc_lock` section as its DMA2D copy, so the producer advancing the view cannot cause a false-negative.  The toggle's B2 paths are gated on `cam_visible` (toggled by preview start/stop; before #61, by screen SHOW/HIDE).  The LCD_CLK was also lowered 9.6→4.8 MHz ([display](../hardware/display.md)).  The figure of merit is `dma fe/s`.

## Camera settings screen (#68)

`ui/guix_camera_ui.c`. The UI is **two full-screen pages**: a clean live `preview_screen` and a static `settings_screen`. **Tapping the live image opens the settings screen; its Back button returns to the preview.** The GUI calls the `port/camera` control API (`camera_set_*`) directly — the **same shared control layer** as the `camera set` shell commands (no duplicated logic).

Putting the settings on their own page (rather than an overlay on top of the live image) keeps both clean: the preview shows only the camera, so the #59 B2 copy-forward never re-stamps the camera rect over a widget; and the settings panel is static and always readable (no live image bleeding through, no compositing race).

- **Image quality (all 9 OV5640 `camera set` controls)**: brightness / contrast / saturation / hue (`[-]`/`[+]` buttons + a numeric value), awb / effect / flip / zoom / night (cycle buttons whose label shows the current value). Applied **live over I2C while the preview owns the camera** (`apply_if_live_locked`; the SDE bus is independent of the DCMI capture path), so unlike `camera res/format` these are not refused with BUSY.
- **Preview resolution (#69)**: a `Resolution` cycle button (160x120 / 320x240, both RGB565 / 15 fps — format and fps stay fixed). Unlike the quality controls, a live re-format is `CAM_ERR_BUSY` (the ring is the live DMA target), so this **stops → re-formats → restarts** the preview: it tears the stream down, waits for it to go idle, resizes the view pixmap + `cam_icon` and re-arms the `guix_display` camera-rect geometry, then restarts at the new resolution (the new image appears on `Back`). Both modes are 4:3 and sit centred on the panel. The selection persists across `gui stop`/`gui start`; the GUI preview boots at **QVGA** (#84; the shell capture/stream path keeps its own QVGA default).

### How the two-page flow works
- `cam_icon` is BORDER_NONE so it takes no touch; a `GX_EVENT_PEN_DOWN` on `preview_screen` reaches its handler → re-read the live values (`controls_sync`, so a `camera set` made meanwhile is reflected) → `guix_display_cam_set_visible(false)` (stop the B2 camera copy-forward so the producer no longer re-stamps the rect) → hide preview / show settings.
- **Back** (`settings_screen` handler, `GX_SIGNAL(ID_BACK, CLICKED)`) hides settings / shows preview → `guix_display_cam_set_visible(preview_running)` (re-arm B2, which marks both LTDC buffers stale so the live image is restored) → `gx_system_dirty_mark(cam_icon)` for an immediate repaint.
- The quality buttons notify the `settings_screen` handler via `GX_SIGNAL(id, GX_EVENT_CLICKED)`; it applies `camera_set_*` and updates the value display. These run on the GUIX system thread, which blocks for the duration of the SDE I2C write (taps are not rapid, so this is acceptable).
- While the settings screen covers the preview, `cam_icon` is hidden, so the `CAMERA_FRAME` handler **clears `cam_redraw_pending = 0` first** and only then skips the dirty-mark (so the producer's coalescing never stalls and live resumes reliably on Back). The producer keeps streaming while settings is up (the view-store DMA2D cost remains; a future optimization could suppress it).
- The initial page reset (preview shown, settings hidden) runs in the **AUTOSTART handler (GUIX thread, after `gx_widget_show(root)`)** so a non-GUIX thread never touches the widget tree; `gx_widget_show(root)` leaves both screens visible, and the reset forces preview-only on both the boot and `gui start` paths.

> **Image quality vs fps (#70):** changing image quality (brightness/contrast/effect/flip/zoom/…) keeps the preview fps — only night mode and resolution changes reprogram the sensor timing, and turning night mode off restores the daylight AEC ceiling so the fps returns to the per-mode target. (Earlier this dropped fps on every quality apply in a dark scene.)

## Face-detect overlay (#83, Epic #80 P4)

`gui overlay on` **draws green face-detection bounding boxes onto the live preview** (`gui overlay off` clears them, default OFF; bare `gui overlay` prints the state + inference stats). The preview frames are fed into BlazeFace-128 (the same inference stack as the `ai` command, see [AI inference](../ai/inference.md)) and the returned face boxes are composited onto the image. Inference runs at ~1.5 fps / priority-18 best-effort while the preview stays at ~15 fps (boxes refresh as each inference completes). The worker's near-continuous bank3 (activation) SDRAM traffic (~93 % CPU) can in principle starve the DCMI DMA and overrun it, but QVGA (the largest preview since #84) has enough bandwidth headroom not to overrun; the overrun auto-recovery below remains as a safety net.

- **Infers while keeping single camera ownership (Approach A)**: `ai stream` owns the camera itself, so it is exclusive with the GUIX preview; the overlay instead **keeps GUIX as the camera owner** and feeds frames from the frame-consume path into inference (`nn_camera`'s external-feed mode `nn_camera_feed_start/feed/feed_abort`). The worker / staging (`.sdram.ai`) / BlazeFace decode / dets are shared with `ai stream` and gated by the single NN session guard: while the overlay runs, `ai bench` / `ai stream start` are refused and `ai stream stop` reports "use `gui overlay off`" (`nn_camera`'s CAMERA / FEED mode split).
- **The box is drawn as part of the frame, on the GUIX thread**: the #59 B2 copy-forward re-stamps the camera rect from `cam_view_buf` on every flip, so a widget composited on top would be overwritten. The box is therefore burned **directly into `cam_view_buf` (under `ltdc_lock`) by `guix_display_cam_overlay_box()`**, from the GUIX thread's `GX_EVENT_CAMERA_FRAME` handler — NOT the camera producer: taking `ltdc_lock` per box on the producer (prio 10) delayed the DCMI DMA re-arm and caused overruns. It then propagates to both LTDC buffers via the same store / pixelmap-draw / corrective-refresh paths as the frame. Coordinates are normalized [0,1] (preprocessing squashes the full frame to 128×128 independently in x/y) and clamped; the next store overwrites the old box (so it clears on overlay-off or when a detection is lost). `cam_view_buf` is bank0 non-cacheable SDRAM, so the CPU writes are coherent with the DMA2D reads — no cache maintenance.
- **Not frame-perfect**: the overlay draws the last completed inference, so a box lags the displayed frame by a few frames (acceptable for a live overlay).
- **Lifecycle + overrun auto-recovery**: overlay on/off and the feed start/abort are serialized on `overlay_lock` (intent `overlay_wanted` vs active `overlay_on`) so concurrent shells and the async camera teardown cannot diverge them. A resolution change (#69) keeps the feed across the stop/restart (BlazeFace input is always 128×128; `preview_reformatting` distinguishes the deliberate teardown). **On a DCMI overrun the preview is auto-restarted** (GUIX-thread `GX_EVENT_CAMERA_RESTART`) and the overlay restored once the NN session frees; an escalating backoff drops the overlay after repeated rapid overruns and finally stops auto-restarting (`gui start` to recover).
- **480×272 was removed (#84)**: the full-panel 480×272 preview both stretched the image (the OV5640 non-uniformly scales its 4:3 sensor field to 16:9, so it came out horizontally stretched) and was bandwidth-heavy — its store (DMA2D bank1→bank0, 261 KB/frame) + DCMI (bank1) + inference (bank3) overran the DCMI, and the F746 bus matrix is round-robin with no per-master SDRAM QoS (RM0385 §2), so priority cannot help. QVGA (320×240) is correctly proportioned (4:3, no stretch) and has ~41 % less bank1 traffic, so the two remaining preview modes (QQVGA/QVGA) neither stretch nor overrun.

## Usage

```text
# power-on shows the live camera preview right away (GUI ON at boot, #60/#61)
# tap the image -> settings page (9 quality controls + Resolution + Back); Back returns to the live preview (#68/#69)
# Resolution cycles 160x120 / 320x240 (stop->reformat->restart; #69)
sh> gui info          # state: running (camera UI active)
sh> lcd fill red      # refused while GUIX runs (display owned by gui)
sh> camera res qvga   # same: CAM_ERR_BUSY while the preview owns the camera
sh> gui stop          # stop the UI + preview and hand the LCD back to `lcd`
sh> lcd info          # after gui stop the lcd/touch/camera test commands work
sh> camera capture    # ditto
sh> gui start         # resume the camera UI (preview comes back)
sh> gui overlay on    # draw face bboxes on the live preview (#83); off clears, bare shows state
```

## Implementation notes

- `gx_draw_context_pitch` is in **pixels** (not bytes). `gx_display_driver_row_pitch_get` returns bytes, but the driver's internal `USHORT*` row stepping is in pixels.
- `GX_RECTANGLE` left/top/right/bottom are all **inclusive**.
- In an error-checking build, `gx_prompt_text_color_set` / `gx_text_button_text_color_set` take **4 arguments** (normal/selected/**disabled**).
- Clean-room implementation. GUIX itself is a read-only submodule and is not edited; the glue lives in `port/guix/`.
