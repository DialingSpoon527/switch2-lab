# SPDX-License-Identifier: MIT
ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is required)
endif
# Supply KAIHEN_SOURCE=/path/to/patched-kaihen so this NRO sees commands 24-30.
KAIHEN_SOURCE ?= ../../kaihen
TARGET := p2-plugin-control
BUILD := build
PREFIX := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-
include $(DEVKITPRO)/libounce/switch2_cpu.mk
ARCH := $(SWITCH2_CPU) -mtp=soft -fPIE
CFLAGS := $(ARCH) -std=gnu11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -MMD -MP \
    -D__SWITCH__ -D__SWITCH2__ \
    -I$(KAIHEN_SOURCE)/libkaihen/include \
    -I$(DEVKITPRO)/portlibs/switch2/include -I$(DEVKITPRO)/libounce/include
LDFLAGS := $(ARCH) -specs=$(DEVKITPRO)/libounce/switch.specs -Wl,--gc-sections
LIBS := -L$(DEVKITPRO)/libounce/lib -lounce_core
all: $(TARGET).nro
$(BUILD):
	mkdir -p $@
$(BUILD)/main.o: main.c command_parser.h | $(BUILD)
	$(PREFIX)gcc $(CFLAGS) -c $< -o $@
$(BUILD)/$(TARGET).elf: $(BUILD)/main.o
	$(PREFIX)gcc $(LDFLAGS) $^ $(LIBS) -o $@
$(BUILD)/plugin-starter.nacp: | $(BUILD)
	$(DEVKITPRO)/tools/bin/nacptool --create "Plugin starter" "Homebrew" "0.1.3" $@
$(TARGET).nro: $(BUILD)/$(TARGET).elf $(BUILD)/plugin-starter.nacp assets/icon.jpg
	$(DEVKITPRO)/tools/bin/elf2nro $< $@ --nacp=$(BUILD)/plugin-starter.nacp --icon=assets/icon.jpg
clean:
	rm -rf $(BUILD) $(TARGET).nro
.PHONY: all clean
-include $(BUILD)/*.d
