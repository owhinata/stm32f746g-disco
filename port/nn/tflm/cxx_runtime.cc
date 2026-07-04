/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cxx_runtime.cc
 * @brief   Minimal bare-metal C++ runtime shim for the TFLM backend (Epic #80 #86).
 *
 * Compiled only into the `tflm` static lib (CONFIG_NN_BACKEND=tflm).  Its whole
 * job is to keep the C++ runtime surface tiny under newlib-nano:
 *
 *  - We provide our OWN global operator new/delete.  TFLM with TF_LITE_STATIC_MEMORY
 *    uses a bump allocator and must never hit the heap, but flatbuffers / STL bits
 *    still *reference* operator new.  If we did not define it, the linker would pull
 *    libstdc++_nano's THROWING operator new, dragging in __cxa_throw / _Unwind_* /
 *    std::bad_alloc type-info even though everything is built -fno-exceptions.  Our
 *    strong (non-throwing) definitions resolve those symbols first, so the throwing
 *    versions are never extracted.
 *
 *  - operator new records every call (g_tflm_cxx_new_calls, exported) and routes to
 *    newlib malloc/free.  The spike's goal is to prove inference runs with NO heap:
 *    with TF_LITE_STATIC_MEMORY the bump allocator means new is never hit, and
 *    `ai selftest` asserts the counter stayed 0 (runtime proof).  Routing to malloc
 *    (rather than returning nullptr) keeps the shell graceful if new is ever reached
 *    and avoids -Wnew-returns-null; it is NOT a link-level "no heap" guarantee.
 *
 *  - __cxa_pure_virtual traps (should never run).  __cxa_guard_* are avoided by
 *    -fno-threadsafe-statics; __cxa_atexit/__dso_handle by -fno-use-cxa-atexit.
 */
#include <cstddef>
#include <cstdint>
#include <cstdlib>

/* Exported so `ai selftest` (tflm_spike.cc / cmd_ai.c) can assert new was never
 * called during a successful inference (the TF_LITE_STATIC_MEMORY contract). */
extern "C" volatile uint32_t g_tflm_cxx_new_calls;
volatile uint32_t g_tflm_cxx_new_calls = 0;

/* Our own global operator new/delete.  Defining them here means the linker resolves
 * operator new from THIS object and never extracts libstdc++_nano's THROWING version
 * (which would drag in __cxa_throw / _Unwind_* / std::bad_alloc type-info even under
 * -fno-exceptions).  We keep the standard (throwing) operator new signature and route
 * to newlib's malloc/free: with TF_LITE_STATIC_MEMORY the bump allocator means these
 * are never hit at runtime, and g_tflm_cxx_new_calls (asserted == 0 by `ai selftest`)
 * proves it -- but if new IS ever reached the shell degrades gracefully instead of
 * hanging.  (Routing to malloc, not returning null, also avoids -Wnew-returns-null.) */
void *operator new(std::size_t n)   { g_tflm_cxx_new_calls++; return malloc(n); }
void *operator new[](std::size_t n) { g_tflm_cxx_new_calls++; return malloc(n); }
void  operator delete(void *p) noexcept   { free(p); }
void  operator delete[](void *p) noexcept { free(p); }
void  operator delete(void *p, std::size_t) noexcept   { free(p); }   /* C++14 sized */
void  operator delete[](void *p, std::size_t) noexcept { free(p); }

/* A class with pure virtuals whose pure method is somehow called -> trap.  Should
 * never execute; provided so the link never falls back to libsupc++ for it. */
extern "C" void __cxa_pure_virtual(void) { for (;;) { } }
