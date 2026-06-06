# ============================================================================
#  STM32F746G-DISCO  bare-metal apps (blink / coremark / threadx)
#
#  Configure (downloads the ARM toolchain into ./tools on first run):
#    cmake -B build -G Ninja \
#          -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
#  Build everything:           cmake --build build
#  Flash one app over ST-Link: cmake --build build --target flash-threadx
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
function(firmware_finalize tgt)
    target_link_options(${tgt} PRIVATE -Wl,-Map=${tgt}.map,--cref)
    set_target_properties(${tgt} PROPERTIES LINK_DEPENDS "${LDSCRIPT}")
    add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND "${CMAKE_OBJCOPY}" -O binary -S $<TARGET_FILE:${tgt}> ${tgt}.bin
        COMMAND "${CMAKE_OBJCOPY}" -O ihex   $<TARGET_FILE:${tgt}> ${tgt}.hex
        COMMAND "${CMAKE_SIZE}" $<TARGET_FILE:${tgt}>
        BYPRODUCTS ${tgt}.bin ${tgt}.hex
        COMMENT "objcopy -> ${tgt}.bin / ${tgt}.hex")
    add_custom_target(flash-${tgt}
        COMMAND st-flash --reset write ${tgt}.bin 0x08000000
        DEPENDS ${tgt}
        USES_TERMINAL
        COMMENT "Flashing ${tgt} over ST-Link")
endfunction()

# --- App: coremark (optional; kept for later, off by default) --------------
# The lib/coremark submodule and port/coremark/ are retained on disk; enable
# the build with: cmake -B build ... -DBUILD_COREMARK=ON
option(BUILD_COREMARK "Build the EEMBC CoreMark benchmark app" OFF)
if(BUILD_COREMARK)
    set(CMK_DIR "${CMAKE_SOURCE_DIR}/lib/coremark")
    add_executable(coremark
        "${CMAKE_SOURCE_DIR}/src/stm32f7xx_it.c"
        "${CMK_DIR}/core_list_join.c"
        "${CMK_DIR}/core_main.c"
        "${CMK_DIR}/core_matrix.c"
        "${CMK_DIR}/core_state.c"
        "${CMK_DIR}/core_util.c"
        "${CMAKE_SOURCE_DIR}/port/coremark/core_portme.c")
    target_link_libraries(coremark PRIVATE common)
    target_include_directories(coremark PRIVATE
        "${CMK_DIR}" "${CMAKE_SOURCE_DIR}/port/coremark")
    target_compile_definitions(coremark PRIVATE
        ITERATIONS=0 PERFORMANCE_RUN=1 TOTAL_DATA_SIZE=2000)
    target_compile_options(coremark PRIVATE -O3 -funroll-loops)
    target_link_options(coremark PRIVATE -u _printf_float)  # nano printf %f
    firmware_finalize(coremark)
endif()

# --- App: threadx ----------------------------------------------------------
# ThreadX provides its own PendSV_Handler, so src/stm32f7xx_it.c is omitted.
set(TX_DIR  "${CMAKE_SOURCE_DIR}/lib/threadx")
set(TX_PORT "${TX_DIR}/ports/cortex_m7/gnu")
file(GLOB TX_CORE "${TX_DIR}/common/src/*.c")
list(FILTER TX_CORE EXCLUDE REGEX "tx_misra\\.c$")
file(GLOB TX_ASM "${TX_PORT}/src/*.S")
list(FILTER TX_ASM EXCLUDE REGEX "tx_misra\\.S$")
add_executable(threadx
    "${CMAKE_SOURCE_DIR}/src/app_threadx.c"
    "${CMAKE_SOURCE_DIR}/port/threadx/tx_glue.c"
    ${TX_CORE}
    ${TX_ASM})
target_link_libraries(threadx PRIVATE common)
target_include_directories(threadx PRIVATE
    "${TX_DIR}/common/inc" "${TX_PORT}/inc"
    "${CMAKE_SOURCE_DIR}/port/threadx")
target_compile_definitions(threadx PRIVATE TX_INCLUDE_USER_DEFINE_FILE)
target_compile_options(threadx PRIVATE -O2)
firmware_finalize(threadx)

