<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 ThreadX Shell Project
-->

# Layering and Dependency Direction

The firmware sources are split into layers so that dependencies flow one way (a
lower layer never knows about a higher one). Issue #43 introduced the
freestanding common service layer `svc/`, removing the back-flow where the
driver layer `port/` used to include the higher `src/` / `include/` headers.
Issue #61 added the presentation layer `ui/`, splitting the GUIX presentation
(the camera UI app) out of `port/`.

```
HAL / CMSIS / ThreadX   <-   svc/   <-   port/   <-   ui/   <-   shell/ , src/
        (lib/)            (common)    (drivers)     (UI)         (app)
```

The arrow means "the left is depended on by the right". `svc/` does not know
about `port/`; `port/` does not know about `ui/`, `shell/` or `src/`; and `ui/`
does not know about `shell/` or `src/`.

## Role of each directory

| Directory | Role | May depend on |
|---|---|---|
| `lib/` | Upstream submodules (ST HAL / CMSIS / ThreadX / FileX / LevelX / OV5640 / CoreMark). Never edited. | — |
| `svc/` | Freestanding common services: `fmt` (clean-room printf), `log` (DTCM RAM log), `timebase` (TIM2 free-run + `udelay`). | HAL / CMSIS |
| `port/` | Peripheral driver glue (qspi / sd / sdram / camera / ltdc / touch / guix / filex / levelx / threadx). | `svc/`, HAL / CMSIS / ThreadX / GUIX |
| `ui/` | GUIX presentation (the camera UI app: widget tree + frame sink + preview control, `guix_camera_ui`). | `svc/`, `port/`, GUIX / ThreadX |
| `shell/` | The CLI shell (core / backend / cmds). | `svc/`, `port/`, `ui/`, ThreadX |
| `src/` | Application bring-up (`main` / `bsp` / `fault` / `iwdg`). | `svc/`, `port/`, `ui/`, `shell/`, HAL |
| `include/` | Public headers of the app layer (`bsp.h` / `main.h` / `iwdg.h`). | — |

## The svc/ layer (issue #43)

`svc/` holds only common functionality that depends on nothing above HAL/CMSIS.
This lets `port/` drivers take `log` and `udelay` from `svc/` instead of
reaching up into `shell/` or `src/` headers.

- `svc/fmt.{h,c}` — a minimal printf formatter whose sink is a putc callback;
  shared by the shell output API, the RAM log and the fault dump. Depends only
  on `<stdarg.h>` / `<stddef.h>`.
- `svc/log.{h,c}` — a reset-persistent ring buffer in the DTCM `.log_noinit`
  section, replayed with `dmesg`. See [Logging](../rtos/logging.md).
- `svc/timebase.{h,c}` — starts TIM2 as a 108 MHz free-running 32-bit counter
  (for the ThreadX execution profile, issue #19) and serves as the source for
  the `udelay()` busy-wait.

## The ui/ layer (issue #61)

`ui/` holds only the GUIX presentation. On top of `port/guix` (display driver /
input / lifecycle) and `port/camera`, it builds the screens and the user
interaction.

- `ui/guix_camera_ui.{h,c}` — the camera live-preview GUIX app. It bundles the
  hand-coded widget tree (a single camera screen + a `GX_ICON`), the
  `frame_pipeline` push sink, and the preview lifecycle (`camera_ui_init` /
  `start` / `stop`) into one module. It merges and relocates the former #55
  `guix_app` (demo UI) and #56 `guix_camera` (sink + control).

**Dependency inversion**: `port/guix/guix_glue` brings GUIX itself up, but
building the widget tree is the `ui/` layer's job. To avoid a `port -> ui`
back-flow, `ui/` registers its widget builder via `guix_register_app_builder()`
and `guix_glue` calls the registered function pointer (arguments are `void*`), so
`guix_glue` never includes a `ui/` header. `guix_display` / `guix_touch` /
`guix_glue` / `gx_user.h` stay in `port/guix/` as driver/integration/config (in
particular the DMA2D B2 copy-forward that is tightly coupled to the LTDC
internals).

## CMake wiring

`svc/` is linked into the final executable as the OBJECT library `svc_obj`
(`svc/fmt.c` / `svc/log.c` / `svc/timebase.c`). The `svc/` include path is on
the `bsp_iface` INTERFACE, so every target can reach `fmt.h` / `log.h` /
`timebase.h`.

`ui/guix_camera_ui.c` is listed directly on the `threadx` target like the
`port/` glue (no OBJECT library), with `ui/` added to the include path.
