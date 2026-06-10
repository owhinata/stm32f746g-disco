/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    fault.c
 * @brief   Cortex-M7 fault handlers + crash dump (issue #28).
 *
 * Strong HardFault/MemManage/BusFault/UsageFault handlers override the CMSIS
 * startup weak aliases (which only spin in Default_Handler).  On a fault the
 * dump goes to two places, RAM log first so it survives even if the UART output
 * then wedges:
 *   1. RAM log (src/log.c): a couple of LOG_ERR lines -> reset-persistent, so
 *      `dmesg` after the next reset shows the crash.
 *   2. USART1 by polling (no HAL, no IRQ, no ThreadX) -> a full register / stack
 *      / backtrace dump even with the scheduler and interrupts dead.
 * Then the core halts in a busy loop (not WFI -- the old ST-Link cannot attach to
 * a sleeping core, #20/#24/#26); SWD can still inspect the halted state.
 *
 * The exception entry is captured by a small naked stub shared by all four
 * vectors (the type is read back from SCB->ICSR), which hands the C handler the
 * stacked frame, EXC_RETURN and the callee-saved registers.  Clean-room: concept
 * from NuttX armv7-m fault handlers / Zephyr log_panic; no code reused.
 */
#define LOG_TAG "fault"
#include "log.h"

#include <stdarg.h>
#include <stdint.h>

#include "cli_fmt.h"
#include "stm32f7xx_hal.h"
#include "tx_api.h"

/* ThreadX current-thread pointer (tx_thread.h is internal; declare it here). */
extern TX_THREAD *_tx_thread_current_ptr;

/* Linker symbols: .text bounds (backtrace scan) and the top of the MSP stack. */
extern uint32_t _stext;
extern uint32_t _etext;
extern uint32_t _estack;

/* ---- init -------------------------------------------------------------- */

void fault_init(void)
{
	/* Route MemManage/Bus/Usage to their own handlers instead of escalating to
	 * HardFault, so the dump can classify the cause precisely (PM0253 4.3.9). */
	SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk |
	              SCB_SHCSR_USGFAULTENA_Msk;
	/* Trap integer divide-by-zero as a UsageFault (cheap, catches a real bug).
	 * UNALIGN_TRP is left off: HAL/memcpy do intentional unaligned accesses. */
	SCB->CCR |= SCB_CCR_DIV_0_TRP_Msk;
	__DSB();
	__ISB();
}

/* ---- polling UART output (HAL/IRQ/ThreadX-free) ------------------------ */

static void fault_putc(void *ctx, char c)
{
	(void)ctx;
	if (!(USART1->CR1 & USART_CR1_UE))
		return;                         /* UART not up yet (very early fault) */
	while (!(USART1->ISR & USART_ISR_TXE))
		;
	USART1->TDR = (uint8_t)c;
}

static void fault_puts(const char *s)
{
	while (*s)
		fault_putc(NULL, *s++);
}

static void fault_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	cli_vformat(fault_putc, NULL, fmt, ap);
	va_end(ap);
}

/* ---- dump helpers ------------------------------------------------------ */

/* True for addresses in on-chip RAM (DTCM + SRAM1 + SRAM2, contiguous 320 KB):
 * the only place a live stack lives, so stack reads here can never bus-fault. */
static int addr_in_ram(uint32_t a)
{
	return a >= 0x20000000u && a < 0x20050000u;
}

static void fault_dump_stack(uint32_t sp, uint32_t top)
{
	uint32_t avail = (top > sp) ? (top - sp) : 0u;
	uint32_t len   = avail > 256u ? 256u : avail;
	if (len == 0u)
		len = 64u;                      /* sp past the clamp: show a little */

	fault_puts(" stack @sp:\r\n");
	for (uint32_t a = sp; a < sp + len; a += 16u) {
		if (!addr_in_ram(a))
			break;
		fault_printf("  %08lx:", (unsigned long)a);
		for (uint32_t j = 0u; j < 16u && addr_in_ram(a + j); j += 4u)
			fault_printf(" %08lx",
			             (unsigned long)(*(volatile uint32_t *)(a + j)));
		fault_puts("\r\n");
	}
}

/* Scan the stack for plausible return addresses: a Thumb-tagged word pointing
 * into .text whose preceding instruction is a BL (imm) or BLX (reg).  No frame
 * pointer is used (the firmware is built without one). */
static void fault_backtrace(uint32_t sp, uint32_t top, uint32_t pc, uint32_t lr)
{
	uint32_t tlo = (uint32_t)&_stext;
	uint32_t thi = (uint32_t)&_etext;
	int n = 0;

	fault_puts(" backtrace:\r\n");
	if (pc >= tlo && pc < thi)
		fault_printf("  #%d %08lx (pc)\r\n", n++, (unsigned long)pc);
	if ((lr & ~1u) >= tlo && (lr & ~1u) < thi)
		fault_printf("  #%d %08lx (lr)\r\n", n++, (unsigned long)(lr & ~1u));

	uint32_t scan = (top > sp) ? (top - sp) : 0u;
	if (scan > 1024u)
		scan = 1024u;                   /* bound the walk */

	for (uint32_t a = sp; a + 4u <= sp + scan && n < 16; a += 4u) {
		if (!addr_in_ram(a))
			break;
		uint32_t v = *(volatile uint32_t *)a;
		if (!(v & 1u))
			continue;                   /* Thumb return addresses are odd */
		uint32_t ia = v & ~1u;
		if (ia < tlo + 4u || ia >= thi)
			continue;                   /* not a .text address (room for -4) */

		uint16_t h1 = *(volatile uint16_t *)(ia - 4u);
		uint16_t h2 = *(volatile uint16_t *)(ia - 2u);
		int is_bl  = ((h1 & 0xF800u) == 0xF000u) && ((h2 & 0xD000u) == 0xD000u);
		int is_blx = ((h2 & 0xFF87u) == 0x4780u);
		if (is_bl || is_blx)
			fault_printf("  #%d %08lx\r\n", n++, (unsigned long)ia);
	}
}

/* ---- C fault handler --------------------------------------------------- */

void fault_handler_c(uint32_t *frame, uint32_t exc_return, uint32_t *cs_regs)
{
	static volatile uint32_t in_fault;

	__disable_irq();
	if (in_fault)
		for (;;)                        /* secondary fault while dumping: halt */
			;
	in_fault = 1u;

	/* The stacked frame may itself be unreadable on a stacking fault
	 * (MSTKERR/STKERR) or a wild SP (PM0253 2.5.1 / 4.3.10): validate the 8-word
	 * basic-frame span is in RAM before dereferencing it, else fall back to a
	 * registers-only dump so SCB status + EXC_RETURN still come out. */
	int frame_ok = addr_in_ram((uint32_t)frame) &&
	               addr_in_ram((uint32_t)frame + 31u);

	/* Basic exception frame R0-R3, R12, LR, PC, xPSR is always the lowest 8
	 * words of the stacked frame -- on an FPU-extended frame the S0-S15/FPSCR
	 * context is stacked ABOVE it, so frame[0..7] are correct either way
	 * (PM0253 2.4.7); only the frame SIZE differs, handled in the SP calc. */
	uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, r12 = 0;
	uint32_t lr = 0, pc = 0, xpsr = 0, sp = 0;
	if (frame_ok) {
		r0 = frame[0]; r1 = frame[1]; r2 = frame[2]; r3 = frame[3];
		r12 = frame[4]; lr = frame[5]; pc = frame[6]; xpsr = frame[7];

		/* SP at the fault = frame + frame size (basic 8 words, or 26-word
		 * FPU-extended frame when EXC_RETURN bit4 is clear) + 4 if the stacked
		 * xPSR bit9 flags STKALIGN padding. */
		sp = (uint32_t)frame + ((exc_return & 0x10u) ? 8u : 26u) * 4u;
		if (xpsr & (1u << 9))
			sp += 4u;
	}

	uint32_t cfsr  = SCB->CFSR;
	uint32_t hfsr  = SCB->HFSR;
	uint32_t mmfar = SCB->MMFAR;
	uint32_t bfar  = SCB->BFAR;
	int mm_valid   = (cfsr & SCB_CFSR_MMARVALID_Msk) != 0u;
	int bf_valid   = (cfsr & SCB_CFSR_BFARVALID_Msk) != 0u;

	uint32_t vect = SCB->ICSR & SCB_ICSR_VECTACTIVE_Msk;
	const char *name = (vect == 3u) ? "HardFault"  :
	                   (vect == 4u) ? "MemManage"  :
	                   (vect == 5u) ? "BusFault"   :
	                   (vect == 6u) ? "UsageFault" : "Fault";

	/* (1) RAM log first -- two lines, reset-persistent (issue #28). */
	LOG_ERR("%s cfsr=%08lx hfsr=%08lx mmfar=%08lx bfar=%08lx",
	        name, (unsigned long)cfsr, (unsigned long)hfsr,
	        (unsigned long)mmfar, (unsigned long)bfar);
	if (frame_ok)
		LOG_ERR("pc=%08lx lr=%08lx psr=%08lx sp=%08lx exc=%08lx",
		        (unsigned long)pc, (unsigned long)lr, (unsigned long)xpsr,
		        (unsigned long)sp, (unsigned long)exc_return);
	else
		LOG_ERR("frame lost (stacking fault?) frame=%08lx exc=%08lx",
		        (unsigned long)(uintptr_t)frame, (unsigned long)exc_return);

	/* (2) Full dump over USART1 by polling. */
	fault_printf("\r\n*** FAULT: %s ***\r\n", name);
	fault_printf(" CFSR=%08lx HFSR=%08lx MMFAR=%08lx%s BFAR=%08lx%s\r\n",
	             (unsigned long)cfsr, (unsigned long)hfsr,
	             (unsigned long)mmfar, mm_valid ? "(valid)" : "",
	             (unsigned long)bfar,  bf_valid ? "(valid)" : "");
	/* R4-R11 are on our handler MSP (we pushed them), so they print regardless. */
	fault_printf(" R4=%08lx R5=%08lx R6=%08lx R7=%08lx\r\n",
	             (unsigned long)cs_regs[0], (unsigned long)cs_regs[1],
	             (unsigned long)cs_regs[2], (unsigned long)cs_regs[3]);
	fault_printf(" R8=%08lx R9=%08lx R10=%08lx R11=%08lx\r\n",
	             (unsigned long)cs_regs[4], (unsigned long)cs_regs[5],
	             (unsigned long)cs_regs[6], (unsigned long)cs_regs[7]);
	fault_printf(" EXC_RETURN=%08lx frame=%08lx\r\n",
	             (unsigned long)exc_return, (unsigned long)(uintptr_t)frame);

	if (!frame_ok) {
		/* Stacked frame unreadable: nothing more we can trust to dump. */
		fault_puts(" stacked frame unavailable (stacking fault?)\r\n halted.\r\n");
		while (!(USART1->ISR & USART_ISR_TC) && (USART1->CR1 & USART_CR1_UE))
			;
		for (;;)
			;
	}

	fault_printf(" R0=%08lx R1=%08lx R2=%08lx R3=%08lx\r\n",
	             (unsigned long)r0, (unsigned long)r1,
	             (unsigned long)r2, (unsigned long)r3);
	fault_printf(" R12=%08lx SP=%08lx LR=%08lx PC=%08lx\r\n",
	             (unsigned long)r12, (unsigned long)sp,
	             (unsigned long)lr, (unsigned long)pc);
	fault_printf(" xPSR=%08lx\r\n", (unsigned long)xpsr);

	/* Clamp the stack scan: thread (PSP) -> current thread's stack_end if sp is
	 * inside it; main (MSP) -> _estack.  An insane sp falls back to a short dump. */
	uint32_t top = (uint32_t)&_estack;
	if (exc_return & 0x4u) {
		TX_THREAD *t = _tx_thread_current_ptr;
		if (t != NULL) {
			uint32_t s0 = (uint32_t)t->tx_thread_stack_start;
			uint32_t s1 = (uint32_t)t->tx_thread_stack_end;
			top = (sp >= s0 && sp <= s1) ? s1 : (sp + 64u);
		}
	}

	fault_dump_stack(sp, top);
	fault_backtrace(sp, top, pc, lr);

	fault_puts(" halted.\r\n");
	while (!(USART1->ISR & USART_ISR_TC) && (USART1->CR1 & USART_CR1_UE))
		;                               /* let the last bytes leave */

	for (;;)
		;                               /* halt; SWD can still attach (#26) */
}

/* ---- naked entry stubs ------------------------------------------------- */

/* One stub for all four fault vectors: select MSP/PSP from EXC_RETURN bit2,
 * pass the frame (r0), EXC_RETURN (r1) and a pointer to the saved callee-saved
 * registers (r2 = MSP after pushing r4-r11) to the C handler.  The C handler
 * reads the precise fault type from SCB->ICSR, so the stubs need not differ. */
__attribute__((naked)) void HardFault_Handler(void)
{
	__asm volatile(
		"tst   lr, #4            \n"
		"ite   eq                \n"
		"mrseq r0, msp           \n"
		"mrsne r0, psp           \n"
		"mov   r1, lr            \n"
		"push  {r4-r11}          \n"
		"mov   r2, sp            \n"
		"b     fault_handler_c   \n");
}
void MemManage_Handler(void)  __attribute__((alias("HardFault_Handler")));
void BusFault_Handler(void)   __attribute__((alias("HardFault_Handler")));
void UsageFault_Handler(void) __attribute__((alias("HardFault_Handler")));
