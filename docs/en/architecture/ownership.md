<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 ThreadX Shell Project
-->

# Ownership & state model (display / DCMI / touch)

Several subsystems share three pieces of hardware that only **one** user can
drive at a time: the **LTDC display**, the **DCMI / camera** capture path, and
the **FT5336 touch** controller on I2C3. The shell, the `lcd`/`camera`/`touch`
commands, and the GUIX camera UI all want them. This page is the single
reference for **who owns what in each state, and which shell commands are
allowed** — the model behind the cross-subsystem guards in `shell/cmds/cmd_*.c`.

## Shared resources and their owners

| Resource | Owner flag (where) | Taken by | Released by |
|---|---|---|---|
| **LTDC display** (draw + scanout) | `ltdc_gui_owned` (`port/ltdc/ltdc_display.c`) | `gui start` → `ltdc_gui_take(true)` | `gui stop` |
| **DCMI / camera** (single capture path) | `cam_stream_active` + `cam_ext_sink` (`port/camera/camera.c`) | shell `camera stream start` **or** GUIX live preview | `camera stream stop` / `gui stop` |
| **FT5336 touch** (I2C3) | *no flag* — `guix_is_up()` is the proxy | GUIX touch thread (`port/guix/guix_touch.c`) while `gui` runs | `gui stop` |

Storage media (`sd`/`fs`/`qspi`) has its own `fs_raw_begin`/`fs_raw_end` slot
(see [Filesystem](../rtos/filesystem.md)); it is not a display/input concern and
is omitted here.

## States

Three mutually-affecting **modes** decide ownership. Camera **power** (on/off) is
an orthogonal axis — `camera stream`/preview force it on, and it can only be
cut from `SHELL`.

```
                       camera stream start
   ┌────────────────────────────────────────────────┐
   │                                                 ▼
┌──────────────┐                            ┌────────────────────┐
│    SHELL      │     camera stream stop     │    CAM-STREAM      │
│   (idle)      │ ◀──────────────────────────│  shell owns DCMI   │
│               │                            └────────────────────┘
│ display free  │
│ DCMI idle     │          gui start         ┌────────────────────┐
│ touch free    │ ─────────────────────────▶ │       GUI          │
│ camera power  │                            │ owns display +     │
│   on/off      │ ◀───────────────────────── │ DCMI (preview) +   │
└──────────────┘          gui stop           │ touch (poller)     │
                                             └────────────────────┘

  • CAM-STREAM and GUI are mutually exclusive on the DCMI: only one capture
    path exists, so `gui start` needs a free camera and `camera stream start`
    is refused while GUIX preview owns it.
  • Camera power is orthogonal: on/off only from SHELL; under STREAM/GUI the
    camera is always on (off / probe / on / res / format / fps / timing-set all
    return BUSY).
```

## What each command can do per state

`OK` = succeeds · `✗` = refused (reason) · `read-only` = allowed, reads existing
state/frame only.

