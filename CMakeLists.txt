# ============================================================================
#  STM32F746G-DISCO  single firmware: interactive Eclipse ThreadX shell
#
#  One executable, `threadx`: the ThreadX CLI shell over the USART1 VCP.
#  Benchmarks (CoreMark) run as shell commands, not as separate images.
#
#  Configure (downloads the ARM toolchain into ./tools on first run):
#    cmake -B build -G Ninja \
#          -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
#  Build:               cmake --build build
#  Flash over ST-Link:  cmake --build build --target flash
# ============================================================================
cmake_minimum_required(VERSION 3.20)

# Fetch HAL/CMSIS/CoreMark/ThreadX submodules if they are missing.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib/stm32f7xx_hal_driver/Src/stm32f7xx_hal.c")
    message(STATUS "Initializing git submodules ...")
    execute_process(
        COMMAND git submodule update --init --recursive
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE _sm)
    if(NOT _sm EQUAL 0)
        message(FATAL_ERROR "git submodule update failed")
    endif()
endif()

project(stm32f746g-disco C ASM)

set(CMAKE_EXECUTABLE_SUFFIX ".elf")

# --- Target / common build options -----------------------------------------
set(MCU_OPTS
    -mcpu=cortex-m7 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard)

set(HAL_DIR    "${CMAKE_SOURCE_DIR}/lib/stm32f7xx_hal_driver")
set(CMSIS_DEV  "${CMAKE_SOURCE_DIR}/lib/cmsis_device_f7")
set(CMSIS_CORE "${CMAKE_SOURCE_DIR}/lib/cmsis_core")
set(LDSCRIPT   "${CMAKE_SOURCE_DIR}/ldscript/STM32F746NGHx_FLASH.ld")

# stm32f7xx_hal_conf.h is generated from the upstream template (its default
# HSE_VALUE of 25 MHz already matches the board crystal).
set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
configure_file("${HAL_DIR}/Inc/stm32f7xx_hal_conf_template.h"
               "${GEN_DIR}/stm32f7xx_hal_conf.h" COPYONLY)

# Usage requirements shared by every target (includes, defs, MCU flags, link).
add_library(bsp_iface INTERFACE)
target_include_directories(bsp_iface INTERFACE
    "${CMAKE_SOURCE_DIR}/include"
    "${GEN_DIR}"
    "${HAL_DIR}/Inc"
    "${CMSIS_DEV}/Include"
    "${CMSIS_CORE}/Include")
target_compile_definitions(bsp_iface INTERFACE USE_HAL_DRIVER STM32F746xx)
target_compile_options(bsp_iface INTERFACE
    ${MCU_OPTS} -Wall -fdata-sections -ffunction-sections -g -gdwarf-2)
target_link_options(bsp_iface INTERFACE
    ${MCU_OPTS} -specs=nano.specs -specs=nosys.specs "-T${LDSCRIPT}"
    -Wl,--gc-sections -Wl,--print-memory-usage)

# --- Common object library (HAL + CMSIS + board bring-up) ------------------
# Compile every HAL driver source; each file self-guards on its
# HAL_<module>_MODULE_ENABLED define, so unused ones become empty objects.
file(GLOB HAL_SOURCES "${HAL_DIR}/Src/*.c")
list(FILTER HAL_SOURCES EXCLUDE REGEX "_template\\.c$")

add_library(common OBJECT
    ${HAL_SOURCES}
    "${CMSIS_DEV}/Source/Templates/system_stm32f7xx.c"
    "${CMSIS_DEV}/Source/Templates/gcc/startup_stm32f746xx.s"
    "${CMAKE_SOURCE_DIR}/src/bsp.c")
target_link_libraries(common PUBLIC bsp_iface)
target_compile_options(common PRIVATE -O2)

