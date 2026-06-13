# GUIX (Eclipse ThreadX GUIX integration)

On top of the board's 4.3″ 480×272 LCD (LTDC, #52/#53) and the FT5336 capacitive touch panel (I2C3, #54), this brings up **Eclipse ThreadX GUIX** (the GUI framework). GUIX runs as a ThreadX thread; its 565rgb display driver is bound to the LTDC tear-free double buffer + DMA2D, its input driver to the FT5336, and it shows a basic widget UI (text + button + two-screen transition). **Phase 4** of the LTDC + GUIX epic (#48), issue #55.

- Submodule: `lib/guix` ([eclipse-threadx/guix](https://github.com/eclipse-threadx/guix) v6.5.1, **MIT**). A read-only mirror like `threadx`/`filex`/`levelx`.
- Glue: `port/guix/`; shell command `gui` (`shell/cmds/cmd_gui.c`).
- Lazy start: GUIX only takes over the LCD and touch on `gui start`. Until then the `lcd`/`touch` test commands work as before.

## Configuration

| Item | Value | Notes |
|------|-------|-------|
| GUIX | Eclipse ThreadX GUIX **v6.5.1** (MIT) | `lib/guix`, `common/src/*.c` globbed |
| Port | Cortex-M7 / GNU (`ports/cortex_m7/gnu/inc/gx_port.h`) | **no asm** (binds to ThreadX) |
| Config | `port/guix/gx_user.h` (`GX_INCLUDE_USER_DEFINE_FILE`) | same idiom as FX/LX |
| Display | 480×272 **RGB565**, 1 layer, LTDC double buffer | canvas = LTDC back buffer |
| Acceleration | **DMA2D**: solid fills (R2M) + double-buffer copy-forward (M2M) | RM0385 §9 |
| Input | FT5336 (I2C3) polling → GUIX pen events | `port/guix/guix_touch.c` |
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

## LTDC ownership interlock

`ltdc_lock` only serializes individual accesses; it does not stop a shell `lcd fill`→`ltdc_flip()` from moving `ltdc_front` while the GUIX canvas points at the back buffer (which would leave GUIX drawing into the visible frame). So while GUIX runs it **takes ownership of the display** (`port/ltdc/ltdc_display.c`):

- `ltdc_gui_take(true/false)` sets/clears the flag under `ltdc_lock` (atomic against an in-flight draw).
- While owned, the public draw helpers (`ltdc_fill`, …) and the public `ltdc_flip()` become **no-ops / refusals** under `ltdc_lock`. GUIX presents through a dedicated owner-only `ltdc_gui_flip()`.
- `cmd_lcd`'s drawing subcommands check `ltdc_gui_owns()` up front and return "display owned by gui; run `gui stop` first". The long-running `lcd anim` re-checks each frame and exits.

This closes the foreground, background and in-flight `lcd` cases against GUIX's frame-buffer invariant.

## Input driver (FT5336 → GUIX pen events)

`port/guix/guix_touch.c`. A dedicated thread (prio 13, 1 KB stack) polls `touch_read()` (#54) at ~60 Hz and turns the first touch point into GUIX pen events (`GX_EVENT_PEN_DOWN` / `_PEN_DRAG` / `_PEN_UP`) via `gx_system_event_send()`. Coordinates are the panel pixels the FT5336 reports directly (no scaling, #54). `gx_system_event_send()` uses a `TX_NO_WAIT` `tx_queue_send`, so a full queue just drops that sample (recovered on the next poll).

Stopping is **cooperative**: the thread checks an `active` flag and parks itself at the top of the loop — outside any I2C transaction. It never uses `tx_thread_suspend()` (which could stop it while it holds/awaits the touch mutex).

## Lifecycle (`gui` command)

`port/guix/guix_glue.c`. `gx_system_initialize()` + display/canvas/widget creation + `gx_system_start()` happen **exactly once** (the GUIX system thread and its global objects are never torn down).

- `gui start` — first time: the init above → take ownership → `gx_widget_show(root)` → `gx_system_start()` → start the input thread. On a later restart (after stop): **no re-initialise**; re-take ownership → resync the canvas onto the back buffer → re-show → post one `GX_EVENT_REDRAW` to wake the sleeping GUIX thread and force a full repaint.
- `gui stop` — park input → `gx_widget_hide(root)` → blank the screen to black via DMA2D (the public `ltdc_fill` is a no-op while owned, so the owner path) → release ownership (the `lcd` drawing path works again).
- `gui info` — state, GUIX system-thread priority, display handle, canvas address.

The UI (`port/guix/guix_app.c`) is hand-coded (no GUIX Studio). The colour/font tables are built by hand, and text uses GUIX's built-in `_gx_system_font_8bpp` (compiled from `lib/guix/common/src/gx_system_font_8bpp.c`). Screen 0 (title + "Next" button) and screen 1 ("Back" button) are child windows of the root; a button's `GX_EVENT_CLICKED` is handled by the parent window's event process, which switches screens with `gx_widget_show/hide`. All widgets are static, so GUIX needs no memory allocator.

## Memory / flash

| Region | Increment | Notes |
|--------|-----------|-------|
| FLASH | ~107 KB (170→277 KB, 26.5% of 1 MB) | GUIX core (after gc-sections + binres exclusion) |
| RAM | GUIX thread stack 8 KB + input stack 1 KB + GUIX statics | ~32% of 256 KB |
| SDRAM | none | the canvas reuses the existing LTDC double buffer |

## Usage

```text
sh> lcd info          # right after boot the LCD test commands work as before
sh> gui start         # show the GUIX UI (takes over LCD + touch)
sh> gui info          # status
# tap "Next >" -> screen 2; "< Back" returns.
sh> lcd fill red      # refused while GUIX runs (display owned by gui)
sh> gui stop          # blank the screen and hand the LCD back to `lcd`
```

## Implementation notes

- `gx_draw_context_pitch` is in **pixels** (not bytes). `gx_display_driver_row_pitch_get` returns bytes, but the driver's internal `USHORT*` row stepping is in pixels.
- `GX_RECTANGLE` left/top/right/bottom are all **inclusive**.
- In an error-checking build, `gx_prompt_text_color_set` / `gx_text_button_text_color_set` take **4 arguments** (normal/selected/**disabled**).
- Clean-room implementation. GUIX itself is a read-only submodule and is not edited; the glue lives in `port/guix/`.
