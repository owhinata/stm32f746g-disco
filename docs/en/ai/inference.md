# On-device NN inference

A backend-agnostic `nn` abstraction layer plus an `ai` shell command for running
**neural-network inference** on the STM32F746G-DISCO (issue #81 / Epic #80).  The
first use case is **camera face detection** (OV5640/DCMI → inference).

!!! note "Phase status"
    **P1 complete** — null-backend foundation + X-CUBE-AI (`stedgeai`) backend +
    BlazeFace-128 face detection running on hardware (`ai run`/`ai stream` print
    face boxes, ~1.5 fps).  **P4 complete** — face bboxes drawn on the GUIX live
    preview (`gui overlay on`, #83).  **P3 complete** — TFLM (tflite-micro) backend.
    After the M1 spike (#86) proved C++ viability, **M2 (#88) ported BlazeFace-front
    128 int8 for real**: the same `.tflite` runs on both stedgeai and TFLM, and
    `ai info`/`ai bench`/`ai stream`/`gui overlay` all go through the TFLM runtime.
    With **CMSIS-NN optimized kernels** it runs at ~622 ms/inference, edging out
    stedgeai (685 ms).  **P2 complete** — a `.tflite` on the microSD card is
    **loaded at runtime into `.sdram.ai`** and run (`ai model load <sd-path>`, #89);
    a common-vision op superset lets any small int8 model be swapped in.
    **P5 complete** — the X-CUBE-AI **relocatable network** loads a `network_rel.bin`
    from SD and runs it **XIP** (`CONFIG_NN_BACKEND=stedgeai_reloc`,
    `ai model load network_rel.bin`, #92), giving the same SD swap as P2(tflm) on the
    stedgeai runtime (both backends covered).  See
    [X-CUBE-AI relocatable backend](#x-cube-ai-relocatable-backend-p5-92).

## Design

- A **backend-agnostic `nn` abstraction** (tensor-in / tensor-out) makes the
  runtime swappable.  Exactly one backend is compiled in per build
  (`CONFIG_NN_BACKEND`):
  - `null` — no runtime (default).  The firmware **always builds** without the
    stedgeai toolchain or a model; a stub with BlazeFace-shaped I/O exercises the
    full plumbing end to end.
  - `stedgeai` — X-CUBE-AI / ST Edge AI Core.  Links the `stedgeai generate`
    output (STAI API) plus the pre-compiled static library
    `NetworkRuntime*_CM7_GCC.a`.  Tensor descriptors are built from the generated
    `STAI_NETWORK_*` macros (model-agnostic, 1-in/1-out to multi-output).
  - `tflm` — LiteRT / TensorFlow Lite Micro (**P3, #86/#88**).  The C++
    `MicroInterpreter` is confined to a separate `tflm` static lib, and C++ is
    enabled only for the `CONFIG_NN_BACKEND=tflm` build (the default null/stedgeai
    builds are unchanged / byte-identical).  Runs **BlazeFace-front 128 int8** with
    **CMSIS-NN optimized kernels** (on by default) at stedgeai-class speed.  See
    [TFLM backend](#tflm-backend-p3-8688).
  - `stedgeai_reloc` — X-CUBE-AI **relocatable network** (**P5, #92**).  Loads the
    position-independent single `network_rel.bin` (PIC code + embedded weights) that
    `stedgeai generate --relocatable` emits from SD and runs it **XIP** via the ST host
    loader + the legacy `ai_rel_network_*` API.  No runtime `.a` is linked (the PIC
    kernels are baked into the `.bin`).  **SD-only** (no Flash-baked builtin model).  See
    [X-CUBE-AI relocatable backend](#x-cube-ai-relocatable-backend-p5-92).
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
| `port/nn/nn_stedgeai.c` | X-CUBE-AI (STAI) backend (`generated/` + ST runtime .a) |
| `port/nn/nn_camera.{c,h}` | Live camera → inference sink + worker |
| `port/nn/models/blazeface.{c,h}` | BlazeFace face-detection decode (anchors + NMS, model-specific) |

## Memory layout

- **`.sdram.ai` arena** — FMC internal **bank3** (`0xC0600000`–`0xC07FFFFF`, 2 MB).
  Its own bank, separate from the DMA banks (bank0 LTDC / bank1 camera DMA /
  bank2 ETH DMA).  **Remapped cacheable (WBWA) by MPU region1 in `src/bsp.c`**
  (bank3 only, overriding region0; higher region number wins on ARMv7-M).  The
  arena is CPU-only (**no DMA into it**), so D-cache needs no maintenance.
  **Non-cacheable was ~20× slower for NN inference**; cacheable cut MNIST from
  **9.5 ms to 2.5 ms** (#6).  Small models whose activations fit DTCM may use it.
    - **bank3 split in two (P5, #92)** — for `stedgeai_reloc`, bank3 (2 MB) is halved.
      The **lower 1 MB** (`0xC0600000`+, `.sdram.ai`) is data-only (activations / staging /
      the XIP RW image) and stays **XN** (region1).  The **upper 1 MB** (`0xC0700000`+,
      `.sdram.ai.model`) is made **instruction-fetchable by MPU region2** in `src/bsp.c`
      to hold the relocatable `.bin` double-slot and run it **XIP**.  Keeping the exec
      window to just the model slots preserves W^X for the data buffers.
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

## Face detection (BlazeFace-front 128, #8)

The first model is ST Model Zoo's **BlazeFace Front 128x128** (a MediaPipe/PINTO
SSD face detector).

- **Model**: `int8` quantized (int8 weights, 105 KB) but **float32 I/O** (a
  QUANTIZE node at the input, DEQUANTIZE nodes at the outputs are baked in).  Input
  `1x128x128x3 f32`; 4 output tensors (2 scales x {box, score}): `1x512x16` +
  `1x512x1` (16x16 grid x2) and `1x384x16` + `1x384x1` (8x8 grid x6) = **896
  anchors**.  box 16 = 4 bbox + 6 keypoints x2.
- **Preprocessing** (`nn_camera.c`): RGB565 → nearest-neighbour resize → float32
  **[0,1]** (`rgb/255`).  The model card says [-1,1], but only [0,1] detects faces
  on hardware (maxscore 288 vs 11) — ST retrained with [0,1].  `ai norm <0|1>`
  toggles it at runtime (0=[0,1] default / 1=[-1,1]).
- **Post-processing** (`port/nn/models/blazeface.c`): tensors located by shape
  (C=16→box, C=1→score; anchors=512→16x16, 384→8x8), cell-centre anchors, a logit
  score threshold (no expf), box = `raw/128 + anchor centre`, hard NMS (IoU 0.5).
  A safe no-op for non-BlazeFace models.
- **Performance**: 31.8M MACC → **~685 ms / ~1.5 fps** (~4.6 cyc/MACC).  Not
  real-time video but usable for a demo; `-O time` is worse (balanced is best).

## `ai` shell commands

| Command | Description |
|---|---|
| `ai info` | backend / model name / I/O tensor shape, dtype, quant / arena size |
| `ai bench [n]` | run inference n times on a fixed input; min/avg/max latency (µs, DWT) + throughput |
| `ai run` | single-shot inference on one camera frame; latency + face boxes |
| `ai stream start [qqvga\|qvga]` | start live continuous inference (default QVGA) |
| `ai stream stop` | stop |
| `ai stream stats` | inference rate / latency / drops / face boxes (+ maxscore diag) |
| `ai model` | show the current model source (`builtin` / `sd:<name>`) |
| `ai model load <sd-path>` | load + run a model from the microSD (`tflm`=`.tflite` / `stedgeai_reloc`=`network_rel.bin`, #89/#92) |
| `ai model builtin` | revert to the built-in model (`stedgeai_reloc` has none, so this unloads → `(none)`) |
| `ai norm <0\|1>` | float input normalization toggle (1=[-1,1] / 0=[0,1] default) |

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

# X-CUBE-AI backend (needs a stedgeai install + a generated model, #6)
# 1) generate the model (output is .gitignored under port/nn/generated/):
$STEDGEAI_ROOT/Utilities/linux/stedgeai generate --model model.tflite \
    --target stm32f7 --type tflite --name network --output port/nn/generated
# 2) build:
cmake -B build ... -DCONFIG_NN_BACKEND=stedgeai -DSTEDGEAI_ROOT=/opt/ST/STEdgeAI/4.0

# X-CUBE-AI relocatable backend (load the model from SD, P5 #92)
# 1) generate the model .bin (not committed; puts the project toolchain on PATH):
scripts/gen-reloc-model.sh [model.tflite] [out]     # default = BlazeFace int8 → port/nn/reloc-out/
#    → copy out/network_rel.bin to the microSD root
# 2) build + flash:
cmake -B build-reloc ... -DCONFIG_NN_BACKEND=stedgeai_reloc
# 3) on hardware:  ai model load network_rel.bin
```

!!! warning "Licensing"
    This repository is public.  The ST-SLA runtime `.a`, generated code
    (`port/nn/generated/`), the ST host loader `ai_reloc_network.c`, the relocatable
    `network_rel.bin`, and ST Model Zoo model binaries are **not committed**
    (`.gitignore`).

## TFLM backend (P3, #86/#88)

TensorFlow Lite Micro (tflite-micro) is C++17.  Bringing C++ into a pure-C/ASM
project is the biggest risk, so the M1 spike (#86) proved viability first; **M2
(#88) ported BlazeFace-front 128 int8 for real**.  The same int8 `.tflite` feeds
both stedgeai and TFLM.

- **C++ only for the tflm build**: `cmake/tflite-micro.cmake` calls
  `enable_language(CXX)` only when `CONFIG_NN_BACKEND=tflm`.  All C++ is confined to
  the `tflm` static lib; `threadx`'s own TUs stay C, so the **final link keeps the C
  driver (gcc)** (`LINKER_LANGUAGE C`), explicitly linking the nano C++ archives
  (`libstdc++_nano.a` / `libsupc++_nano.a`) + `libm`.  Default null/stedgeai builds
  never see C++ (byte-identical).
- **Bare-metal C++ runtime**: startup's `__libc_init_array` runs the static ctors
  (`.init_array` is `KEEP`t in the linker script).  `-fno-exceptions -fno-rtti
  -fno-threadsafe-statics -fno-use-cxa-atexit` + `TF_LITE_STATIC_MEMORY` (bump
  allocator = no heap).  Our own non-throwing `operator new/delete`
  (`port/nn/tflm/cxx_runtime.cc`) keeps libstdc++'s **throwing operator new**
  (→ `__cxa_throw`/`_Unwind_*`) from ever being linked.
- **Model array generated at configure**: the `.tflite` (`NN_TFLM_MODEL`, defaults
  to the same BlazeFace int8 as stedgeai) is not committed (`.gitignore`).
  `cmake/gen_model_array.py` emits an `alignas(16)` `g_blazeface_model_data[]` into
  the **build dir** and adds it to the tflm lib; a path+size+mtime stamp detects a
  model swap (`-DNN_TFLM_MODEL=<path>` overrides).
- **Resolver / I/O**: a `MicroMutableOpResolver<8>` registers BlazeFace's ops
  (QUANTIZE / CONV_2D / DEPTHWISE_CONV_2D / ADD / PAD / MAX_POOL_2D / RESHAPE /
  DEQUANTIZE).  `nn_tflm.cc` builds 1-in / N-out generically from the interpreter
  (reporting `arena_used_bytes()` to `ai info`), so `blazeface.c` locates the four
  outputs by shape — no model-specific code in the backend.
- **Arena**: `.sdram.ai` (bank3, cacheable WBWA, CPU-only).  BlazeFace activations
  measured **~470 KB** (512 KB reserved; with the 384 KB staging it fits bank3's
  2 MB).  Same bank as stedgeai.
- **Single-precision FPU** (`fpv5-sp-d16`) → `double` is soft-float.  BlazeFace has
  no Softmax/Logistic (activations are fused), so there is no heavy `std::exp`
  (double) path.
- **Vendoring = fetch at configure (not checked in)**: `cmake/tflite-micro.cmake`
  `git fetch`es the pinned SHA at CMake configure time and runs `create_tflm_tree.py`
  into a self-contained tree under the **build dir**, compiled with our flags.  A stamp
  keyed on SHA + kernel (ref/cmsis_nn) skips it after the first run.  **numpy/Pillow are
  required** (`requirements.txt`, auto-created `./.venv`).

### CMSIS-NN optimized kernels (M2b)

`NN_TFLM_CMSIS_NN` (default **ON**) passes `OPTIMIZED_KERNEL_DIR=cmsis_nn` to
`create_tflm_tree.py`.  The tflm Makefile downloads **CMSIS_6 core + ARM-software/
CMSIS-NN** (each at its own pinned SHA) and swaps in the cmsis_nn kernel wrappers
(CONV_2D / DEPTHWISE_CONV_2D / ADD / PAD / MAX_POOL_2D) for the reference ones.
`-DCMSIS_NN` flips the kernel headers from inline reference registrations to extern
declarations (without it the optimized kernels redefine them).  **Cortex-M7 is
armv7e-m**, so GCC defines `__ARM_FEATURE_DSP` and CMSIS-NN's **SIMD (SMLAD) int8
path** is compiled (a `#error` canary in `nn_tflm.cc` asserts it; MVE is absent on
the M7).  `NN_TFLM_CMSIS_NN=OFF` reverts to pure reference kernels for comparison.

**`ai bench`** (BlazeFace-front 128 int8, 31.8M MACC, @216 MHz, isolated `nn_run`):

| backend | kernel | latency/inference | cyc/MACC |
|---|---|---|---|
| `stedgeai` (X-CUBE-AI) | ST optimized | ~685 ms | ~4.6 |
| `tflm` | reference | ~2,418 ms | ~16.4 |
| **`tflm`** | **CMSIS-NN** | **~622 ms** | **~4.2** |

CMSIS-NN accelerates CONV_2D / DEPTHWISE_CONV_2D with SIMD — **~3.9× over reference**
and a hair faster than stedgeai.  Detections (bbox / score) agree across all three.
Live `ai stream` runs ~1.5 s/inference (~0.5 inf/s) — slower than the isolated bench
because the DCMI competes for bank3 bandwidth (the F746 bus matrix is round-robin
with no SDRAM QoS).  Flash cost is **~+60 KB** over the null baseline (interpreter +
CMSIS-NN lib + the 189 KB model).

```bash
# TFLM backend (fetches tflite-micro + CMSIS-NN at configure -- needs network + python3)
cmake -B build-tflm -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
    -DCONFIG_NN_BACKEND=tflm                 # CMSIS-NN on by default
cmake --build build-tflm
# to compare against reference kernels:
cmake -B build-tflm ... -DCONFIG_NN_BACKEND=tflm -DNN_TFLM_CMSIS_NN=OFF
# on hardware: ai info / ai bench / ai stream
```

## Load a model from SD (P2, #89)

Because TFLM interprets a `.tflite` flatbuffer in RAM, the model can be swapped at
runtime with no recompile (unlike stedgeai).  P2 loads a `.tflite` from the microSD
(FileX) into `.sdram.ai` and rebuilds the interpreter in place.

- **Op superset**: since the runtime model is unknown, the resolver registers a broad
  common-vision **superset (23 ops: Conv/DWConv/FC/Pool/Softmax/Logistic/Relu/Reshape/
  Quantize/Dequantize/Pad/Add/Mul/Sub/Mean/Concat/Resize/StridedSlice/LeakyRelu/
  Transpose ...)**, so any small int8 classifier/detector runs.  BlazeFace uses only 8
  of them (the default path is unaffected; unused kernels are dropped by `--gc-sections`).
- **Double model slot**: two 512 KB slots in `.sdram.ai`.  `ai model load` reads the SD
  file into the **inactive** slot and only flips it active once the interpreter rebuild
  succeeds, so the live flatbuffer is never corrupted mid-load -- the swap is
  **transactional** (the previous model stays on failure).  bank3: staging 384 KB +
  arena 512 KB + model 2x512 KB = 1920 KB < 2 MB.
- **Safe rebuild**: `~MicroInterpreter()` calls `FreeSubgraphs()`, so a rebuild explicitly
  destroys the old interpreter before placement-new.  The `.tflite` is length-checked with
  `VerifyModelBuffer` before use (a truncated/corrupt file fails cleanly); the SD read
  pre-checks the full size and rejects a short read.
- **Exclusion**: the interpreter/arena/model are singletons, so `ai model load` runs with
  the **stream stopped + the single session held** (the session is claimed *before* slot
  selection, so another shell cannot flip our chosen slot).
- **The SDMMC DMA never writes `.sdram.ai` directly**: the SD driver DMAs into a fixed SRAM
  bounce buffer then `memcpy`s, so the bank3 write is a CPU store -- D-cache coherent, no
  maintenance needed.

```
ai model                 # current source (builtin / sd:<name>)
ai model load bf.tflite  # load a .tflite from microSD (check with `sd ls`)
ai info                  # inspect the loaded I/O shapes / arena
ai model builtin         # revert to the built-in model
```

!!! note "tflm live-stream bandwidth"
    `ai stream` (tflm + CMSIS-NN) can saturate the SDRAM bus during inference and stop
    the camera on a DCMI FIFO overrun (the F746 bus matrix is round-robin with no SDRAM
    QoS).  Single-shot `ai bench` / `ai run` and the GUI overlay (which auto-recovers)
    are fine; overrun resilience for the shell `ai stream` path is a follow-up.

## X-CUBE-AI relocatable backend (P5, #92)

X-CUBE-AI normally bakes the topology into Flash, but a **relocatable network** makes it
position-independent so the model can be swapped **without reflashing**.  P5 implements this
as `CONFIG_NN_BACKEND=stedgeai_reloc`, the stedgeai-runtime counterpart to P2(tflm)'s SD swap
(both backends covered).

- **Artifact = a single `network_rel.bin`**: `stedgeai generate --relocatable --target stm32f7`
  emits a ~208 KB blob (PIC code + embedded weights) via `scripts/gen-reloc-model.sh`.
  **Not committed** (ST-SLA / model-zoo).
- **Firmware compiles just the loader**: the ST host loader `ai_reloc_network.c` (compiled from
  the ST install, `APP_DEBUG=0` to drop its printf) + the legacy `ai_rel_network_*` API.  **No
  runtime `.a` is linked** — the PIC kernels, weights, and memcpy/memset all live inside the
  `.bin`, and each model entry is dispatched indirectly through an r9=GOT-base trampoline.
- **XIP + MPU**: the `.bin` is loaded into bank3's upper 1 MB (`.sdram.ai.model`, `0xC0700000`),
  which `src/bsp.c` **MPU region2** makes instruction-fetchable (the lower half stays XN).  Code
  executes in place from the `.bin` buffer (only data/got/bss go to the RT RAM).  ★**Cache**: the
  loader does no cache maintenance in XIP, so the backend does a **`SCB_CleanDCache_by_Addr` +
  `SCB_InvalidateICache`** before install (the Cortex-M7 I/D caches are separate; I-fetch does not
  snoop D-cache; RM0385 §2.1.3 AXIM).
- **Transactional double-slot**: two `.bin` slots in `.sdram.ai.model`.  Because **XIP runs the old
  code from the old `.bin`**, overwriting a single buffer would be fatal — a reload reads the new
  `.bin` into the *inactive* slot and only flips once install/init/get_report validate (the old
  model stays alive until then; on any failure the old one is kept).
- **Bounded `.bin` verifier**: the ST loader takes no length and blindly follows in-object offsets,
  so the backend self-parses the header (magic `0x4E49424E` / CM7-FPU-hard variant / all section
  bounds / entry vectors / `.rel` scan) **before any ST API call**.  Sizing (`acts_sz` / RT RAM /
  weights) is derived from the verified header + ctx file image rather than `ai_rel_network_rt_get_info()`
  (which does a scaled-pointer OOB read pre-install).
- **SD-only**: no built-in model.  Until `ai model load network_rel.bin`, there is no model
  (`ai run`/`ai stream`/`gui overlay` reject with a clear error); `ai model builtin` unloads (`(none)`).

**Performance (measured)**: the relocatable code runs **XIP from SDRAM**, not Flash, but cacheable
SDRAM + the I-cache keep it fast — `ai bench` measures **~592 ms/inference** (BlazeFace-front 128,
@216 MHz), *below* the Flash-baked `stedgeai` (685 ms) and on par with TFLM CMSIS-NN (622 ms) (the
legacy ai_network runtime's kernels help here).  The live `ai stream`/`gui overlay` runs at ~1.4
inf/s because the DCMI competes for bank3 bandwidth (same root cause as #90, slower than the
single-shot bench).  P5's main value is **demonstrating X-CUBE-AI relocatable / completing the Epic**.

Key files: `port/nn/nn_stedgeai_reloc.c` (backend + `.bin` verifier + transactional reload + XIP),
`src/bsp.c` (MPU region2), `ldscript/STM32F746NGHx_FLASH.ld` (`.sdram.ai` split + ASSERTs),
`scripts/gen-reloc-model.sh` (offline `.bin` generation).

## Roadmap (Epic #80)

| Phase | Scope |
|---|---|
| **P1 ✅** | `nn` abstraction + X-CUBE-AI backend + `ai` command + **BlazeFace-128 face detection first-light** |
| **P2 ✅** | **Load a model from SD (FileX → `.sdram.ai`, `ai model load`, #89)** |
| **P3 ✅** | TFLM backend.  M1 spike (C++ enablement + hello_world, #86) → **M2 BlazeFace port + CMSIS-NN + comparative bench (#88)** |
| **P4 ✅** | GUIX overlay (live preview + face bbox, #83 → [GUIX camera UI](../rtos/guix.md#face-detect-overlay-83-epic-80-p4)) |
| **P5 ✅** | **X-CUBE-AI relocatable network — swap the whole model from SD, XIP (`stedgeai_reloc`, #92)** |