# --- Helper: post-build artifacts + flash target ---------------------------
# Single firmware, so the flash target is simply `flash` (not flash-<tgt>).
function(firmware_finalize tgt)
    target_link_options(${tgt} PRIVATE -Wl,-Map=${tgt}.map,--cref)
    set_target_properties(${tgt} PROPERTIES LINK_DEPENDS "${LDSCRIPT}")
    add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND "${CMAKE_OBJCOPY}" -O binary -S $<TARGET_FILE:${tgt}> ${tgt}.bin
        COMMAND "${CMAKE_OBJCOPY}" -O ihex   $<TARGET_FILE:${tgt}> ${tgt}.hex
        COMMAND "${CMAKE_SIZE}" $<TARGET_FILE:${tgt}>
        BYPRODUCTS ${tgt}.bin ${tgt}.hex
        COMMENT "objcopy -> ${tgt}.bin / ${tgt}.hex")
    # --connect-under-reset: hold the MCU in reset while attaching.  Required
    # since #20 (TX_ENABLE_WFI) -- the running firmware sleeps in WFI when idle,
    # and the old onboard ST-Link (V2J28x) cannot connect to a sleeping core
    # without reset (st-flash otherwise fails with "Can not connect to target /
    # unknown chip id").  Harmless for non-sleeping firmware (issue #24).
    add_custom_target(flash
        COMMAND st-flash --connect-under-reset --reset write ${tgt}.bin 0x08000000
        DEPENDS ${tgt}
        USES_TERMINAL
        COMMENT "Flashing ${tgt} over ST-Link")
endfunction()

# --- CoreMark object library (run as the shell `coremark` command) ----------
# Built once at -O3 and linked into the threadx firmware below; cmd_coremark.c
# calls coremark_main().  core_main.c is compiled with -Dmain=coremark_main so
# its main() does not clash with the firmware main() in src/main.c.  MEM_METHOD=
# MEM_STATIC keeps the TOTAL_DATA_SIZE block in .bss (off the shell thread
# stack) as a static array, so no malloc is pulled in.
set(CMK_DIR "${CMAKE_SOURCE_DIR}/lib/coremark")
add_library(coremark_obj OBJECT
    "${CMK_DIR}/core_list_join.c"
    "${CMK_DIR}/core_main.c"
    "${CMK_DIR}/core_matrix.c"
    "${CMK_DIR}/core_state.c"
    "${CMK_DIR}/core_util.c"
    "${CMAKE_SOURCE_DIR}/port/coremark/core_portme.c")
target_link_libraries(coremark_obj PUBLIC bsp_iface)   # HAL_GetTick for timing
target_include_directories(coremark_obj PRIVATE
    "${CMK_DIR}" "${CMAKE_SOURCE_DIR}/port/coremark")
target_compile_definitions(coremark_obj PRIVATE
    ITERATIONS=0 PERFORMANCE_RUN=1 TOTAL_DATA_SIZE=2000
    MEM_METHOD=MEM_STATIC MEM_LOCATION="STATIC")
target_compile_options(coremark_obj PRIVATE -O3 -funroll-loops)
# Rename CoreMark's main() so it does not collide with the firmware main().
set_source_files_properties("${CMK_DIR}/core_main.c" PROPERTIES
    COMPILE_DEFINITIONS "main=coremark_main")

# --- Eclipse ThreadX core + Cortex-M7/GNU port -----------------------------
# Shared by the threadx firmware below.  ThreadX supplies its own PendSV_Handler
# (there is no SVC user), so src/stm32f7xx_it.c is not part of the build; the
# startup file's weak Default_Handler catches every other vector.
set(TX_DIR  "${CMAKE_SOURCE_DIR}/lib/threadx")
set(TX_PORT "${TX_DIR}/ports/cortex_m7/gnu")
file(GLOB TX_CORE "${TX_DIR}/common/src/*.c")
list(FILTER TX_CORE EXCLUDE REGEX "tx_misra\\.c$")
file(GLOB TX_ASM "${TX_PORT}/src/*.S")
list(FILTER TX_ASM EXCLUDE REGEX "tx_misra\\.S$")

# --- App: threadx (the interactive ThreadX CLI shell) ----------------------
# The single firmware: the ThreadX CLI shell over the USART1 VCP.  Uses the
# ThreadX core/port set up above and omits src/stm32f7xx_it.c because ThreadX
# supplies PendSV_Handler (USART1_IRQHandler comes from the UART backend).
#
# The shell core + backends are collected into an OBJECT library named shell_obj
# (NOT threadx -- the executable below takes the target name `threadx`).  Because
# cli_instance.h pulls in tx_api.h, the objlib MUST be compiled with the same
# TX_INCLUDE_USER_DEFINE_FILE + port/threadx as the ThreadX core linked into the
# exe, or the TX_THREAD / event-flags / mutex layouts disagree (ABI mismatch).
add_library(shell_obj OBJECT
    "${CMAKE_SOURCE_DIR}/shell/core/cli_core.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_complete.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_edit.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_history.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_job.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_parse.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_printf.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_fmt.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_session.c"
    "${CMAKE_SOURCE_DIR}/shell/backend/cli_backend_uart.c"
    "${CMAKE_SOURCE_DIR}/shell/backend/cli_backend_dummy.c")
