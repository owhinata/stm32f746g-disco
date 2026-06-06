/**
 * @file    tm_svc.c
 * @brief   SVC handler glue for the Thread-Metric interrupt tests.
 *
 * The Thread-Metric interrupt tests raise an interrupt via TM_CAUSE_INTERRUPT,
 * which on Cortex-M is `SVC #0` (see tm_porting_layer.h). The SVCall handler
 * must invoke tm_interrupt_handler(). Only linked for the interrupt tests; the
 * non-interrupt tests leave SVC_Handler as the startup default (unused).
 */
void tm_interrupt_handler(void);

void SVC_Handler(void)
{
    tm_interrupt_handler();
}
