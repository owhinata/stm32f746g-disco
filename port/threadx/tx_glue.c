/*
 * ThreadX low-level glue for STM32F746G-DISCO (Cortex-M7 / GNU).
 *
 * Integration choices:
 *  - SysTick is owned by the HAL (1 ms) and shared with ThreadX: the SysTick
 *    handler increments the HAL tick and, once ThreadX is initialized, calls
 *    _tx_timer_interrupt().  So HAL_GetTick()/HAL_Delay() and ThreadX both work.
 *  - PendSV (context switch) and SysTick run at the lowest interrupt priority.
 *  - ThreadX's PendSV_Handler comes from the port asm, so the firmware ships no
 *    src/stm32f7xx_it.c; SVC has no user here, and every other vector falls to
 *    the startup file's weak Default_Handler.
 */
#include "tx_api.h"
#include "stm32f7xx_hal.h"

extern VOID  _tx_timer_interrupt(VOID);
extern VOID *_tx_initialize_unused_memory;

/* The threads in this app own their stacks statically, so ThreadX never needs
   the "first unused memory" region; point it at a tiny valid buffer. */
static UCHAR tx_unused_memory[4];

/* Gate so the SysTick ISR does not poke ThreadX timer lists before they
   exist (SysTick is already ticking from bsp_init()/HAL_Init()). */
static volatile UINT tx_timer_active = 0u;

void _tx_initialize_low_level(void)
{
    /* PendSV (context switch) at the lowest priority.  SysTick MUST be a
       higher priority than PendSV: when no thread is ready, ThreadX idles by
       spinning inside the PendSV handler with interrupts enabled, waiting for
       the timer tick to make a thread ready.  If SysTick == PendSV priority it
       cannot preempt that spin, the tick never advances, and sleeping threads
       never wake (deadlock).  Critical sections use PRIMASK (CPSID i), so the
       higher SysTick priority is still masked safely inside ThreadX. */
    NVIC_SetPriority(PendSV_IRQn, 15);
    NVIC_SetPriority(SysTick_IRQn, 14);

    _tx_initialize_unused_memory = (VOID *)tx_unused_memory;
}

/* Called at the end of tx_application_define(), once the timer lists are set
   up by _tx_initialize_high_level(), to let the SysTick ISR drive ThreadX. */
void tx_glue_timer_enable(void)
{
    tx_timer_active = 1u;
}

/* Number of 1 ms SysTicks per ThreadX tick. Default 1 (1 kHz ThreadX tick).
   Override with -DTX_GLUE_TICK_DIV (e.g. 10 -> 100 Hz). Keep in sync with
   TX_TIMER_TICKS_PER_SECOND in tx_user.h. */
#ifndef TX_GLUE_TICK_DIV
#define TX_GLUE_TICK_DIV 1u
#endif

void SysTick_Handler(void)
{
    HAL_IncTick();   /* HAL timebase stays at 1 ms */

    if (tx_timer_active != 0u)
    {
        static UINT div = 0u;
        if (++div >= (UINT)TX_GLUE_TICK_DIV)
        {
            div = 0u;
            _tx_timer_interrupt();
        }
    }
}
