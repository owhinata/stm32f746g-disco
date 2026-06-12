<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 ThreadX Shell Project
-->

# Layering and Dependency Direction

The firmware sources are split into layers so that dependencies flow one way (a
lower layer never knows about a higher one). Issue #43 introduced the
freestanding common service layer `svc/`, removing the back-flow where the
driver layer `port/` used to include the higher `src/` / `include/` headers.

```
HAL / CMSIS / ThreadX   <-   svc/   <-   port/   <-   shell/ , src/
        (lib/)            (common)    (drivers)        (app)
```

The arrow means "the left is depended on by the right". `svc/` does not know
about `port/`, and `port/` does not know about `shell/` or `src/`.

## Role of each directory

| Directory | Role | May depend on |
|---|---|---|
| `lib/` | Upstream submodules (ST HAL / CMSIS / ThreadX / FileX / LevelX / OV5640 / CoreMark). Never edited. | — |
| `svc/` | Freestanding common services: `fmt` (clean-room printf), `log` (DTCM RAM log), `timebase` (TIM2 free-run + `udelay`). | HAL / CMSIS |
| `port/` | Peripheral driver glue (qspi / sd / sdram / camera / filex / levelx / threadx). | `svc/`, HAL / CMSIS / ThreadX |
| `shell/` | The CLI shell (core / backend / cmds). | `svc/`, `port/`, ThreadX |
| `src/` | Application bring-up (`main` / `bsp` / `fault` / `iwdg`). | `svc/`, `port/`, `shell/`, HAL |
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

## CMake wiring

`svc/` is linked into the final executable as the OBJECT library `svc_obj`
(`svc/fmt.c` / `svc/log.c` / `svc/timebase.c`). The `svc/` include path is on
the `bsp_iface` INTERFACE, so every target can reach `fmt.h` / `log.h` /
`timebase.h`.
