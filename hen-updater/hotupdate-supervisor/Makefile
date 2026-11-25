# SPDX-License-Identifier: GPL-2.0-only
# Builds the native resident supervisor and shared package writer.
DEVKITPRO ?= /opt/devkitpro
LIBOUNCE ?= $(DEVKITPRO)/libounce
PREFIX := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-
include $(LIBOUNCE)/switch2_cpu.mk
ARCH := $(SWITCH2_CPU) -mtp=soft -fPIE
CXXFLAGS := $(ARCH) -std=gnu++20 -O2 -Wall -Wextra -Werror \
    -Wno-missing-field-initializers -fno-exceptions -fno-rtti \
    -ffunction-sections -fdata-sections -MMD -MP -D__SWITCH__ -D__SWITCH2__ \
    -I../libkaihen/include -I$(LIBOUNCE)/include
LDFLAGS := $(ARCH) -specs=build/switch.specs -Wl,--gc-sections
LIBS := -L../libkaihen/build -L$(LIBOUNCE)/lib -lkaihen_plugin -lounce_core
OUT := build
OBJECTS := $(OUT)/main.o $(OUT)/launch_backend.o $(OUT)/restart_controller.o $(OUT)/deferred_monitor.o \
           $(OUT)/policy_package.o $(OUT)/sd_writer.o
all: hotupdate-supervisor.nso
$(OUT):
	mkdir -p $@
$(OUT)/switch.specs: $(LIBOUNCE)/switch.specs | $(OUT)
	sed 's|%:getenv(DEVKITPRO /libnx/switch.ld)|$(abspath $(LIBOUNCE))/switch.ld|' $< > $@
$(OUT)/%.o: source/%.cpp | $(OUT)
	$(PREFIX)g++ $(CXXFLAGS) -c $< -o $@
$(OUT)/%.o: ../common/%.cpp | $(OUT)
	$(PREFIX)g++ $(CXXFLAGS) -c $< -o $@
../libkaihen/build/libkaihen_plugin.a:
	$(MAKE) -C ../libkaihen
$(OUT)/hotupdate-supervisor.elf: $(OBJECTS) $(OUT)/switch.specs ../libkaihen/build/libkaihen_plugin.a
	$(PREFIX)g++ $(LDFLAGS) $(OBJECTS) $(LIBS) -o $@
hotupdate-supervisor.nso: $(OUT)/hotupdate-supervisor.elf
	$(DEVKITPRO)/tools/bin/elf2nso $< $@
clean:
	rm -rf $(OUT) hotupdate-supervisor.nso
.PHONY: all clean
-include $(OBJECTS:.o=.d)
