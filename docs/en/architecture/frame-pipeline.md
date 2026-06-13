<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 ThreadX Shell Project
-->

# Camera frame pipeline (one source -> many sinks)

An architecture for distributing camera frames from **one producer (DCMI
capture) to many sinks (file / LTDC / Ethernet / VCP)** (issue #47).  It applies
the same "thin vtable + multiple backends" pattern that `fs_device`
([Filesystem](../rtos/filesystem.md), #34) uses for media and that `ym_source`
([xfer](../rtos/shell-xfer.md), #50) uses for a byte source, this time to frame
distribution.

!!! note "This page is the design (an interface proposal)"
    The implementation of this design (the ring/dispatch engine, the DMA
    double-buffer, the individual sinks) lands in later issues.  This page fixes
    the abstract interface and the ownership / back-pressure / threading /
    format / layering decisions.  The header proposals are `svc/frame.h` and
    `svc/frame_pipeline.h` (declarations only).

## Background

The camera today ([Camera](../hardware/camera.md), Epic #22) captures a single
QVGA RGB565 snapshot into **one static buffer** `cam_frame[]` in `.sdram`, and
serialises reads with `cam_frame_gen` (a generation counter) and `cam_done`
(the DCMI frame-complete semaphore).  **LTDC live display (#48)** and **Ethernet
streaming (#49)** are planned, so a frame's "source" and "destination" become
many-to-many.  Wiring sinks directly while implementing streaming (#46) would
mean rebuilding frame distribution every time a destination is added.

| Sink | Rough bandwidth | QVGA RGB565 live |
|---|---|---|
| VCP (USART1 115200) | ~0.09 Mbps | x preview only |
| File (SD / QSPI) | a few MB/s | ~ burst / time-lapse |
| LTDC (#48 planned) | internal | best, local display |
| Ethernet (#49 planned) | 100 Mbps | best, network (MJPEG/HTTP, RTP) |

## Layering (consistent with #43)

The pipeline's **pure core** lives in the freestanding [`svc/` layer](layering.md).
It calls no `tx_*` (ThreadX) and no HAL; its only dependencies are `<stdint.h>`
and an injected mutual-exclusion vtable (`frame_os`).  That makes the
ring/refcount/policy logic **host unit-testable** (a no-op lock plus mock
sinks), so it sits in the same class as `svc/ymodem.c` / `svc/fmt.c`
(`svc/timebase.c`, which includes HAL, is the exception the pipeline does not
follow).

```
HAL / CMSIS / ThreadX   <-   svc/frame_pipeline (pure core)   <-   port/camera (producer)
        (lib/)                                                 <-   each sink (shell / port / port-net)
```

All ThreadX -- the producer/sink threads, the ISR-to-thread notification, the
`TX_MUTEX` behind `frame_os` -- is confined to the glue.  The core is always
entered from thread context under the injected lock, and the DCMI ISR never
touches the ring; it just posts `cam_done` (the existing discipline).

| Element | Location | Dependency direction |
|---|---|---|
| pipeline pure core | `svc/frame_pipeline.{c,h}` + `svc/frame.h` | **no** ThreadX/HAL/FileX/shell |
| ThreadX glue | producer / each sink | drives the core; `tx_*` only here |
| producer | `port/camera/` (owns the SDRAM slot array, injects it, publishes) | `port/camera -> svc/` (same way as `port/sdram -> svc/timebase`) |
| push sinks | file/vcp = `shell/`, ltdc = `port/` (#48), eth = `port/` (#49) | each sink -> `svc/` + its own layer |
| wiring | shell command / `src/main` | attaches sinks |

## Two access styles

```
 [Producer]               [Frame Ring (svc/ bookkeeping / SDRAM owned by producer)]   [Push Sinks]
 port/camera              +- slot0 -+  per slot: gen + refcount + state               +- ltdc_sink (#48)
 DCMI(+DMA2)        --->   |  ...    |  producer reuses only refcount==0               +- eth_sink  (#49)
 snapshot->continuous     +- slotN -+  async sink pins via get/put                    +- vcp_sink  (preview)
       |                       |
       | cam_done ISR (today)  +-- pull: read_latest(latest published slot) -> save / send / stats (#42/#50)
```

- **push sinks** (streaming) register with `frame_pipeline_attach()` and get
  `consume()` called on every publish: LTDC / Ethernet / VCP preview.
- **pull access** (snapshot) reads the latest published slot via
  `frame_pipeline_read_latest()` -- the generalised `camera_frame_read()`: save /
  send / stats.

### Snapshot is the degenerate pipeline

A single `camera capture` is just the special case: an **N=1 ring, no push
sinks, one publish, read back by pull**.  So the existing `camera capture` /
`camera_frame_read` / `camera save` (#42) / `camera send` (#50) keep both their
**shell-command semantics and their public API unchanged**; only `camera.c`'s
internals swap `cam_frame[]` for the ring in #46.  This is what de-risks the
migration.

## Data contract: `frame_desc`

A frame lives in an SDRAM slot and is **never copied** between producer and
sink; only this descriptor is handed across.

```c
enum frame_format { FRAME_FMT_RGB565 = 0, FRAME_FMT_YUV422, FRAME_FMT_Y8, FRAME_FMT_JPEG };

struct frame_desc {
    void    *data;     /* slot start in SDRAM (non-cacheable, #40); sink read-only */
    uint32_t bytes;    /* valid payload length (JPEG variable, effective length)   */
    uint32_t gen;      /* monotonic generation (generalises cam_frame_gen)         */
    uint16_t width, height, stride;   /* stride = bytes per row (0 for JPEG)        */
    uint8_t  format;   /* enum frame_format                                         */
    uint8_t  flags;
    void    *_slot;    /* pipeline-internal handle (sinks must not touch)           */
};
```

`consume()` receives a `const struct frame_desc *`, and `data` is **read-only**
to the sink (only the producer writes the slot).  `format` is where variable
formats (#45) and JPEG plug into the descriptor; a sink decides acceptability in
`open()`.

## Sink contract: `frame_sink`

```c
enum frame_policy { FRAME_POLICY_DROP = 0, FRAME_POLICY_LATEST };

struct frame_sink {
    const char *name;
    void       *ctx;
    uint8_t     policy;
    int  (*open)(void *ctx, enum frame_format fmt, uint16_t w, uint16_t h); /* <0 rejects */
    int  (*consume)(void *ctx, const struct frame_desc *f);                /* <0 = sink error */
    void (*close)(void *ctx);
    uint32_t delivered, dropped, errors;          /* core updates under the lock */
    /* core-internal (owner must not touch) */
    struct frame_sink *_next; const struct frame_desc *_pending; int _busy;
};
```

`consume()` is called by publish **outside** the lock, with the slot already
**pre-pinned once** on this sink's behalf.  A synchronous sink does its work and
`put()`s before returning; an asynchronous sink hands `f` to its own thread/queue
and returns (the pre-pin keeps the slot alive), then `put()`s from that thread
when done.  The pin is released with **exactly one `put()`, even on an error
return** (no refcount leak).  `attach()` calls `open()` with the producer's
current format/geometry (fixed QVGA RGB565 today; #45 makes it variable,
re-opening sinks via close()+open() on a change); an unsupported format fails the
attach.

## Concurrency and lifetime contract

The core has only the injected lock and no wait primitive (all waiting /
notification / threading is owned by the glue).  Under that constraint, three
contracts make ownership sound.

### (1) publish lock discipline and two-phase fan-out

The lock guards only bookkeeping (free-slot choice / refcount / registry,
`_busy`, `_pending` / stats); **`consume()` is never called while the lock is
held**.

```text
publish(f):
  LOCK
    f.gen = ++published ; slot(f).state = READY
    deliver = []
    for s in registry:
        if s._busy:
            if s.policy == DROP:   s.dropped++ ; continue
            if s.policy == LATEST:                       # keep only the newest (do not deliver now)
                if s._pending: put_locked(s, s._pending)  # drop the old pending's pin
                s._pending = f ; slot(f).refcount++      # pre-pin the new pending
                continue
        s._busy = 1 ; slot(f).refcount++                 # pre-pin (one per delivering sink)
        deliver.append(s)
  UNLOCK
  for s in deliver: s.consume(f)                         # lock-free; stats/put reflected under a short lock afterwards
```

`frame_pipeline_get/put` take the lock briefly themselves.  By the time
`consume()` calls them the publish lock is already released, so a **non-recursive
mutex never self-deadlocks**.  Because several sinks receive the same descriptor,
**get/put also take the target sink `s`** (the slot alone cannot identify whose
pin / `_busy` / `_pending` this is).  The producer's `acquire()` chooses a slot
that is **refcount==0 and not the latest published one** (so a pull reader's
current frame is never recycled under it).  All slots pinned -> NULL -> the
producer counts an overrun and drops the frame.

### (2) LATEST pending transfer

`_pending` is pre-pinned at coalesce time.  On a LATEST sink's completion
`put()` the order is fixed: (1) lock (2) move `_pending` to a local,
`_pending=NULL`, keep `_busy=1` (**transfer the pin, do not pin again**)
(3) unlock (4) call `consume(local)` lock-free (5) reflect stats/put afterwards.
With no `_pending`, step (2) sets `_busy=0`.  So a LATEST sink's `consume` is
invoked either from publish (producer thread) or from put (its own completion),
but `_busy` ensures **never concurrently for the same sink**.  Pin accounting
balances: +1 at coalesce, -1 at the re-delivery's put.

### (3) detach and in-flight sink lifetime

`frame_sink` is caller-owned, so a queued frame or a running `consume()` after
detach would be a use-after-free.  A two-step contract:

1. `frame_pipeline_detach()` unlinks the sink under the lock and marks it
   detaching; **after it returns no further `consume()` is issued to that sink**.
   It returns the number of pins still in flight (**including a LATEST pending
   pin**).
2. Before freeing the sink / its ctx, the caller (which owns the sink's thread
   and queue) **drains that thread** -- lets the running consume finish and every
   pin be `put()` -- until the in-flight count reaches zero.  The core does not
   wait.

Because both the publish delivery set and detach happen under the lock, detach
serialises either "before the set is fixed" (excluded) or "after" (at most one
more frame delivered, then observed as in-flight); there is no intermediate UAF.

### Tear detection for pull (read_latest)

`read_latest(off, dst, len, gen)` does **memcpy + gen return under the lock**
(the same discipline as `camera_frame_read`).  A single call cannot tear (the
slot is not recycled mid-copy); a multi-call reader (row-by-row save) compares
`gen` across calls to detect a frame replaced between reads (the existing
#42/#50 contract, generalised).

## Decisions at a glance

| Decision | Resolution |
|---|---|
| **Back-pressure** | push policy = drop / latest only; must-complete (save) is expressed as pull + snapshot (the DCMI cannot stall mid-frame, so a slow sink drops) |
| **Ownership / lifetime** | generation stamp + per-slot refcount; publish pre-pins one per delivering sink under the lock, the sink puts once; the producer reuses only a refcount==0, non-latest slot; N defaults to 3 (2 for DMA ping-pong + 1 held by a sink) |
| **Threading** | ISR (DCMI frame) -> `cam_done` post only; fan-out runs in the producer thread's publish(); slow sinks use their own thread + queue |
| **Format coupling** | `frame_desc.format` + variable `bytes`; a sink decides in `open(fmt,w,h)`; ltdc=RGB565 (or DMA2D), eth(MJPEG)=JPEG, vcp=downscale |
| **Snapshot unification** | the N=1 degenerate case expresses the existing capture/frame_read/save/send unchanged |
| **Error / overrun** | DCMI OVR -> producer counts an overrun, discards, re-arms; sink consume()<0 -> per-sink errors; the producer never stops |

## Hardware rationale

- **Memory**: the ring lives in [`.sdram`](../hardware/sdram.md) (0xC0000000,
  8 MB, NOLOAD, MPU non-cacheable, #40).  QVGA RGB565 = 150 KB/frame -> 450 KB at
  N=3; VGA = 600 KB/frame -> 1.8 MB at N=3 (ample within 8 MB).  Because the region
  is **non-cacheable**, DCMI-DMA write / CPU read / future LTDC scanout / ETH
  MAC-DMA are all coherent with no cache maintenance (RM0385 2.1.10-13: every
  master reaches the FMC).
- **DMA NDTR <= 65535**: QVGA (38400 words) fits a single transfer; VGA and above
  need band splitting / DBM (a #45/#46 producer-internal concern; `frame_desc`
  and the ring are unchanged).
- **DCMI mode**: snapshot auto-stops (today); continuous uses DBM/circular (#46).
  The inactive DBM `M0AR/M1AR` is repointed at the next free slot in the
  transfer-complete callback to form an N-slot ring (RM0385 8.3.10).  The
  producer **owns the DBM pointer updates** and does not rely on the
  `HAL_DCMI_Start_DMA(Length>0xFFFF)` internal contiguous-split path.
- **Interrupts / priorities**: DCMI/DMA2_Stream1 = NVIC prio 8; SysTick 14 >
  PendSV 15 (required by ThreadX).  ThreadX thread priorities are
  [IWDG](../rtos/iwdg.md) petter 5 / LED 10 / shell 16 / jobs 17; the producer
  sits around 9-12 and async sinks below it (so the watchdog is never starved).
  `tx_semaphore_put` is ISR-safe.

## Downstream requirements (settled in #48/#49)

- **SDRAM bandwidth**: the risk is bandwidth, not capacity.  DCMI write + LTDC
  scanout + DMA2D + ETH DMA + CPU read all converge on one FMC SDRAM.  **#48/#49
  must include a measured-bandwidth and slot / display-buffer budget table.**
- **`.sdram` teardown**: `sdram test` clobbers `.sdram`.  As the ring and a
  future LTDC framebuffer grow there, `camera_frame_invalidate()` alone is no
  longer enough -> the pipeline gains a "suspend/invalidate all SDRAM-resident
  consumers" contract (a generalisation of `camera_frame_invalidate`).

## Relationship to later issues

```
#47 (this design) -- fixes --> #46 (producer base: DCMI continuous + DMA double-buffer)
                                 |- #45 (resolution / format: JPEG into frame_desc)
                                 |- #48 (LTDC + GUIX: plug in ltdc_sink)
                                 +- #49 (Ethernet + NetX Duo: plug in eth_sink)
```

This design is a **precondition for #46's implementation plan**: #46's
double-buffer is designed to hand descriptors to several sinks, and #45's JPEG /
variable formats fold into `frame_desc`.  #44 (image-quality settings) can
proceed independently.  Bringing up LTDC / Ethernet is separate work, but this
design guarantees their sinks can be slotted in later.
