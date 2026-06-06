/**
 * @file    app_shell.c
 * @brief   ThreadX Shell (CLI) bring-up app for STM32F746G-DISCO (issue #8).
 *
 * Brings the shell library up on real hardware over the ST-Link VCP and runs a
 * second, dummy-backed instance concurrently to demonstrate the multi-instance
 * architecture on silicon (req §10, acceptance §18.8):
 *
 *   - vcp_sh : interactive shell on USART1 (PA9/PB7, 115200 8N1).  Connect a
 *              terminal to /dev/ttyACM0; `help` lists commands, `echo` echoes.
 *   - dum_sh : a loopback (dummy) instance with no I/O pins.  A low-priority
 *              driver thread scripts it (inject -> process -> capture) and mirrors
 *              its transcript to the VCP via printf, so both instances are visibly
 *              running with their own independent state.
 *   - led    : LD1 (PI1) heartbeat, showing the shell coexists with other ThreadX
 *              threads.
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
#include "cli_backend_dummy.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

void tx_glue_timer_enable(void);

/* ---- shell instances --------------------------------------------------- */

/* Interactive VCP shell over the board's USART1 handle (brought up by bsp.c). */
CLI_BACKEND_UART_DEFINE(vcp_tr, &huart1);
CLI_INSTANCE_DEFINE(vcp_sh, &vcp_tr, "sh> ");

/* Second instance on the in-memory dummy backend (no pins); driven below. */
CLI_BACKEND_DUMMY_DEFINE(dum_tr);
CLI_INSTANCE_DEFINE(dum_sh, &dum_tr, "dum> ");

/* Every instance started at boot.  This is the compile-time gate the
 * requirements ask for (§4.2 / cli_instance.h): the live instance count must not
 * exceed the build's CLI_MAX_INSTANCES. */
static struct cli_instance *const shells[] = { &vcp_sh, &dum_sh };
#define SHELL_COUNT (sizeof(shells) / sizeof(shells[0]))
_Static_assert(SHELL_COUNT <= CLI_MAX_INSTANCES,
               "more shell instances than CLI_MAX_INSTANCES");

/* ---- background threads ------------------------------------------------- */

#define LED_STACK_SIZE     1024
#define DRIVER_STACK_SIZE  2048   /* printf needs headroom */

static TX_THREAD led_thread;
static TX_THREAD driver_thread;
static UCHAR     led_stack[LED_STACK_SIZE];
static UCHAR     driver_stack[DRIVER_STACK_SIZE];

/* Priorities.  The shell instance threads run at CLI_INSTANCE_PRIORITY (16).  The
 * heartbeat sits well above them (its work is brief).  The dummy driver runs
 * strictly BELOW the dummy shell thread so, under ThreadX strict-priority
 * preemption, it only touches the dummy's lock-free capture while that shell
 * thread is blocked on RX -- never concurrently with dummy_write() (see
 * driver_entry). */
#define LED_PRIORITY     10
#define DRIVER_PRIORITY  (CLI_INSTANCE_PRIORITY + 1)
_Static_assert(DRIVER_PRIORITY <= 31, "DRIVER_PRIORITY out of ThreadX range");

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

/*
 * Dummy-instance driver.  Every ~3 s it feeds the dummy shell one scripted line
 * and mirrors what that shell produced to the VCP, so an observer sees a second
 * instance running with its own state.
 *
 * Runs at DRIVER_PRIORITY, strictly below the dummy shell thread, which makes the
 * lock-free dummy backend safe to share between the two: cli_dummy_inject() ends
 * in cli_transport_notify_rx(), which makes the higher-priority dummy thread
 * ready and preempts us at that call, so by the time inject() returns the line is
 * fully processed and that thread is blocked on RX again.  Hence
 * cli_dummy_clear_output() (before inject) and the capture snapshot (after) never
 * overlap the dummy thread's dummy_write().  The settle sleep is
 * belt-and-suspenders.  Any imperfection here can only garble this cosmetic
 * mirror -- never the VCP instance, whose state is wholly separate (req §10).
 */
static void driver_entry(ULONG arg)
{
	static const char *const scripts[] = { "help\r", "echo hi\r" };
	unsigned i = 0;

	(void)arg;
	for (;;) {
		const char *cmd = scripts[i++ % (sizeof scripts / sizeof scripts[0])];

		cli_dummy_clear_output(&dum_tr);
		cli_dummy_inject(&dum_tr, cmd, strlen(cmd));
		tx_thread_sleep(10);            /* settle: let the dummy thread finish */

		/* Strip the trailing CR for the "sent" label; mirror the rest verbatim
		 * (the transcript already carries its own CR/LF + dum> prompt). */
		printf("[dummy] sent '%.*s' ->\r\n%s\r\n",
		       (int)(strlen(cmd) - 1u), cmd, cli_dummy_output_str(&dum_tr));

		tx_thread_sleep(3000);
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

	/* Only drive the dummy instance if it actually came up.  If its cli_init/
	 * cli_start failed above it was skipped, and its event flags may not exist --
	 * so cli_dummy_inject()'s cli_transport_notify_rx() would poke an uncreated
	 * object.  Gating on CLI_STARTED (set by cli_start) keeps the failure
	 * contained (req §9); the VCP shell is unaffected either way. */
	if (dum_sh.state == CLI_STARTED)
		tx_thread_create(&driver_thread, "dummy_drv", driver_entry, 0,
		                 driver_stack, sizeof driver_stack,
		                 DRIVER_PRIORITY, DRIVER_PRIORITY, TX_NO_TIME_SLICE,
		                 TX_AUTO_START);
	else
		printf("shell: dummy instance not started; driver skipped\r\n");

	/* Timer lists exist now: let the SysTick ISR drive ThreadX. */
	tx_glue_timer_enable();
}

int main(void)
{
	bsp_init();

	/* Early banner over the polling _write fallback (the console is not enabled
	 * until the VCP instance thread runs after tx_kernel_enter). */
	printf("\r\nThreadX Shell demo (issue #8)\r\n");
	printf("VCP shell on USART1 @115200 8N1; type 'help'.\r\n");

	tx_kernel_enter();   /* does not return */

	return 0;
}
