# On-device NN inference

A backend-agnostic `nn` abstraction layer plus an `ai` shell command for running
**neural-network inference** on the STM32F746G-DISCO (issue #81 / Epic #80).  The
first use case is **camera face detection** (OV5640/DCMI → inference).

!!! note "Phase status"
    The P1 **foundation (null backend)** is implemented.  The X-CUBE-AI
    (`stedgeai`) backend + BlazeFace decode (#6/#8), SD model loading (P2), the
    TFLM backend (P3), and the GUIX overlay (P4) are follow-ups.

## Design

- A **backend-agnostic `nn` abstraction** (tensor-in / tensor-out) makes the
  runtime swappable.  Exactly one backend is compiled in per build
  (`CONFIG_NN_BACKEND`):
  - `null` — no runtime (default).  The firmware **always builds** without the
    stedgeai toolchain or a model; a stub with BlazeFace-shaped I/O exercises the
    full plumbing end to end.
  - `stedgeai` — X-CUBE-AI / ST Edge AI Core (follow-up).  Links the generated C
    plus the pre-compiled static library `libNetworkRuntime.a`.
  - `tflm` — LiteRT / TensorFlow Lite Micro (follow-up).
- **Model-specific pre/post-processing** (BlazeFace anchor decode + NMS, etc.)
  lives *above* the `nn` layer (`port/nn/models/`); the generic `nn` layer stays
  model-agnostic.
- **Both backends consume the same int8 `.tflite`**, so the model is portable and
  the runtime choice is reversible.

## Layering

```
HAL/CMSIS/ThreadX  <-  svc  <-  port/nn  <-  ui  <-  shell/src
                                  |
             nn.h (API) / nn_backend.h (vtable)
             nn.c (dispatch + DWT timing)
             nn_null.c | nn_stedgeai.c | nn_tflm.c   (one is selected)
             nn_camera.c (camera -> inference glue)
             models/blazeface.c (model-specific post-processing)
```

| File | Role |
|---|---|
| `port/nn/nn.h` | `nn_tensor` (with int8 quant params) / `nn_model` / `nn_init`, `nn_model_open`, `nn_input`, `nn_output`, `nn_run`, `nn_last_cycles` |
| `port/nn/nn_backend.h` | Vtable each backend implements (`nn_backend_vt_selected`) |
| `port/nn/nn.c` | Public API dispatch + **DWT CYCCNT** inference-latency timing |
| `port/nn/nn_null.c` | Null backend (BlazeFace-shaped stub) |
| `port/nn/nn_camera.{c,h}` | Live camera → inference sink + worker |

## Memory layout

- **`.sdram.ai` arena** — FMC internal **bank3** (`0xC0600000`–`0xC07FFFFF`, 2 MB).
  Its own bank, separate from the DMA banks (bank0 LTDC / bank1 camera DMA /
  bank2 ETH DMA), so it can later be **remapped cacheable via an MPU region**
  without touching the non-cacheable DMA banks (activations are CPU-only, hence
  coherent).
- **Weights** — Flash `.rodata` in P1.  **Performance note:** this linker places
  Flash at `0x08000000` = the **AXIM** side.  The ART accelerator (RM0385 §3.3.2)
  serves the **ITCM interface** flash path; AXIM `.rodata` weights are a
  **D-cache + Flash wait-state (7 WS at 216 MHz)** matter, not ART/I-cache.
  Measure with `ai bench` rather than assuming.

The linker (`ldscript/STM32F746NGHx_FLASH.ld`) guards placement with three
ASSERTs: `_ssdram_ai == bank3 start` / `.sdram.ai <= 2 MB` / `.sdram.eth` stays
within bank2.

## Camera → inference pipeline (`nn_camera.c`)

```
DCMI producer (prio 10)          nn worker (prio 18, best-effort)
      |  RGB565 frame                    ^
      v                                   | READY stage
  consume():                              |
    nearest-neighbour resize + RGB565->int8  --> stage[0/1] --(copy)--> model input --> nn_run()
    camera_frame_put() immediately
```

- **Synchronous copy sink**: `consume()` runs in the camera producer thread,
  resizes + converts, then `camera_frame_put()`s immediately, so the pipeline
  in-flight count is always 0 and the camera's async teardown stays correct (same
  shape as the `nx_mjpeg.c` eth_sink).
- **NN-input ownership** (important): the pipeline pin/put only protects the
  **camera slot** lifetime, not the NN input.  To prevent an overwrite race, two
  staging buffers carry a `FREE → FILLING → READY → RUNNING → FREE` state machine
  guarded by a short `TX_MUTEX`; the sink never writes a buffer the worker is
  using (`RUNNING`).  No free buffer → **drop**.
- **Worker priority = 18 (fully best-effort)**: never starves camera(10)/net(12)/
  GUIX(14)/ETH-link(15)/CLI(16)/BG(17).  Inference is monolithic (no mid-run
  yield), so placing it below the CLI (16) is what guarantees `ai stream stop`
  reaches us.

## `ai` shell commands

| Command | Description |
|---|---|
| `ai info` | backend / model name / I/O tensor shape, dtype, quant / arena size |
| `ai bench [n]` | run inference n times on a fixed input; min/avg/max latency (µs, DWT) + throughput |
| `ai run` | single-shot inference on one camera frame; latency + detections |
| `ai stream start [qqvga\|qvga\|480x272]` | start live continuous inference (default QVGA) |
| `ai stream stop` | stop |
| `ai stream stats` | inference rate / latency / drops / detections |

**Latency** is measured with the Cortex-M7 DWT CYCCNT (RM0385 §40.10/§40.13/
§40.14.2).  On the M7 the DWT CoreSight software lock must be cleared by writing
`DWT->LAR = 0xC5ACCE55` (the CoreSight lock key), same as `cmd_membench.c` (#57).
The counter only freezes in WFI; inference is CPU-bound and never sleeps, so the
elapsed count is the real latency.

## Build configuration

```bash
# Default (null backend; always builds without stedgeai)
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
cmake --build build

# X-CUBE-AI backend (follow-up #6; needs a stedgeai install + generated model)
# cmake -B build ... -DCONFIG_NN_BACKEND=stedgeai -DSTEDGEAI_ROOT=/path/to/stedgeai
```

!!! warning "Licensing"
    This repository is public.  The ST-SLA `libNetworkRuntime.a`, generated
    weights, and ST Model Zoo model binaries are **not committed** (`.gitignore`
    + `port/nn/generated/`).

## Roadmap (Epic #80)

| Phase | Scope |
|---|---|
| **P1** | `nn` abstraction + X-CUBE-AI backend + `ai` command + BlazeFace-128 first-light |
| P2 | Load the model/weights from the SD card (FileX → `.sdram.ai`) |
| P3 | TFLM backend behind the same API; comparative bench |
| P4 | GUIX overlay (live preview + face bbox) |
| P5 | X-CUBE-AI relocatable network (swap the whole model from SD) |
