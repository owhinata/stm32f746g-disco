# GUIX (Eclipse ThreadX GUIX integration)

On top of the board's 4.3″ 480×272 LCD (LTDC, #52/#53) and the FT5336 capacitive touch panel (I2C3, #54), this brings up **Eclipse ThreadX GUIX** (the GUI framework). GUIX runs as a ThreadX thread; its 565rgb display driver is bound to the LTDC tear-free double buffer + DMA2D, its input driver to the FT5336, and it shows a basic widget UI (text + button + two-screen transition). **Phase 4** of the LTDC + GUIX epic (#48), issue #55.

- Submodule: `lib/guix` ([eclipse-threadx/guix](https://github.com/eclipse-threadx/guix) v6.5.1, **MIT**). A read-only mirror like `threadx`/`filex`/`levelx`.
- Glue: `port/guix/`; shell command `gui` (`shell/cmds/cmd_gui.c`).
- Started ON at boot (#60): GUIX takes over the LCD and touch from `tx_application_define()` at boot, resident and symmetric with the camera producer. Run `gui stop` to release the display so the `lcd`/`touch` test commands work, and `gui start` to resume.

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

`port/guix/guix_glue.c`. `gx_system_initialize()` + display/canvas/widget creation + `gx_system_start()` happen **exactly once** (the GUIX system thread and its global objects are never torn down).

**Started ON at boot (#60)**: that first `guix_start()` is called from **`tx_application_define()` (before the scheduler starts)**, not from `gui start`, right after the LTDC/touch bring-up. This is safe — `guix_first_start()` does **only memory setup + ThreadX object creation** with no blocking wait (`gx_system_initialize()` creates the GUIX system thread **`TX_DONT_START`**, `gx_system_start()` just `tx_thread_resume()`s it, and the LTDC mutex is uncontended at boot so it never suspends). **The first actual paint runs on the GUIX thread once scheduling starts**, so no DMA2D completion wait (#64) happens in this init context. Failure is fail-soft (the shell keeps running and `gui start` can retry).

- `gui start` — first time: the init above → take ownership → `gx_widget_show(root)` → `gx_system_start()` → start the input thread (already done at boot by #60). On a later restart (after stop): **no re-initialise**; re-take ownership → resync the canvas onto the back buffer → re-show → post one `GX_EVENT_REDRAW` to wake the sleeping GUIX thread and force a full repaint.
- `gui stop` — park input → `gx_widget_hide(root)` → blank the screen to black via DMA2D (the public `ltdc_fill` is a no-op while owned, so the owner path) → release ownership (the `lcd` drawing path works again).
- `gui info` — state, GUIX system-thread priority, display handle, canvas address.

The UI (`port/guix/guix_app.c`) is hand-coded (no GUIX Studio). The colour/font tables are built by hand, and text uses GUIX's built-in `_gx_system_font_8bpp` (compiled from `lib/guix/common/src/gx_system_font_8bpp.c`). Screen 0 (title + "Next" button) and screen 1 ("Back" button) are child windows of the root; a button's `GX_EVENT_CLICKED` is handled by the parent window's event process, which switches screens with `gx_widget_show/hide`. All widgets are static, so GUIX needs no memory allocator.

## Memory / flash

| Region | Increment | Notes |
|--------|-----------|-------|
| FLASH | ~107 KB (170→277 KB, 26.5% of 1 MB) | GUIX core (after gc-sections + binres exclusion) |
| RAM | GUIX thread stack 8 KB + input stack 1 KB + GUIX statics | ~32% of 256 KB |
| SDRAM | canvas: none (reuses the existing LTDC double buffer) | camera preview (#56) adds a +150 KB view buffer (320×240×2) |

## Camera live preview (#56)

`port/guix/guix_camera.c`. The QVGA RGB565 frames the DCMI streaming producer (#46, `port/camera`) publishes into the `frame_pipeline` are shown live on the GUIX screen at **native scale (no scaling)**.

Data flow:

1. A **push sink** (`guix_cam_sink`, `FRAME_POLICY_DROP`) is attached to the pipeline by `camera_preview_start()`; its `consume()` runs on the producer thread (prio 10).
2. `consume()` copies the ring slot into a **private view buffer `cam_view_buf` (320×240, non-cacheable `.sdram`) by DMA2D M2M** (`guix_display_copy_rgb565`, under ltdc_lock), then immediately `put`s the slot (synchronous — it never holds a pin across threads). Only on a successful copy does it set a coalesce flag and post `GX_EVENT_CAMERA_FRAME` to the root (`gx_system_event_send`, thread-safe). A failed send leaves the flag clear so the next frame retries (no freeze).
3. The GUIX system thread (prio 14) **root event handler** receives `GX_EVENT_CAMERA_FRAME` and calls `gx_system_dirty_mark(cam_icon)` (dirty marking is GUIX-thread-only — other threads only send events).
4. Redrawing `cam_icon` (a `GX_ICON` on screen 2 at (80,16)) calls `guix_pixelmap_draw`, which **blits the view buffer into the back buffer by DMA2D M2M at native scale**; `guix_buffer_toggle` then presents it tear-free via SRCR.VBR.

The **single view buffer** decouples GUIX redraws (touch, screen change, first show) from the ring slot lifetime. The slot→view and view→back blits are both **serialized DMA2D under ltdc_lock**, so even though the producer (prio 10) can preempt GUIX (prio 14) there is no intra-frame tear (ltdc_lock is TX_INHERIT, which also prevents priority inversion).

**Ownership model** (same shape as `ltdc_gui_take`): while the preview runs, `cam_ext_sink` is set and the public `camera stream start/stop` are refused (`CAM_ERR_STATE`); conversely `gui camera on` fails while a plain `camera stream` runs. The **async teardown** (DCMI overrun etc.) also detaches and releases `cam_ext_sink`, so a later `frame_pipeline_init` never memsets a still-attached sink. `gui camera off` (or screen 2's Back button) stops the stream (owner-only) then bounded-drains any in-flight `consume()`.

Shell control:

- `gui camera on` — auto `gui start` if the UI is down → start streaming + attach the sink → switch to screen 2 (run on the shell thread, as it does the sensor probe/configure).
- `gui camera off` — return to screen 0 → stop the stream + detach/drain the sink.
- Screen 2's **Back button** also stops and returns (`guix_camera_off` only does a bounded drain, so it is safe to call from the GUIX thread).

**Bandwidth**: even at QVGA, the LTDC continuous read + DCMI write + the DMA2D blits per displayed frame all share the SDRAM (16-bit FMC @108 MHz). The average fits the budget, but on hardware monitor `camera stream stats` `dcmi_ovr` / `cam_ring_ovr` / `dma fe/s` and the LTDC underrun and confirm they stay near 0.

!!! note "#59: fewer DMA2D round-trips (FE mitigation)"
    It originally ran DMA2D three times per displayed frame (slot→view, view→back, toggle copy-forward), which contributed to the SDRAM contention and the DCMI DMA FIFO errors (FE).  #59 removed two of them:

    - **B1**: `consume()` **coalesces** the slot→view copy while `cam_redraw_pending` is set (the previous frame is not yet drawn); the slot is always `put`.  It helps exactly when frames outpace the redraw — the contended case.
    - **B2**: the camera rect is fully repainted every frame, so the `guix_buffer_toggle` **copy-forward is eliminated in steady state**.  Consistency is held by a **per-buffer stale flag (`cam_buf_stale[2]`, invariant "false ⟺ that buffer's camera rect == latest view") plus a pre-flip corrective copy**.  Each stale-flag transition stays in the same `ltdc_lock` section as its DMA2D copy, so the producer advancing the view cannot cause a false-negative.  The toggle's B2 paths are gated on `cam_visible` (toggled by SHOW/HIDE) so camera pixels are never stamped onto another screen.  The LCD_CLK was also lowered 9.6→4.8 MHz ([display](../hardware/display.md)).  The figure of merit is `dma fe/s`.

## Usage

```text
sh> gui info          # state: running right from boot (GUI ON at boot, #60)
# tap "Next >" -> screen 2; "< Back" returns.
sh> gui camera on     # live camera preview (320x240 native)
sh> gui camera off    # stop the preview (screen 2's Back button also stops it)
sh> lcd fill red      # refused while GUIX runs (display owned by gui)
sh> gui stop          # blank the screen and hand the LCD back to `lcd`
sh> lcd info          # after gui stop the LCD/touch test commands work
sh> gui start         # resume the GUIX UI (takes over LCD + touch again)
```

## Implementation notes

- `gx_draw_context_pitch` is in **pixels** (not bytes). `gx_display_driver_row_pitch_get` returns bytes, but the driver's internal `USHORT*` row stepping is in pixels.
- `GX_RECTANGLE` left/top/right/bottom are all **inclusive**.
- In an error-checking build, `gx_prompt_text_color_set` / `gx_text_button_text_color_set` take **4 arguments** (normal/selected/**disabled**).
- Clean-room implementation. GUIX itself is a read-only submodule and is not edited; the glue lives in `port/guix/`.
