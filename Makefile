# ============================================================================
#  STM32F746G-DISCO  bare-metal LED blink
#
#  - OS-less
#  - STMicroelectronics official HAL fetched as git submodules
#  - ARM GNU toolchain auto-downloaded into ./tools by this Makefile
#  - Chip config: FPU on / I-Cache on / D-Cache on / SYSCLK 200 MHz
#
#  Usage:
#    make            # download toolchain (first run) + build -> build/blink.{elf,bin,hex}
#    make flash      # build then write to the board over ST-Link
#    make reset      # reset the target over ST-Link
#    make clean      # remove build artifacts
#    make distclean  # also remove the downloaded toolchain
# ============================================================================

# Application selector:  make            -> blink (default)
#                        make APP=coremark
APP       ?= blink
TARGET    := $(APP)
BUILD_DIR := build/$(APP)

# ---------------------------------------------------------------------------
# Make sure HAL/CMSIS submodules are present before the wildcards below expand.
# ---------------------------------------------------------------------------
ifeq ($(wildcard lib/stm32f7xx_hal_driver/Src/stm32f7xx_hal.c),)
  $(info >>> Initializing git submodules (HAL / CMSIS) ...)
  $(shell git submodule update --init --recursive 1>&2)
endif

# ---------------------------------------------------------------------------
# ARM GNU toolchain (downloaded automatically into ./tools)
# ---------------------------------------------------------------------------
TOOLCHAIN_VERSION := 13.3.rel1
TOOLCHAIN_NAME    := arm-gnu-toolchain-$(TOOLCHAIN_VERSION)-x86_64-arm-none-eabi
TOOLCHAIN_URL     := https://developer.arm.com/-/media/Files/downloads/gnu/$(TOOLCHAIN_VERSION)/binrel/$(TOOLCHAIN_NAME).tar.xz
TOOLS_DIR         := tools
GCC_DIR           := $(TOOLS_DIR)/$(TOOLCHAIN_NAME)
GCC_PATH          := $(GCC_DIR)/bin
GCC_STAMP         := $(GCC_PATH)/arm-none-eabi-gcc

PREFIX := arm-none-eabi-
CC := $(GCC_PATH)/$(PREFIX)gcc
AS := $(GCC_PATH)/$(PREFIX)gcc -x assembler-with-cpp
CP := $(GCC_PATH)/$(PREFIX)objcopy
SZ := $(GCC_PATH)/$(PREFIX)size

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
HAL_DIR    := lib/stm32f7xx_hal_driver
CMSIS_DEV  := lib/cmsis_device_f7
CMSIS_CORE := lib/cmsis_core

