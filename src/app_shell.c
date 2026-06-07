/**
 * @file    app_shell.c
 * @brief   ThreadX Shell (CLI) demo app for STM32F746G-DISCO (issues #8/#9).
 *
 * Brings the shell library up on real hardware over the ST-Link VCP:
 *
 *   - vcp_sh : interactive shell on USART1 (PA9/PB7, 115200 8N1).  Connect a
 *              terminal to /dev/ttyACM0; it has the full #9 line editor (cursor
 *              motion, in-line edit, meta keys, VT100, terminal-width wrap,
 *              colour).  `help` lists commands, `echo` echoes.
 *   - led    : LD1 (PI1) heartbeat, showing the shell coexists with other ThreadX
 *              threads.
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

#include <stddef.h>
#include <stdio.h>

void tx_glue_timer_enable(void);

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

	tx_thread_create(&led_thread, "led", led_entry, 0,
	                 led_stack, sizeof led_stack,
	                 LED_PRIORITY, LED_PRIORITY, TX_NO_TIME_SLICE, TX_AUTO_START);

	/* Timer lists exist now: let the SysTick ISR drive ThreadX. */
	tx_glue_timer_enable();
}

int main(void)
{
	bsp_init();

	/* Early banner over the polling _write fallback (the console is not enabled
	 * until the VCP instance thread runs after tx_kernel_enter). */
	printf("\r\nThreadX Shell demo (issues #8/#9)\r\n");
	printf("VCP shell on USART1 @115200 8N1; type 'help'.\r\n");

	tx_kernel_enter();   /* does not return */

	return 0;
}
