/**
 * @file    main.c
 * @brief   STM32F746G-DISCO single firmware: the interactive ThreadX CLI shell.
 *
 * This is the one and only application (CMake target `threadx`).  It brings the
 * shell library up on real hardware over the ST-Link VCP; everything else --
 * including the CoreMark benchmark -- runs as a shell command, not a separate
 * image (see shell/cmds/cmd_coremark.c).
 *
 *   - vcp_sh : interactive shell on USART1 (PA9/PB7, 115200 8N1).  Connect a
 *              terminal to /dev/ttyACM0; it has the full line editor (cursor
 *              motion, in-line edit, meta keys, VT100, terminal-width wrap,
 *              colour).  `help` lists commands (version/uptime/reboot/thread/
 *              devmem/coremark/help/echo/...).
 *   - led    : LD1 (PI1) heartbeat, showing the shell coexists with other ThreadX
 *              threads (it keeps blinking even while `coremark` runs).
 *
 * The multi-instance architecture (req §10, acceptance §18.8) is exercised by the
 * host tests (two dummy instances, no crosstalk) and was demonstrated on silicon
 * in #8; the demo keeps a single interactive instance so the VCP carries a clean,
 * uninterrupted line-editing session (no second instance mirroring over the same
 * UART).  The dummy backend remains a first-class, host-tested backend in the
 * library.
 *
 * Board bring-up (216 MHz clock, caches, VCP UART, printf) is in bsp.c; the
 * SysTick/ThreadX integration is in port/threadx/tx_glue.c; the strong _write
 * that gives USART1 a single owner is in shell/backend/cli_backend_uart.c.
 *
 * Clean-room design; no third-party code reused.
 */
#include "tx_api.h"
#include "main.h"
#include "bsp.h"

#include "cli_instance.h"
#include "cli_backend_uart.h"
#include "qspi_flash.h"
#include "fs_glue.h"
#include "sd_card.h"

#include <stddef.h>
#include <stdio.h>

void tx_glue_timer_enable(void);
void tx_glue_profile_enable(void);   /* issue #19: arm exec-profile ISR hooks */

/* ---- shell instances --------------------------------------------------- */

/* Interactive VCP shell over the board's USART1 handle (brought up by bsp.c). */
CLI_BACKEND_UART_DEFINE(vcp_tr, &huart1);
CLI_INSTANCE_DEFINE(vcp_sh, &vcp_tr, "sh> ");

/* Every instance started at boot.  The array + compile-time gate is kept (even at
 * one instance) as the §4.2 requirement: the live instance count must not exceed
 * the build's CLI_MAX_INSTANCES.  Add more transports here to run them at once. */
static struct cli_instance *const shells[] = { &vcp_sh };
#define SHELL_COUNT (sizeof(shells) / sizeof(shells[0]))
_Static_assert(SHELL_COUNT <= CLI_MAX_INSTANCES,
               "more shell instances than CLI_MAX_INSTANCES");

/* ---- background threads ------------------------------------------------- */

#define LED_STACK_SIZE  1024

static TX_THREAD led_thread;
static UCHAR     led_stack[LED_STACK_SIZE];

/* Priorities.  The shell instance thread runs at CLI_INSTANCE_PRIORITY (16); the
 * heartbeat sits well above it (its work is brief). */
#define LED_PRIORITY  10

static void led_entry(ULONG arg)
{
	GPIO_InitTypeDef g = {0};

	(void)arg;

	/* First application thread to run (priority 10) -- this point is past
	 * _tx_execution_initialize(), so it is the earliest safe spot to arm the
	 * execution-profile ISR hooks (issue #19).  See tx_glue.c profile_active. */
	tx_glue_profile_enable();

	LD1_GPIO_CLK_EN();
	g.Pin   = LD1_PIN;
	g.Mode  = GPIO_MODE_OUTPUT_PP;
	g.Pull  = GPIO_NOPULL;
	g.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(LD1_GPIO_PORT, &g);

	for (;;) {
		HAL_GPIO_TogglePin(LD1_GPIO_PORT, LD1_PIN);
		tx_thread_sleep(250);   /* 250 ms (1 tick == 1 ms) */
	}
}

/* Called by ThreadX during tx_kernel_enter() to create the application. */
void tx_application_define(void *first_unused_memory)
{
	(void)first_unused_memory;

	/* Bring up every shell instance.  cli_init (backend / ThreadX object create)
	 * and cli_start (tx_thread_create) can each fail; on either, disable just
	 * that instance and keep going -- one instance's failure must not stop the
	 * rest of the system (req §9 fail-safe).  (A later enable() failure is handled
	 * inside the started thread, which uninits and exits; nothing to do here.) */
	for (size_t i = 0; i < SHELL_COUNT; i++) {
		if (cli_init(shells[i]) != 0) {
			printf("shell: instance %u init failed (skipped)\r\n", (unsigned)i);
			continue;
		}
		if (cli_start(shells[i]) != 0)
			printf("shell: instance %u start failed (skipped)\r\n", (unsigned)i);
	}

	/* Background-job worker pool (issue #25): create the per-worker event groups
	 * once, now that the interactive instances exist.  Workers are spawned on
	 * demand by `cmd &`. */
	cli_job_pool_init();

	tx_thread_create(&led_thread, "led", led_entry, 0,
	                 led_stack, sizeof led_stack,
	                 LED_PRIORITY, LED_PRIORITY, TX_NO_TIME_SLICE, TX_AUTO_START);

	/* QSPI NOR bring-up (issue #29): peripheral + operation mutex only -- no
	 * flash transaction, so it is safe before the scheduler starts.  On failure
	 * the `qspi` command reports "driver not initialized"; nothing else stops. */
	if (qspi_flash_init() != 0)
		printf("qspi: init failed (qspi command disabled)\r\n");

	/* Filesystem glue (issue #30): mount mutex + fx_system_initialize only;
	 * the media itself mounts lazily on the first `fs` command. */
	fs_glue_init();

	/* SDMMC1 SD-card bring-up (issue #33): GPIO/DMA/NVIC + operation mutex and
	 * DMA-completion semaphore only -- no card I/O, so it is safe before the
	 * scheduler starts.  On failure the `sd` command reports "driver not
	 * initialized"; nothing else stops. */
	if (sd_card_init() != 0)
		printf("sd: init failed (sd command disabled)\r\n");

	/* Timer lists exist now: let the SysTick ISR drive ThreadX. */
	tx_glue_timer_enable();
}

int main(void)
{
	bsp_init();

	/* Early banner over the polling _write fallback (the console is not enabled
	 * until the VCP instance thread runs after tx_kernel_enter). */
	printf("\r\nThreadX Shell -- STM32F746G-DISCO\r\n");
	printf("VCP on USART1 @115200 8N1; type 'help' (try 'coremark').\r\n");

	tx_kernel_enter();   /* does not return */

	return 0;
}
