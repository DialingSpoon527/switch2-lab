# SPDX-License-Identifier: GPL-2.0-only
DEVKITPRO ?= /opt/devkitpro
LIBOUNCE ?= $(DEVKITPRO)/libounce
include $(LIBOUNCE)/switch2_cpu.mk
ARCH := $(SWITCH2_CPU) -mtp=soft -fPIE
PREFIX := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-
CXXFLAGS := $(ARCH) -std=gnu++20 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -fno-exceptions -fno-rtti \
    -ffunction-sections -fdata-sections -MMD -MP -D__SWITCH__ -D__SWITCH2__ \
    -I$(LIBOUNCE)/include -I../libkaihen/include
LDFLAGS := $(ARCH) -specs=build/switch.specs -Wl,--gc-sections
all: hen-updater.nro
build:
	mkdir -p $@
build/switch.specs: $(LIBOUNCE)/switch.specs | build
	sed 's|%:getenv(DEVKITPRO /libnx/switch.ld)|$(abspath $(LIBOUNCE))/switch.ld|' $< > $@
build/main.o: source/main.cpp | build
	$(PREFIX)g++ $(CXXFLAGS) -c $< -o $@
build/main.elf: build/main.o build/switch.specs
	$(PREFIX)g++ $(LDFLAGS) $< -L$(LIBOUNCE)/lib -lounce_core -o $@
build/hen-updater.nacp: | build
	$(DEVKITPRO)/tools/bin/nacptool --create "HEN updater" "Homebrew" "0.2.0" $@
hen-updater.nro: build/main.elf build/hen-updater.nacp
	$(DEVKITPRO)/tools/bin/elf2nro $< $@ --nacp=build/hen-updater.nacp
clean:
	rm -rf build hen-updater.nro
.PHONY: all clean
-include build/main.d