# Compile every HAL driver source; each file self-guards on its
# HAL_<module>_MODULE_ENABLED define in stm32f7xx_hal_conf.h, so the ones we
# do not use simply compile to empty objects.
HAL_SOURCES := $(filter-out %_template.c,$(wildcard $(HAL_DIR)/Src/*.c))

# Sources common to every app (board bring-up + HAL + CMSIS).
# Note: src/stm32f7xx_it.c is per-app (ThreadX provides its own handlers).
COMMON_SOURCES := \
  src/bsp.c \
  $(CMSIS_DEV)/Source/Templates/system_stm32f7xx.c \
  $(HAL_SOURCES)

# ---- Per-application sources / flags -------------------------------------
APP_ASM_SOURCES :=
ifeq ($(APP),coremark)
  CMK_DIR      := lib/coremark
  APP_SOURCES  := \
    src/stm32f7xx_it.c \
    $(CMK_DIR)/core_list_join.c \
    $(CMK_DIR)/core_main.c \
    $(CMK_DIR)/core_matrix.c \
    $(CMK_DIR)/core_state.c \
    $(CMK_DIR)/core_util.c \
    port/coremark/core_portme.c
  APP_INCLUDES := -I$(CMK_DIR) -Iport/coremark
  APP_DEFS     := -DITERATIONS=0 -DPERFORMANCE_RUN=1 -DTOTAL_DATA_SIZE=2000
  APP_OPT      := -O3 -funroll-loops
  APP_LDFLAGS  := -Wl,-u,_printf_float    # nano printf float support for %f
else ifeq ($(APP),threadx)
  TX_DIR       := lib/threadx
  TX_PORT      := $(TX_DIR)/ports/cortex_m7/gnu
  # All ThreadX core C files; the port assembly minus tx_misra.S (which would
  # redefine __aeabi_memset and core globals).
  APP_SOURCES  := \
    src/app_threadx.c \
    port/threadx/tx_glue.c \
    $(filter-out %/tx_misra.c,$(wildcard $(TX_DIR)/common/src/*.c))
  APP_ASM_SOURCES := $(filter-out %/tx_misra.S,$(wildcard $(TX_PORT)/src/*.S))
  APP_INCLUDES := -I$(TX_DIR)/common/inc -I$(TX_PORT)/inc -Iport/threadx
  APP_DEFS     := -DTX_INCLUDE_USER_DEFINE_FILE
  APP_OPT      := -O2
  APP_LDFLAGS  :=
else ifeq ($(APP),blink)
  APP_SOURCES  := src/main.c src/stm32f7xx_it.c
  APP_INCLUDES :=
  APP_DEFS     :=
  APP_OPT      := -Og
  APP_LDFLAGS  :=
else
  $(error Unknown APP '$(APP)'. Use APP=blink, APP=coremark or APP=threadx)
endif

C_SOURCES   := $(COMMON_SOURCES) $(APP_SOURCES)
ASM_SOURCES := $(CMSIS_DEV)/Source/Templates/gcc/startup_stm32f746xx.s $(APP_ASM_SOURCES)

# ---------------------------------------------------------------------------
# stm32f7xx_hal_conf.h is generated from the upstream template (its default
# HSE_VALUE is 25 MHz, matching the STM32F746G-DISCO crystal).
# ---------------------------------------------------------------------------
GEN_DIR  := $(BUILD_DIR)/gen
HAL_CONF := $(GEN_DIR)/stm32f7xx_hal_conf.h

# ---------------------------------------------------------------------------
# Flags
# ---------------------------------------------------------------------------
CPU       := -mcpu=cortex-m7
FPU       := -mfpu=fpv5-sp-d16
FLOAT_ABI := -mfloat-abi=hard
MCU       := $(CPU) -mthumb $(FPU) $(FLOAT_ABI)

C_DEFS := -DUSE_HAL_DRIVER -DSTM32F746xx $(APP_DEFS)

C_INCLUDES := \
  -Iinclude \
  -I$(GEN_DIR) \
  -I$(HAL_DIR)/Inc \
  -I$(CMSIS_DEV)/Include \
  -I$(CMSIS_CORE)/Include \
  $(APP_INCLUDES)

OPT := $(APP_OPT)

CFLAGS  := $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) \
           -Wall -fdata-sections -ffunction-sections -g -gdwarf-2
ASFLAGS := $(MCU) -Og $(C_INCLUDES) -Wall -fdata-sections -ffunction-sections -g -gdwarf-2

LDSCRIPT := ldscript/STM32F746NGHx_FLASH.ld
LIBS     := -lc -lm -lnosys
LDFLAGS  := $(MCU) -specs=nano.specs -T$(LDSCRIPT) $(APP_LDFLAGS) $(LIBS) \
            -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref -Wl,--gc-sections

# ---------------------------------------------------------------------------
# Objects
# ---------------------------------------------------------------------------
ASM_SOURCES_S := $(filter %.s,$(ASM_SOURCES))   # lowercase .s (CMSIS startup)
ASM_SOURCES_UP := $(filter %.S,$(ASM_SOURCES))   # uppercase .S (ThreadX port)

OBJECTS := $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES_S:.s=.o)))
vpath %.s $(sort $(dir $(ASM_SOURCES_S)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES_UP:.S=.o)))
vpath %.S $(sort $(dir $(ASM_SOURCES_UP)))

# ---------------------------------------------------------------------------
# Build rules
# ---------------------------------------------------------------------------
.PHONY: all clean distclean flash reset toolchain
all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

toolchain: $(GCC_STAMP)

$(GCC_STAMP):
	@mkdir -p $(TOOLS_DIR)
	@echo ">>> Downloading ARM GNU toolchain $(TOOLCHAIN_VERSION) (~150 MB) ..."
	curl -L --fail -o $(TOOLS_DIR)/$(TOOLCHAIN_NAME).tar.xz "$(TOOLCHAIN_URL)"
	@echo ">>> Extracting ..."
	tar -xf $(TOOLS_DIR)/$(TOOLCHAIN_NAME).tar.xz -C $(TOOLS_DIR)
	@rm -f $(TOOLS_DIR)/$(TOOLCHAIN_NAME).tar.xz
	@touch $@

$(HAL_CONF): $(HAL_DIR)/Inc/stm32f7xx_hal_conf_template.h | $(GEN_DIR)
	cp $< $@

$(BUILD_DIR)/%.o: %.c $(HAL_CONF) | $(BUILD_DIR) $(GCC_STAMP)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: %.s | $(BUILD_DIR) $(GCC_STAMP)
	$(AS) -c $(ASFLAGS) $< -o $@

$(BUILD_DIR)/%.o: %.S | $(BUILD_DIR) $(GCC_STAMP)
	$(AS) -c $(ASFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SZ) $@

$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(CP) -O ihex $< $@

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(CP) -O binary -S $< $@

$(BUILD_DIR) $(GEN_DIR):
	mkdir -p $@

# ---------------------------------------------------------------------------
# Flash / reset over ST-Link (uses system st-flash)
# ---------------------------------------------------------------------------
flash: $(BUILD_DIR)/$(TARGET).bin
	st-flash --reset write $< 0x08000000

reset:
	st-flash reset

clean:
	rm -rf $(BUILD_DIR)

distclean: clean
	rm -rf $(TOOLS_DIR)
