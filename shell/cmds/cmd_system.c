/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_system.c
 * @brief   System built-in shell commands (issue #12): version / uptime / reboot.
 *
 * These join help/echo (cmd_builtin.c) in the `shell` executable only -- they are
 * never linked into the host test harness, which keeps its own command set.  All
 * three read board state through the standard buffered output API and touch only
 * the shell instance passed to them, so they stay reentrant across instances
 * (req §10).
 *
 * version / uptime read hardware identity and the millisecond tick straight from
 * CMSIS register macros and the HAL; reboot is a *dangerous* command (spec §12),
 * compiled in only when CLI_ENABLE_DANGEROUS_CMDS is set (default ON for the demo,
 * forwarded from the CMake option of the same name).  With it off the handler and
 * its registration vanish, so reboot leaves .shell_root_cmds and disappears from
 * help and completion.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "cli_version.h"     /* CLI_FW_NAME / CLI_FW_VERSION / CLI_GIT_DESC (generated) */

#include "stm32f7xx_hal.h"   /* HAL_GetTick, HAL_NVIC_SystemReset, DBGMCU, *_BASE */
#include "tx_api.h"          /* THREADX_*_VERSION, tx_thread_sleep */

#include <stdint.h>

/*
 * version: firmware identity + build + ThreadX + this STM32F746's silicon id.
 * IDCODE fields and the UID / flash-size register bases come from CMSIS macros
 * (RM0385 §40.6.1 DBGMCU_IDCODE, §41.1 unique id, §41.2 flash size).  Every
 * 32-bit value is printed via an unsigned long cast since cli_print's %l* path
 * is long-width.
 */
static int cmd_version(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t idcode = DBGMCU->IDCODE;
	uint32_t devid  = idcode & DBGMCU_IDCODE_DEV_ID_Msk;
	uint32_t rev    = (idcode & DBGMCU_IDCODE_REV_ID_Msk) >> DBGMCU_IDCODE_REV_ID_Pos;
	const volatile uint32_t *uid = (const volatile uint32_t *)UID_BASE;
	uint32_t flash_kb = *(const volatile uint16_t *)FLASHSIZE_BASE;

	(void)argc;
	(void)argv;

	cli_print(sh, "%s v%s (%s)\r\n", CLI_FW_NAME, CLI_FW_VERSION, CLI_GIT_DESC);
	cli_print(sh, "Built:    %s %s\r\n", __DATE__, __TIME__);
	cli_print(sh, "ThreadX:  %u.%u.%u\r\n",
	          THREADX_MAJOR_VERSION, THREADX_MINOR_VERSION, THREADX_PATCH_VERSION);
	cli_print(sh, "MCU:      STM32F746 (devid 0x%03lx rev 0x%04lx)\r\n",
	          (unsigned long)devid, (unsigned long)rev);
	cli_print(sh, "Flash:    %lu KB\r\n", (unsigned long)flash_kb);
	cli_print(sh, "UID:      0x%08lx 0x%08lx 0x%08lx\r\n",
	          (unsigned long)uid[0], (unsigned long)uid[1], (unsigned long)uid[2]);
	return 0;
}

/*
 * uptime: elapsed time since boot from the 1 kHz HAL millisecond tick
 * (HAL_GetTick, fed by SysTick -> HAL_IncTick in tx_glue.c).  The raw counter is
 * uint32_t, so it wraps after ~49.7 days -- noted in help and docs.
 */
static int cmd_uptime(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t ms   = HAL_GetTick();
	uint32_t secs = ms / 1000u;
	uint32_t days = secs / 86400u;
	uint32_t hh   = (secs % 86400u) / 3600u;
	uint32_t mm   = (secs % 3600u) / 60u;
	uint32_t ss   = secs % 60u;

	(void)argc;
	(void)argv;

	cli_print(sh, "up %lud %02lu:%02lu:%02lu (%lu ms)\r\n",
	          (unsigned long)days, (unsigned long)hh, (unsigned long)mm,
	          (unsigned long)ss, (unsigned long)ms);
	return 0;
}

#if CLI_ENABLE_DANGEROUS_CMDS
/*
 * reboot: immediate software reset via SCB->AIRCR SYSRESETREQ (HAL_NVIC_SystemReset,
 * RM0385 §5.1.1).  No confirmation prompt -- spec §12 keeps that out of scope.
 *
 * cli_print enqueues into the IRQ-driven UART TX ring and returns before the
 * bytes leave the wire, so sleep briefly to let the ring drain before the reset
 * truncates the message.  50 ms covers a worst-case full 512 B ring at 115200 8N1
 * (~44 ms); an interactive prompt's ring is near empty, so this is best-effort
 * headroom on a terminal path where latency is irrelevant.  Interrupts stay
 * enabled during the sleep -- the TX-complete IRQ is what drains the ring.
 */
static int cmd_reboot(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;

	cli_print(sh, "rebooting...\r\n");
	tx_thread_sleep(50);          /* ~50 ms: let the UART TX ring flush */
	HAL_NVIC_SystemReset();       /* does not return */
	return 0;                     /* unreachable */
}
#endif /* CLI_ENABLE_DANGEROUS_CMDS */

CLI_CMD_REGISTER(version, NULL, "show firmware/MCU version", cmd_version, 1, 0);
CLI_CMD_REGISTER(uptime,  NULL, "show uptime since boot (~49.7d wrap)", cmd_uptime, 1, 0);
#if CLI_ENABLE_DANGEROUS_CMDS
CLI_CMD_REGISTER(reboot,  NULL, "reboot the board (immediate)", cmd_reboot, 1, 0);
#endif
