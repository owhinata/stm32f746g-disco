/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_free.c
 * @brief   `free` shell command (issue #58): per-region memory usage at runtime.
 *
 * A runtime, dynamic counterpart to the build-time `size` output: it reports, for
 * each of the four physical memory regions, the total / statically-used / free
 * bytes, plus the current newlib heap occupancy.  Pure introspection -- it reads
 * linker-provided boundary symbols and the C library's malloc accounting; it
 * changes no state and touches only the shell instance passed to it, so it stays
 * reentrant across instances (req §10).  Linked into the `shell` executable only,
 * never into the host test harness.
 *
 *   region   start        total      used      free  use%
 *   Flash    0x08000000  1048576    294752    753824   28%   .isr/.text/.rodata/.data(load)
 *   DTCM     0x20000000    65536      8224     57312   12%   .log_noinit (reset-persistent ring)
 *   SRAM     0x20010000   262144     ......    ......  ..%   .data/.bss/.sram1_dma + heap
 *   SDRAM    0xC0000000  8388608   ........  ........  ..%   .sdram (camera/LTDC NOLOAD)
 *
 * Per-region accounting (linker symbols in ldscript/STM32F746NGHx_FLASH.ld):
 *   Flash  used = LOADADDR(.data) + sizeof(.data) - ORIGIN(FLASH).  .data's load
 *          image is the last thing placed in FLASH, so this is the whole footprint
 *          (== `size`'s text+data).
 *   DTCM   used = .log_noinit (_elog_noinit - _slog_noinit), the only resident.
 *   SRAM   static = _end - ORIGIN(RAM) (.data + .bss + .sram1_dma); the heap then
 *          grows up from _end and the main/ISR stack grows down from _estack, so
 *          used = (heap break) - ORIGIN(RAM) and free = _estack - (heap break).
 *   SDRAM  used = .sdram.fixed (bank0) + .sdram.cam (bank1 camera arena); the two
 *          bank-aligned sub-regions are summed so the 2 MB-alignment hole between
 *          them is not counted (issue #65).
 *
 * Region ORIGIN/LENGTH are compile-time constants mirroring the linker MEMORY
 * block (single source of truth: the .ld).  They never change without a linker
 * edit, and the firmware already hardcodes the same addresses in bsp.c (MPU) and
 * the SDRAM/QSPI drivers; duplicating them here keeps `free` a zero-linker-change,
 * read-only command rather than adding PROVIDE() symbols.
 *
 * Heap is read via newlib's mallinfo() rather than sbrk(0): the toolchain's stock
 * _sbrk() (libnosys) compares the requested break against the *current* stack
 * pointer, which in a ThreadX thread is the thread's PSP (in .bss, below the heap)
 * -- so sbrk(0) is unreliable from thread context.  mallinfo() reads malloc's own
 * accounting and avoids _sbrk entirely.  arena == 0 (malloc never called) prints a
 * zero heap line.  The free-list walk is not locked against a concurrent
 * malloc/free on another thread; like `thread`'s stats this is best-effort
 * diagnostics, and this firmware barely uses the heap.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"

#include <malloc.h>   /* mallinfo / struct mallinfo */
#include <stdint.h>

/*
 * Region geometry -- mirrors the MEMORY block of
 * ldscript/STM32F746NGHx_FLASH.ld (the authoritative source).  See the file
 * header for why these are constants here rather than linker symbols.
 */
#define FLASH_ORIGIN  0x08000000u
#define FLASH_LENGTH  (1024u * 1024u)
#define DTCM_ORIGIN   0x20000000u
#define DTCM_LENGTH   (64u   * 1024u)
#define SRAM_ORIGIN   0x20010000u
#define SRAM_LENGTH   (256u  * 1024u)
#define SDRAM_ORIGIN  0xC0000000u
#define SDRAM_LENGTH  (8u * 1024u * 1024u)

/*
 * Linker boundary symbols.  Their *addresses* carry the values; _Min_Stack_Size
 * is an ABSOLUTE symbol whose address IS the byte count (0x400).  Declared as
 * arrays so a bare reference already yields the address without &.
 */
extern uint8_t _sdata[], _edata[];          /* .data run image in SRAM        */
extern uint8_t _sidata[];                   /* .data load image in FLASH      */
extern uint8_t _slog_noinit[], _elog_noinit[]; /* DTCM reset-persistent ring  */
/* .sdram is split into two bank-aligned sub-regions with a 2 MB-alignment hole
 * between them (issue #65); sum the two residents so the hole is not counted. */
extern uint8_t _ssdram_fixed[], _esdram_fixed[]; /* .sdram.fixed (bank0)       */
extern uint8_t _ssdram_cam[],   _esdram_cam[];   /* .sdram.cam   (bank1 arena) */
extern uint8_t _end[];                       /* top of static SRAM = heap base */
extern uint8_t _estack[];                    /* top of SRAM (initial MSP)      */
extern uint8_t _Min_Stack_Size[];            /* reserved main-stack bytes      */

static uint32_t sym(const uint8_t s[])
{
	return (uint32_t)(uintptr_t)s;
}

/* One region row: name, start, total, used; free = total - used, use% = used/total. */
static void print_region(struct cli_instance *sh, const char *name,
                         uint32_t start, uint32_t total, uint32_t used,
                         const char *note)
{
	uint32_t freeb = (used <= total) ? (total - used) : 0u;
	uint32_t pct   = total ? (uint32_t)(((uint64_t)used * 100u) / total) : 0u;

	cli_print(sh, "%-6s 0x%08lX %9lu %9lu %9lu %3lu%%  %s\r\n",
	          name, (unsigned long)start, (unsigned long)total,
	          (unsigned long)used, (unsigned long)freeb, (unsigned long)pct, note);
}

static int cmd_free(struct cli_instance *sh, int argc, char **argv)
{
	struct mallinfo mi = mallinfo();   /* heap accounting (arena/uordblks/fordblks) */

	uint32_t flash_used = (sym(_sidata) - FLASH_ORIGIN)
	                    + (sym(_edata) - sym(_sdata));
	uint32_t dtcm_used  = sym(_elog_noinit) - sym(_slog_noinit);
	uint32_t sdram_used = (sym(_esdram_fixed) - sym(_ssdram_fixed))
	                    + (sym(_esdram_cam) - sym(_ssdram_cam));

	uint32_t heap_arena = (uint32_t)(unsigned)mi.arena;   /* bytes sbrk'd from system */
	uint32_t heap_base  = sym(_end);
	uint32_t heap_break = heap_base + heap_arena;
	uint32_t sram_used  = heap_break - SRAM_ORIGIN;        /* static + heap */

	(void)argc;
	(void)argv;

	cli_print(sh, "%-6s %-10s %9s %9s %9s %4s\r\n",
	          "region", "start", "total", "used", "free", "use%");
	print_region(sh, "Flash", FLASH_ORIGIN, FLASH_LENGTH, flash_used,
	             ".isr/.text/.rodata/.data");
	print_region(sh, "DTCM",  DTCM_ORIGIN,  DTCM_LENGTH,  dtcm_used,
	             ".log_noinit (reset-persistent)");
	print_region(sh, "SRAM",  SRAM_ORIGIN,  SRAM_LENGTH,  sram_used,
	             ".data/.bss/.sram1_dma + heap");
	print_region(sh, "SDRAM", SDRAM_ORIGIN, SDRAM_LENGTH, sdram_used,
	             ".sdram.fixed(bank0)+.sdram.cam(bank1)");

	cli_print(sh, "\r\n");
	cli_print(sh, "heap:  base 0x%08lX  arena %lu  in-use %lu  free-pool %lu\r\n",
	          (unsigned long)heap_base, (unsigned long)heap_arena,
	          (unsigned long)(unsigned)mi.uordblks, (unsigned long)(unsigned)mi.fordblks);
	cli_print(sh, "stack: top  0x%08lX  main-reserve %lu B (MSP/ISR grow down into SRAM free)\r\n",
	          (unsigned long)sym(_estack), (unsigned long)sym(_Min_Stack_Size));
	return 0;
}

CLI_CMD_REGISTER(free, NULL, "show per-region memory usage", cmd_free, 1, 0);