target_link_libraries(shell_obj PUBLIC bsp_iface)
target_include_directories(shell_obj
    PUBLIC  "${CMAKE_SOURCE_DIR}/shell/include"
    PRIVATE "${CMAKE_SOURCE_DIR}/shell/core"
            "${CMAKE_SOURCE_DIR}/shell/backend"
            "${TX_DIR}/common/inc" "${TX_PORT}/inc"
            "${TX_DIR}/utility/execution_profile_kit"   # tx_api.h auto-includes tx_execution_profile.h (EPK)
            "${CMAKE_SOURCE_DIR}/port/threadx")
target_compile_definitions(shell_obj PRIVATE TX_INCLUDE_USER_DEFINE_FILE)
target_compile_options(shell_obj PRIVATE -O2)

# Embed a firmware version header for the `version` builtin (issue #12).  git
# describe is captured at configure time (a snapshot; re-run cmake to refresh)
# and substituted into cli_version.h under GEN_DIR, which is already on the
# bsp_iface include path so cmd_system.c can #include "cli_version.h".
execute_process(
    COMMAND git -C "${CMAKE_SOURCE_DIR}" describe --always --dirty --tags
    OUTPUT_VARIABLE CLI_GIT_DESC OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT CLI_GIT_DESC)
    set(CLI_GIT_DESC "unknown")
endif()
configure_file("${CMAKE_SOURCE_DIR}/cmake/cli_version.h.in"
               "${GEN_DIR}/cli_version.h" @ONLY)

# Compile-time gate for the dangerous shell commands (reboot, devmem), spec §12.
# ON by default for the demo; production builds pass -DCLI_ENABLE_DANGEROUS_CMDS=OFF.
# A cache var alone does NOT reach the compiler, so forward it explicitly to the
# define cli_config.h expects.
option(CLI_ENABLE_DANGEROUS_CMDS "Build the dangerous shell commands (reboot, devmem)" ON)

# Upper bound (bytes) on a single `devmem dump`; overrides the cli_config.h
# default.  STRING (not option) because it carries a number, forwarded below.
set(CLI_DEVMEM_DUMP_MAX_LEN "256" CACHE STRING "Max bytes per devmem dump")

add_executable(threadx
    "${CMAKE_SOURCE_DIR}/src/main.c"
    "${CMAKE_SOURCE_DIR}/src/log.c"
    "${CMAKE_SOURCE_DIR}/src/fault.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_dmesg.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_crash.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_builtin.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_system.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_thread.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_devmem.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_coremark.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_sleep.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_watch.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_jobs.c"
    "${CMAKE_SOURCE_DIR}/port/threadx/tx_glue.c"
    "${TX_DIR}/utility/execution_profile_kit/tx_execution_profile.c"  # EPK (issue #19 cpu%)
    ${TX_CORE}
    ${TX_ASM})
target_link_libraries(threadx PRIVATE common shell_obj coremark_obj)
target_include_directories(threadx PRIVATE
    "${TX_DIR}/common/inc" "${TX_PORT}/inc"
    "${TX_DIR}/utility/execution_profile_kit"   # EPK header (issue #19 cpu%)
    "${CMAKE_SOURCE_DIR}/port/threadx"
    "${CMAKE_SOURCE_DIR}/shell/include"
    "${CMAKE_SOURCE_DIR}/shell/core"      # cli_instance.h -> cli_internal.h
    "${CMAKE_SOURCE_DIR}/shell/backend")  # CLI_BACKEND_*_DEFINE macros
target_compile_definitions(threadx PRIVATE TX_INCLUDE_USER_DEFINE_FILE
    CLI_ENABLE_DANGEROUS_CMDS=$<BOOL:${CLI_ENABLE_DANGEROUS_CMDS}>
    CLI_DEVMEM_DUMP_MAX_LEN=${CLI_DEVMEM_DUMP_MAX_LEN}
    CLI_INSTANCE_STACK_SIZE=4096          # headroom: coremark runs in this thread
    CLI_BG_JOB_STACK_SIZE=4096)           # ... and in a bg worker: coremark & (issue #25)
target_compile_options(threadx PRIVATE -O2)
target_link_options(threadx PRIVATE -u _printf_float)   # CoreMark %f score line
firmware_finalize(threadx)