# --- App: thread_metric (optional Thread-Metric RTOS benchmark) -------------
# Runs the EEMBC-style ThreadX benchmark suite. Pick the test with
# -DTHREAD_METRIC_TEST=<name>. Reuses TX_DIR/TX_PORT/TX_CORE/TX_ASM from above.
option(BUILD_THREAD_METRIC "Build the Thread-Metric RTOS benchmark app" OFF)
if(BUILD_THREAD_METRIC)
    set(THREAD_METRIC_TEST "basic" CACHE STRING
        "Thread-Metric test: basic|cooperative|preemptive|memory|message|sync|interrupt|interrupt_preempt")
    set(TM_DIR "${TX_DIR}/utility/benchmarks/thread_metric")

    # Map the short test name to its source file; interrupt tests also need the
    # SVC glue (TM_CAUSE_INTERRUPT == SVC #0).
    set(TM_NEEDS_SVC FALSE)
    if(THREAD_METRIC_TEST STREQUAL "basic")
        set(TM_TEST_SRC "${TM_DIR}/tm_basic_processing_test.c")
    elseif(THREAD_METRIC_TEST STREQUAL "cooperative")
        set(TM_TEST_SRC "${TM_DIR}/tm_cooperative_scheduling_test.c")
    elseif(THREAD_METRIC_TEST STREQUAL "preemptive")
        set(TM_TEST_SRC "${TM_DIR}/tm_preemptive_scheduling_test.c")
    elseif(THREAD_METRIC_TEST STREQUAL "memory")
        set(TM_TEST_SRC "${TM_DIR}/tm_memory_allocation_test.c")
    elseif(THREAD_METRIC_TEST STREQUAL "message")
        set(TM_TEST_SRC "${TM_DIR}/tm_message_processing_test.c")
    elseif(THREAD_METRIC_TEST STREQUAL "sync")
        set(TM_TEST_SRC "${TM_DIR}/tm_synchronization_processing_test.c")
    elseif(THREAD_METRIC_TEST STREQUAL "interrupt")
        set(TM_TEST_SRC "${TM_DIR}/tm_interrupt_processing_test.c")
        set(TM_NEEDS_SVC TRUE)
    elseif(THREAD_METRIC_TEST STREQUAL "interrupt_preempt")
        set(TM_TEST_SRC "${TM_DIR}/tm_interrupt_preemption_processing_test.c")
        set(TM_NEEDS_SVC TRUE)
    else()
        message(FATAL_ERROR "Unknown THREAD_METRIC_TEST '${THREAD_METRIC_TEST}'")
    endif()

    set(TM_SRC
        "${CMAKE_SOURCE_DIR}/src/app_thread_metric.c"
        "${CMAKE_SOURCE_DIR}/port/threadx/tx_glue.c"
        "${TM_DIR}/threadx_example/tm_porting_layer_threadx.c"
        "${TM_TEST_SRC}"
        ${TX_CORE} ${TX_ASM})
    if(TM_NEEDS_SVC)
        list(APPEND TM_SRC "${CMAKE_SOURCE_DIR}/port/threadx/tm_svc.c")
    endif()

    add_executable(thread_metric ${TM_SRC})
    target_link_libraries(thread_metric PRIVATE common)
    target_include_directories(thread_metric PRIVATE
        "${TX_DIR}/common/inc" "${TX_PORT}/inc"
        "${CMAKE_SOURCE_DIR}/port/threadx"
        "${TM_DIR}")
    # ThreadX tick at 100 Hz to match the porting layer's
    # TM_THREADX_TICKS_PER_SECOND (so tm_thread_sleep(seconds) is real seconds),
    # plus the recommended Thread-Metric performance build options.
    target_compile_definitions(thread_metric PRIVATE
        TX_INCLUDE_USER_DEFINE_FILE
        TX_TIMER_TICKS_PER_SECOND=100
        TX_GLUE_TICK_DIV=10
        TX_DISABLE_ERROR_CHECKING
        TX_DISABLE_NOTIFY_CALLBACKS
        TX_DISABLE_REDUNDANT_CLEARING
        TX_DISABLE_STACK_FILLING
        TX_NOT_INTERRUPTABLE
        TX_TIMER_PROCESS_IN_ISR
        TX_REACTIVATE_INLINE
        TX_INLINE_THREAD_RESUME_SUSPEND)
    target_compile_options(thread_metric PRIVATE -O2)
    # The benchmark counters are plain (non-volatile) globals updated in tight
    # infinite loops; at -O2 GCC keeps them in a register and never writes them
    # back, so the reporting thread reads a stale value ("thread died"). Build
    # the test source at -O0 so the counters hit memory. ThreadX itself stays
    # at -O2, so the measured RTOS operations remain representative.
    set_source_files_properties("${TM_TEST_SRC}" PROPERTIES COMPILE_OPTIONS "-O0")
    firmware_finalize(thread_metric)
endif()

# --- App: exec_profile (optional Execution Profile Kit demo) ----------------
# Per-thread / ISR / idle CPU-time profiling via the Cortex-M7 DWT cycle
# counter. TX_EXECUTION_PROFILE_ENABLE makes the ThreadX port asm call the
# profile hooks, so ThreadX core + port must be rebuilt with it (per-target
# here). Reuses TX_DIR/TX_PORT/TX_CORE/TX_ASM from the threadx block.
option(BUILD_EXEC_PROFILE "Build the ThreadX Execution Profile Kit demo" OFF)
if(BUILD_EXEC_PROFILE)
    add_executable(exec_profile
        "${CMAKE_SOURCE_DIR}/src/app_exec_profile.c"
        "${CMAKE_SOURCE_DIR}/port/threadx/tx_glue.c"
        "${TX_DIR}/utility/execution_profile_kit/tx_execution_profile.c"
        ${TX_CORE} ${TX_ASM})
    target_link_libraries(exec_profile PRIVATE common)
    target_include_directories(exec_profile PRIVATE
        "${TX_DIR}/common/inc" "${TX_PORT}/inc"
        "${CMAKE_SOURCE_DIR}/port/threadx"
        "${TX_DIR}/utility/execution_profile_kit")
    target_compile_definitions(exec_profile PRIVATE
        TX_INCLUDE_USER_DEFINE_FILE
        TX_EXECUTION_PROFILE_ENABLE)
    target_compile_options(exec_profile PRIVATE -O2)
    firmware_finalize(exec_profile)
endif()
