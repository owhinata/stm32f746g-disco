/*
 * Linker script for STM32F746NGH6 (STM32F746G-DISCO).
 *   FLASH : 1024 KB @ 0x08000000
 *   DTCM  :   64 KB @ 0x20000000 (tightly coupled, not D-cached)
 *   RAM   :  256 KB @ 0x20010000 (SRAM1 240K + SRAM2 16K, contiguous, D-cached)
 *
 * The physical RAM is one contiguous 320 KB block from 0x20000000, but it is
 * split here into two regions (issue #33): the reset-persistent log lives in
 * DTCM, while .data/.bss/heap/stack and the SDMMC DMA bounce buffer live in
 * SRAM1+SRAM2 (RAM).  _estack = ORIGIN(RAM)+LENGTH(RAM) = 0x20050000, the same
 * top-of-RAM value as the previous single-region layout, so the MSP is unmoved.
 *
 * Symbols (_estack, _sdata, _edata, _sidata, _sbss, _ebss) match the CMSIS
 * gcc startup file startup_stm32f746xx.s.
 */

ENTRY(Reset_Handler)

/* Top of RAM -> initial stack pointer */
_estack = ORIGIN(RAM) + LENGTH(RAM);

_Min_Heap_Size  = 0x200;   /* required heap  */
_Min_Stack_Size = 0x400;   /* required stack */

MEMORY
{
  DTCM  (xrw) : ORIGIN = 0x20000000, LENGTH = 64K
  RAM   (xrw) : ORIGIN = 0x20010000, LENGTH = 256K   /* SRAM1 240K + SRAM2 16K */
  SDRAM (xrw) : ORIGIN = 0xC0000000, LENGTH = 8M     /* FMC bank1, 16-bit bus (issue #40) */
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
}

SECTIONS
{
  /* Interrupt vector table at the start of FLASH */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  /* Program code and read-only data */
  .text :
  {
    . = ALIGN(4);
    _stext = .;                 /* .text start, used by fault.c backtrace scan */
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)

    KEEP(*(.init))
    KEEP(*(.fini))

    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  /* Shell (CLI) root command table: const cli_cmd entries registered from any
   * TU via CLI_CMD_REGISTER().  KEEP survives --gc-sections; SORT_BY_NAME gives
   * a deterministic (alphabetical) order; entries carry aligned(__alignof__) so
   * the array is gap-free and walkable by pointer arithmetic between the two
   * boundary symbols.  Empty (start==end) when no command is registered. */
  .shell_root_cmds (READONLY) :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__cli_root_cmds_start = .);
    KEEP (*(SORT_BY_NAME(.shell_root_cmds.*)))
    KEEP (*(.shell_root_cmds))
    PROVIDE_HIDDEN (__cli_root_cmds_end = .);
    . = ALIGN(4);
  } >FLASH

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH

  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH

  .fini_array :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  /* RAM log ring (issue #28): NOLOAD so the CMSIS Reset_Handler .data copy /
   * .bss clear never touches it -- contents survive any system reset (reboot /
   * fault / IWDG / WWDG / NRST / LPWR, RM0385 5.1.1).  Placed in the DTCM region
   * (0x20000000..0x2000FFFF), which bypasses the D-cache: log writes from fault
   * context are committed without cache maintenance and are always visible to
   * SWD.  Since #33 the DTCM is its own MEMORY region, so this placement is
   * explicit (no longer relying on "first in RAM").  Power reset (POR/PDR/BOR,
   * 5.1.2) does not guarantee SRAM retention -- log_init() validates the magic
   * and re-initialises if invalid. */
  .log_noinit (NOLOAD) :
  {
    . = ALIGN(4);
    _slog_noinit = .;
    KEEP(*(.log_noinit))
    . = ALIGN(4);
    _elog_noinit = .;
  } >DTCM

  ASSERT(_elog_noinit <= ORIGIN(DTCM) + LENGTH(DTCM),
         ".log_noinit must fit inside the 64 KB DTCM")

  /* membench DTCM benchmark buffer (issue #57): NOLOAD so the startup .data
   * copy / .bss clear never touches it (the bench fills it itself).  Placed in
   * the DTCM region after .log_noinit so the `membench` command can measure the
   * tightly-coupled, non-D-cached DTCM (which otherwise holds only the log ring).
   * Sized 16 KB: 0x2020 (.log_noinit) + 0x4000 = 0x6020, well inside the 64 KB
   * DTCM.  Present only when the buffer is linked in (the shell build); the
   * input section is empty otherwise. */
  .dtcm_bench (NOLOAD) :
  {
    . = ALIGN(32);
    _sdtcm_bench = .;
    KEEP(*(.dtcm_bench))
    . = ALIGN(32);
    _edtcm_bench = .;
  } >DTCM

  ASSERT(_edtcm_bench <= ORIGIN(DTCM) + LENGTH(DTCM),
         ".dtcm_bench must fit inside the 64 KB DTCM (after .log_noinit)")

  /* Initialized data: loaded from FLASH (_sidata), runs from RAM */
  _sidata = LOADADDR(.data);
  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH

  /* Zero-initialized data */
  . = ALIGN(4);
  .bss :
  {
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  /* SDMMC DMA bounce buffer (issue #33): NOLOAD, 32 B aligned (one Cortex-M7
   * D-cache line), placed in SRAM1 so the D-cache clean/invalidate around DMA
   * acts only on this dedicated buffer.  Not because DMA2 cannot reach the DTCM
   * (it can, via the M7 AHBS slave -- RM0385 2.1.1/2.1.6) but to confine cache
   * maintenance to one 32 B-aligned region; the SDMMC driver only ever DMAs into
   * this buffer and memcpy's to/from the caller's buffer.  The two ASSERTs keep
   * it strictly inside SRAM1 (0x20010000..0x2004BFFF), before SRAM2. */
  .sram1_dma (NOLOAD) :
  {
    . = ALIGN(32);
    _ssram1_dma = .;
    KEEP(*(.sram1_dma))
    . = ALIGN(32);
    _esram1_dma = .;
  } >RAM

  ASSERT(_ssram1_dma >= 0x20010000,
         ".sram1_dma must start in SRAM1 (cache-managed DMA buffer)")
  ASSERT(_esram1_dma <= 0x2004C000,
         ".sram1_dma must end within SRAM1 (before SRAM2 @0x2004C000)")

  /* External FMC SDRAM (issue #40): 8 MB at 0xC0000000, MPU-mapped Normal
   * non-cacheable (src/bsp.c).  NOLOAD only: the FMC is not initialized until
   * sdram_init() runs from tx_application_define(), so the startup code
   * (.data copy / .bss zeroing) must never touch this region -- objects here
   * get no load image and no zero-init, and nothing survives reset.  Holds
   * large DMA-target buffers (camera frame buffer, issue #41). */
  .sdram (NOLOAD) :
  {
    . = ALIGN(32);
    _ssdram = .;
    KEEP(*(.sdram))
    . = ALIGN(32);
    _esdram = .;
  } >SDRAM

  /* Guard against a future .sdram resident overflowing the 8 MB bank (issue #45
   * grows the camera frame/ring buffers).  >SDRAM already errors on overflow,
   * but this makes the intent and the headroom explicit. */
  ASSERT(_esdram - _ssdram <= 0x800000, ".sdram exceeds the 8 MB SDRAM bank")

  /* Make sure there is room left for the heap and stack */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
