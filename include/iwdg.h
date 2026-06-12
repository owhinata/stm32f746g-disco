/**
 * @file    iwdg.h
 * @brief   IWDG independent watchdog driver + compile-time gate (issue #38).
 */
#ifndef IWDG_H
#define IWDG_H

/* Compile-time gate for the IWDG watchdog (issue #38).  1 == IWDG armed + petter
 * thread + LSI enabled + DBGMCU freeze + `wdt starve`; 0 == compiled out entirely
 * (no IWDG symbols, LSI left disabled, `wdt info` reports "disabled (build)").
 * The threadx build forwards the CMake option of the same name to this define via
 * bsp_iface (so it reaches src/bsp.c in the `common` library too); this #ifndef
 * default is the fall-back for targets that do not (e.g. host tests). */
#ifndef BSP_ENABLE_IWDG
#define BSP_ENABLE_IWDG 0
#endif

_Static_assert(BSP_ENABLE_IWDG == 0 || BSP_ENABLE_IWDG == 1,
               "BSP_ENABLE_IWDG must be 0 or 1");

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the IWDG: LSI-clocked, prescaler /64, reload 1499 -- ~3.0 s timeout at the
 * 32 kHz typical LSI (2.04 s at the 47 kHz fast corner, 5.65 s at the 17 kHz slow
 * corner).  HAL_IWDG_Init() performs the first refresh itself.  Call once, after
 * all bring-up, from tx_application_define() just before tx_glue_timer_enable()
 * (issue #38): QSPI/SD HAL init (up to 5 s blocking on a media fault) must complete
 * BEFORE the watchdog arms, so a media fault stays fail-soft instead of becoming a
 * reset loop.  Compiled to nothing when BSP_ENABLE_IWDG == 0.
 */
void iwdg_init(void);

/**
 * Refresh (pet) the IWDG counter -- a single IWDG->KR write, safe from thread, ISR
 * or fault context.  The priority-5 petter thread calls this ~1 s; the fault halt
 * loop calls it only while a debugger is attached.
 */
void iwdg_refresh(void);

/**
 * Non-zero if HAL_IWDG_Init() reported an error in iwdg_init().  The watchdog may
 * still be armed (HAL starts it before polling the prescaler/reload update), so
 * `wdt info` reports "init failed (may be armed)" rather than "disabled".
 */
int iwdg_init_failed(void);

#ifdef __cplusplus
}
#endif

#endif /* IWDG_H */