| Command | SHELL (idle) | CAM-STREAM | GUI running |
|---|---|---|---|
| `lcd` draw (`fill`/`bar`/`grad`/`clear`/`anim`/`blit`) | OK | OK *(SDRAM contention)* | ✗ `owned by gui; gui stop first` |
| `lcd on` / `lcd off` | OK | OK | backlight only *(scanout stays — gui owns)* |
| `lcd info` | OK | OK | OK |
| `camera probe` / `on` / `off` | OK | ✗ BUSY | ✗ BUSY |
| `camera res` / `format` / `fps` / `set` *(timing)* | OK | ✗ BUSY | ✗ BUSY |
| `camera stream start` | OK → CAM-STREAM | *(already running)* | ✗ BUSY *(preview owns DCMI)* |
| `camera stream stop` | ✗ not streaming | OK → SHELL | ✗ BUSY *(cannot stop gui's preview)* |
| `camera stream stats` | read-only | read-only | read-only *(preview stats)* |
| `camera capture` | OK | ✗ BUSY | ✗ BUSY |
| `camera info` / `save` / `send` | OK | read-only | read-only |
| `gui start` | OK → GUI | ✗ camera busy | *(already running)* |
| `gui stop` | ✗ not running | ✗ not running | OK → SHELL |
| `touch probe` / `touch read` | OK | OK | ✗ `owned by gui; gui stop first` *(#73)* |
| `touch info` | OK | OK | OK |
| `sd` / `fs` / `qspi` | OK | OK | OK |

## Interlock primitives (where the guards live)

- **Display** — `ltdc_gui_owns()` gates every public draw/flip
  (`ltdc_fill`/`ltdc_flip`/…), and the shell `lcd` draw commands refuse up-front
  via `lcd_can_draw()` (`shell/cmds/cmd_lcd.c`). GUIX presents through the
  owner-only `ltdc_gui_flip()`. `ltdc_set_scanout()` is refused while GUIX owns
  the display, so `lcd on`/`off` toggle only the backlight in that state.
- **DCMI / camera** — `camera_stream_start`/`stop`, `camera_capture`,
  `camera_set_format`/`fps`, `camera_power_off`, `camera_probe` all return
  `CAM_ERR_BUSY` (shown as `busy (streaming or preview active)`) when
  `cam_stream_active` or `cam_ext_sink` is set. A GUIX preview is itself a
  stream, so it sets `cam_stream_active`. Read-only paths (`info`, `save`,
  `send`, `stream stats`) only take the camera lock and are always allowed.
- **NET-MJPEG** (#49 P5) — `net mjpeg start` is another `cam_ext_sink` owner
  (owning the DCMI in JPEG), behaving like the CAM-STREAM column.
  `camera_mjpeg_start` goes through the same ownership gate (via
  `stream_start_locked`), so it returns `CAM_ERR_BUSY` if a GUIX preview /
  `camera stream` owns the DCMI, and while MJPEG runs `gui start` / `camera
  stream start` / `camera set` are BUSY. Exclusion falls out of the single-owner
  model with no extra state (the `port/netxduo/nx_mjpeg.c` eth_sink is a
  synchronous copy sink -- in-flight 0 -- so the producer's async teardown stays
  safe).
- **Touch** — there is no touch ownership flag (`guix_is_up()` is the proxy).
  Two things protect the shell: (1) `touch_read()` (`port/touch/touch.c`) drops
  the FT5336 all-ones "not touched" sentinel — an out-of-panel `0xFFF/0xFFF`
  point that an idle or just-released controller reports with a *nonzero* status
  count (the same point the GUIX driver filters) — so a poll never prints the
  phantom `P15 x=4095 y=4095 event=3`; and (2) the bus-touching `touch
  probe`/`read` refuse while `guix_is_up()` (`shell/cmds/cmd_touch.c`, #73),
  because a shell poll concurrent with the GUIX touch thread would be a second
  unsynchronised I2C3 master (`touch_lock` serialises only a single
  transaction). `touch info` does no I/O and stays allowed.

## Notes & caveats

- **`lcd on`/`off` under GUI** toggles the backlight but leaves scanout running
  (GUIX owns it) — deliberately allowed (blanking the panel is harmless to the
  UI). Everything *visible* still belongs to GUIX until `gui stop`.
- **Read-only commands** (`*info`, `camera save`/`send`, `camera stream stats`)
  work in every state; they never re-arm hardware. During an external-sink stream
  (MJPEG / GUIX preview), though, `camera save`/`send` return the **last
  non-external-sink frame** (or `CAM_ERR_NO_FRAME` if none) -- see the JPEG
  streaming section in [Camera](../hardware/camera.md) (#82).
- See [Camera](../hardware/camera.md), [Display](../hardware/display.md),
  [Touch](../hardware/touch.md) and [GUIX](../rtos/guix.md) for each
  subsystem's details.
