/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_membench.c
 * @brief   `membench` shell command (issue #57): memory bandwidth + latency curve.
 *
 * A self-contained micro-benchmark (no lmbench port) that borrows only the two
 * core ideas of lmbench -- bw_mem (sequential read/write/copy bandwidth) and
 * lat_mem_rd (pointer-chase latency over a swept working set) -- to measure the
 * four physical memories at cycle precision with the Cortex-M7 DWT CYCCNT.  The
 * goal is to make the L1 D$(16KB) -> SRAM -> SDRAM latency step visible on the
 * board (input for #65 .sdram layout and #49 NetX buffer sizing).
 *
 * Timing: DWT CYCCNT (enabled locally; DWT is otherwise unused -- the ThreadX
 * exec-profile uses TIM2 because DWT freezes in WFI, but the bench never sleeps).
 * Clock is read from HAL_RCC_GetHCLKFreq() (not a hardcoded 216 MHz).
 *
 * Preemption: each timed run is sized to ~0.3 ms (< one 1 kHz SysTick period) via
 * a calibration pass, then run up to MEMBENCH_TRIALS times; runs during which the
 * millisecond tick advanced (a SysTick ISR fired) are rejected, and the minimum
 * over the surviving (tick-clean) runs is reported.  This removes SysTick-ISR
 * contamination only -- other IRQs / bus-master contention (USART, DMA, the
 * camera producer, LTDC/FMC scan-out) are NOT detectable via the tick and are
 * merely reduced by the min and by running with camera/LTDC stopped.  Interrupts
 * are never disabled (the IWDG petter, prio-5, must keep refreshing).  Cancel
 * with Ctrl+C between cells.
 *
 * DCE/line-reuse defeat: reads go through `const volatile`, a volatile sink ends
 * each loop, and the latency walk is a dependent load chain `idx = buf[idx]` over
 * word indices (each load address depends on the previous result, suppressing the
 * core's speculative issue and line reuse).
 *
 * Cache semantics (RM0385 §2.1/§3.2, bsp.c MPU): SRAM is cacheable write-back, so
 * the `4KB cached` row is L1 D$ speed and the `32KB refill` row exceeds the D$;
 * for write/copy that row is CPU-observed streaming throughput, NOT an external
 * write-completion figure (the last dirty footprint is not written back here).
 * DTCM and SDRAM are non-cacheable (DTCM is tightly coupled; SDRAM is MPU Normal
 * non-cacheable), so their rows are the real memory rate.  Flash is read-only via
 * AXIM + L1 D$ (the ART accelerator serves the instruction path, not this data
 * read).  The benchmark is CPU-only (no DMA), so no cache maintenance is needed.
 *
 * Linked into the `shell`/threadx executable only (like cmd_coremark/cmd_sdram),
 * never the host test harness.  Clean-room glue.
 */
#include "cli.h"
#include "sdram.h"            /* sdram_is_up() */

#include "stm32f7xx_hal.h"   /* DWT/CoreDebug/SCB, HAL_RCC_GetHCLKFreq, HAL_GetTick, __DSB/__ISB */

#include <stdint.h>
#include <stdlib.h>          /* malloc / free for the on-demand SRAM buffer */
#include <stdio.h>           /* snprintf for table cells */
#include <string.h>          /* strcpy */

/* Per-region benchmark buffers (see plan / linker).  DTCM needs a dedicated
 * NOLOAD section (the region otherwise holds only the log ring); SDRAM reuses the
 * existing .sdram NOLOAD section.  These are diagnostic-only and show up in
 * `free`'s per-region used.  The SRAM buffer is NOT a static .bss array: at 32 KB
 * it would permanently reserve ~1/8 of the internal SRAM for a command that is
 * rarely run, so it is malloc'd on demand and freed when the command returns
 * (issue #94).  32 KB is 2x the 16 KB L1 D-cache, enough to defeat it for the
 * refill row, so the measured out-of-cache rate is unchanged from the old 64 KB. */
#define DTCM_BENCH_BYTES   (16u * 1024u)
#define SRAM_BENCH_BYTES   (32u * 1024u)
#define SDRAM_BENCH_BYTES  (64u * 1024u)
#define FLASH_BENCH_BYTES  (64u * 1024u)
#define SRAM_CACHED_BYTES  ( 4u * 1024u)   /* in-D$ working set for the cached row */

static uint32_t dtcm_bench_buf[DTCM_BENCH_BYTES / 4]
	__attribute__((aligned(32), section(".dtcm_bench")));
static uint32_t sdram_bench_buf[SDRAM_BENCH_BYTES / 4]
	__attribute__((aligned(32), section(".sdram.fixed")));

/* Volatile sink: every measured loop ends by storing into it, so -O2/-O3 cannot
 * eliminate the loop as dead code. */
static volatile uint32_t g_sink;

/* Harness tuning. */
#define MEMBENCH_TRIALS    16u    /* attempts to find tick-clean runs           */
#define MEMBENCH_CLEAN      3u    /* stop once this many clean runs are seen     */
#define MEMBENCH_MAX_ITERS 100000u
#define MEMBENCH_TARGET_DIV 3333u /* hclk / 3333 ~= 0.3 ms of cycles per run     */

typedef void (*work_fn)(void *ctx);

/* ---- DWT cycle counter ------------------------------------------------------ */

/* DWT Lock Access Register (0xE0001FB0) + unlock key.  On Cortex-M7 the DWT has
 * a software lock that, with no debugger attached, leaves CYCCNT frozen at 0 even
 * after TRCENA + CYCCNTENA are set; writing the key unlocks register access so
 * the counter runs.  Not all CMSIS core_cm7.h revisions expose DWT->LAR, so the
 * absolute address is used.  Harmless on parts where the lock is absent. */
#define DWT_LAR_ADDR  0xE0001FB0u
#define DWT_LAR_KEY   0xC5ACCE55u

static int dwt_enable(void)
{
	int attempt;

	if (DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk)
		return -1;                  /* CYCCNT not implemented on this core */

	/* Enable, then self-test that the counter actually advances; retry a few
	 * times re-asserting trace-enable + the DWT unlock.  On a cold boot the
	 * debug power domain can need a moment, and on M7 the DWT software lock must
	 * be cleared (CYCCNT stays frozen at 0 otherwise, e.g. no debugger attached).
	 * If it never advances, abort cleanly rather than let calibration see a zero
	 * delta -> reps clamped to the max -> a multi-minute hang with all-zero results. */
	for (attempt = 0; attempt < 3; attempt++) {
		volatile uint32_t spin = 64u;
		uint32_t a;

		CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
		*((volatile uint32_t *)DWT_LAR_ADDR) = DWT_LAR_KEY;   /* unlock (M7) */
		DWT->CYCCNT = 0u;
		DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

		a = DWT->CYCCNT;
		while (spin--)
			__NOP();
		if (DWT->CYCCNT != a)
			return 0;           /* counting */
	}
	return -1;
}

/* ---- timed harness ---------------------------------------------------------- */

/* Pick an iteration count for `fn` so one run is ~target cycles (~0.3 ms): time
 * `init` units, then scale.  *iter is the field fn reads (reps or chase count). */
static void calibrate(work_fn fn, void *ctx, uint32_t *iter, uint32_t init,
                      uint32_t target)
{
	uint32_t c0, c1, total, per, it;

	*iter = init;
	__DSB(); __ISB();
	c0 = DWT->CYCCNT;
	fn(ctx);
	__DSB(); __ISB();
	c1 = DWT->CYCCNT;
	total = c1 - c0;
	if (total < init) {     /* < 1 cycle/unit is impossible for real work: the
	                         * counter is not advancing -> keep reps at 1 instead
	                         * of exploding to the max (defensive anti-hang). */
		*iter = 1u;
		return;
	}
	per = total / init;     /* now per >= 1 */
	it = target / per;
	if (it < 1u)
		it = 1u;
	if (it > MEMBENCH_MAX_ITERS)
		it = MEMBENCH_MAX_ITERS;
	*iter = it;
}

/* Run fn (warm-up discarded) and return the min cycle count over tick-clean
 * runs; falls back to min-of-all if no clean run is found (slow single passes). */
static uint32_t timed_min(work_fn fn, void *ctx)
{
	uint32_t best_clean = 0xFFFFFFFFu, best_any = 0xFFFFFFFFu;
	uint32_t clean = 0u, i;

	fn(ctx);                            /* warm-up (cache/PLL settle) -> discard */
	for (i = 0u; i < MEMBENCH_TRIALS && clean < MEMBENCH_CLEAN; i++) {
		uint32_t t0 = HAL_GetTick();
		uint32_t c0, c1, t1, dc;

		__DSB(); __ISB();
		c0 = DWT->CYCCNT;
		fn(ctx);
		__DSB(); __ISB();
		c1 = DWT->CYCCNT;
		t1 = HAL_GetTick();

		dc = c1 - c0;
		if (dc < best_any)
			best_any = dc;
		if (t1 == t0) {                 /* no SysTick fired across the run */
			clean++;
			if (dc < best_clean)
				best_clean = dc;
		}
	}
	return clean ? best_clean : best_any;
}

/* ---- bandwidth (bw_mem) ----------------------------------------------------- */

struct bw_ctx {
	const volatile uint32_t *src;
	volatile uint32_t       *dst;
	uint32_t                 words;   /* words touched per scan */
	uint32_t                 reps;    /* scans per timed run    */
};

static void bw_read_work(void *vctx)
{
	struct bw_ctx *c = (struct bw_ctx *)vctx;
	const volatile uint32_t *p = c->src;
	uint32_t words = c->words, reps = c->reps, r, i, acc = 0u;

	for (r = 0u; r < reps; r++) {
		for (i = 0u; i + 8u <= words; i += 8u) {
			acc += p[i + 0] + p[i + 1] + p[i + 2] + p[i + 3];
			acc += p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
		}
		for (; i < words; i++)
			acc += p[i];
	}
	g_sink = acc;
}

static void bw_write_work(void *vctx)
{
	struct bw_ctx *c = (struct bw_ctx *)vctx;
	volatile uint32_t *p = c->dst;
	uint32_t words = c->words, reps = c->reps, r, i;
	uint32_t v = g_sink + 0x9E3779B9u;   /* derive from volatile -> not const-folded */

	for (r = 0u; r < reps; r++) {
		for (i = 0u; i + 8u <= words; i += 8u) {
			p[i + 0] = v; p[i + 1] = v; p[i + 2] = v; p[i + 3] = v;
			p[i + 4] = v; p[i + 5] = v; p[i + 6] = v; p[i + 7] = v;
		}
		for (; i < words; i++)
			p[i] = v;
	}
}

static void bw_copy_work(void *vctx)
{
	struct bw_ctx *c = (struct bw_ctx *)vctx;
	const volatile uint32_t *s = c->src;
	volatile uint32_t *d = c->dst;
	uint32_t words = c->words, reps = c->reps, r, i;

	for (r = 0u; r < reps; r++)
		for (i = 0u; i < words; i++)
			d[i] = s[i];
}

static uint32_t bw_mbps(uint32_t cycles, uint64_t bytes, uint32_t hclk)
{
	if (cycles == 0u)
		return 0u;
	return (uint32_t)((bytes * (uint64_t)hclk) / ((uint64_t)cycles * 1000000ULL));
}

/* ---- latency (lat_mem_rd) --------------------------------------------------- */

struct lat_ctx {
	const volatile uint32_t *buf;
	uint32_t                 k;    /* chase accesses per timed run */
};

/* Build a single-cycle chase over the working set as WORD INDICES rather than
 * raw pointers (a void-pointer chain through a uint32_t array would be a
 * strict-aliasing violation): nodes are 64 B = 16 words apart (>= the 32 B cache
 * line, so each touches a distinct line), linked i -> i+1 (mod n).  buf[node*16]
 * holds the next node's word index.  Deterministic (no RNG), reproducible. */
static void build_chase(uint32_t *buf, uint32_t wss_bytes)
{
	uint32_t n = wss_bytes / 64u, i;

	if (n == 0u)
		n = 1u;
	for (i = 0u; i < n; i++)
		buf[i * 16u] = ((i + 1u) % n) * 16u;   /* next node's word index */
}

static void lat_work(void *vctx)
{
	struct lat_ctx *c = (struct lat_ctx *)vctx;
	const volatile uint32_t *b = c->buf;
	uint32_t idx = 0u, k = c->k;

	while (k--)                          /* each load address depends on the */
		idx = b[idx];               /* previous result -> serialized chain */
	g_sink = idx;
}

/* Tenths of a nanosecond per access: cycles/k accesses at hclk. */
static uint32_t lat_ns_tenths(uint32_t cycles, uint32_t k, uint32_t hclk)
{
	if (k == 0u || hclk == 0u)
		return 0u;
	return (uint32_t)(((uint64_t)cycles * 10000000000ULL) / ((uint64_t)hclk * k));
}

/* ---- formatting / rows ------------------------------------------------------ */

static void fmt_u(char *buf, size_t n, uint32_t v)
{
	snprintf(buf, n, "%lu", (unsigned long)v);
}

static void fmt_ns(char *buf, size_t n, uint32_t tenths)
{
	snprintf(buf, n, "%lu.%lu", (unsigned long)(tenths / 10u),
	         (unsigned long)(tenths % 10u));
}

/* One bandwidth row: read always; write/copy only when `writable` (Flash is RO).
 * read/write span `words`; copy moves the first half into the second half, so
 * its denominator is the actually-copied bytes (half the span). */
static void bw_row(struct cli_instance *sh, const char *label, uint32_t *base,
                   uint32_t words, uint32_t hclk, int writable)
{
	struct bw_ctx c;
	char rds[12], wrs[12], cps[12];
	uint32_t target = hclk / MEMBENCH_TARGET_DIV;

	c.src = base; c.dst = base; c.words = words; c.reps = 1u;
	calibrate(bw_read_work, &c, &c.reps, 1u, target);
	fmt_u(rds, sizeof rds,
	      bw_mbps(timed_min(bw_read_work, &c), (uint64_t)words * 4u * c.reps, hclk));

	if (writable) {
		uint32_t half = words / 2u;

		c.src = base; c.dst = base; c.words = words; c.reps = 1u;
		calibrate(bw_write_work, &c, &c.reps, 1u, target);
		fmt_u(wrs, sizeof wrs,
		      bw_mbps(timed_min(bw_write_work, &c), (uint64_t)words * 4u * c.reps, hclk));

		c.src = base; c.dst = base + half; c.words = half; c.reps = 1u;
		calibrate(bw_copy_work, &c, &c.reps, 1u, target);
		fmt_u(cps, sizeof cps,
		      bw_mbps(timed_min(bw_copy_work, &c), (uint64_t)half * 4u * c.reps, hclk));
	} else {
		strcpy(wrs, "--");
		strcpy(cps, "--");
	}

	cli_print(sh, "  %-22s %8s %8s %8s\r\n", label, rds, wrs, cps);
}

static uint32_t lat_ns10(uint32_t *buf, uint32_t wss_bytes, uint32_t hclk)
{
	struct lat_ctx c;
	uint32_t target = hclk / MEMBENCH_TARGET_DIV;

	build_chase(buf, wss_bytes);
	c.buf = buf;
	c.k = 256u;
	calibrate(lat_work, &c, &c.k, 256u, target);
	return lat_ns_tenths(timed_min(lat_work, &c), c.k, hclk);
}

/* ---- command ---------------------------------------------------------------- */

static int cmd_membench(struct cli_instance *sh, int argc, char **argv)
{
	static const uint32_t wss_kb[] = { 1u, 2u, 4u, 8u, 16u, 32u, 64u };
	int do_dtcm = 1, do_sram = 1, do_sdram = 1, do_flash = 1;
	uint32_t hclk, i;
	int sdram_ok;
	void     *sram_raw = NULL;   /* malloc base (freed on every exit via `done`) */
	uint32_t *sram_bench_buf = NULL;   /* 32-byte-aligned working pointer         */

	if (argc >= 2) {
		const char *r = argv[1];

		do_dtcm = do_sram = do_sdram = do_flash = 0;
		if (!strcmp(r, "all"))        do_dtcm = do_sram = do_sdram = do_flash = 1;
		else if (!strcmp(r, "dtcm"))  do_dtcm = 1;
		else if (!strcmp(r, "sram"))  do_sram = 1;
		else if (!strcmp(r, "sdram")) do_sdram = 1;
		else if (!strcmp(r, "flash")) do_flash = 1;
		else {
			cli_error(sh, "membench: unknown region '%s' (dtcm|sram|sdram|flash|all)\r\n", r);
			return 1;
		}
	}

	if (dwt_enable() != 0) {
		cli_error(sh, "membench: DWT CYCCNT unavailable on this core\r\n");
		return 1;
	}
	hclk = HAL_RCC_GetHCLKFreq();
	sdram_ok = sdram_is_up();

	/* On-demand SRAM buffer (issue #94): malloc'd from the heap (which lives in
	 * internal SRAM, so it is the right region to measure) instead of a permanent
	 * 32 KB .bss array.  Over-allocate by 32 to hand the bench a cache-line-aligned
	 * pointer.  On failure, skip the SRAM rows rather than run on a null buffer. */
	if (do_sram) {
		sram_raw = malloc(SRAM_BENCH_BYTES + 32u);
		if (sram_raw == NULL) {
			cli_warn(sh, "membench: no heap for the 32 KB SRAM buffer; "
			             "skipping SRAM rows\r\n");
			do_sram = 0;
		} else {
			sram_bench_buf = (uint32_t *)
				(((uintptr_t)sram_raw + 31u) & ~(uintptr_t)31u);
		}
	}

	cli_print(sh, "DWT CYCCNT @%luMHz; warm-up + tick-guarded min; "
	          "D$=16KB/32B line; SDRAM/DTCM non-cacheable.\r\n\r\n",
	          (unsigned long)(hclk / 1000000u));

	/* bandwidth table */
	cli_print(sh, "%-24s %8s %8s %8s\r\n", "bandwidth (MB/s)", "read", "write", "copy");
	if (do_dtcm) {
		if (cli_cancel_requested(sh)) goto done;
		bw_row(sh, "DTCM   (16KB)", dtcm_bench_buf, DTCM_BENCH_BYTES / 4u, hclk, 1);
	}
	if (do_sram) {
		if (cli_cancel_requested(sh)) goto done;
		bw_row(sh, "SRAM   ( 4KB, cached)", sram_bench_buf, SRAM_CACHED_BYTES / 4u, hclk, 1);
		bw_row(sh, "SRAM   (32KB, refill)", sram_bench_buf, SRAM_BENCH_BYTES / 4u, hclk, 1);
	}
	if (do_sdram) {
		if (cli_cancel_requested(sh)) goto done;
		if (sdram_ok)
			bw_row(sh, "SDRAM  (64KB, non-cache)", sdram_bench_buf, SDRAM_BENCH_BYTES / 4u, hclk, 1);
		else
			cli_print(sh, "  %-22s %8s %8s %8s\r\n", "SDRAM  (64KB)", "down", "--", "--");
	}
	if (do_flash) {
		if (cli_cancel_requested(sh)) goto done;
		bw_row(sh, "Flash  (64KB, AXIM+L1D$)", (uint32_t *)FLASH_BASE, FLASH_BENCH_BYTES / 4u, hclk, 0);
	}

	/* latency curve (DTCM/SRAM/SDRAM; Flash has no pointer-chase row) */
	if (do_dtcm || do_sram || do_sdram) {
		cli_print(sh, "\r\nlatency (ns/access, dependent-load chain, 64B stride)\r\n");
		cli_print(sh, "  %-6s %7s %7s %7s\r\n", "WSS", "DTCM", "SRAM", "SDRAM");
		for (i = 0u; i < sizeof wss_kb / sizeof wss_kb[0]; i++) {
			uint32_t wb = wss_kb[i] * 1024u;
			char d[12], s[12], m[12], wlbl[8];

			if (cli_cancel_requested(sh))
				goto done;

			if (do_dtcm && wb <= DTCM_BENCH_BYTES)
				fmt_ns(d, sizeof d, lat_ns10(dtcm_bench_buf, wb, hclk));
			else
				strcpy(d, "--");

			if (do_sram && wb <= SRAM_BENCH_BYTES)
				fmt_ns(s, sizeof s, lat_ns10(sram_bench_buf, wb, hclk));
			else
				strcpy(s, "--");

			if (do_sdram && sdram_ok)
				fmt_ns(m, sizeof m, lat_ns10(sdram_bench_buf, wb, hclk));
			else
				strcpy(m, "--");

			snprintf(wlbl, sizeof wlbl, "%luKB", (unsigned long)wss_kb[i]);
			cli_print(sh, "  %-6s %7s %7s %7s\r\n", wlbl, d, s, m);
		}
	}

done:
	free(sram_raw);   /* NULL when SRAM was not benched (free(NULL) is a no-op) */
	return 0;
}

CLI_CMD_REGISTER(membench, NULL, "memory bandwidth + latency benchmark",
                 cmd_membench, 1, 1);
