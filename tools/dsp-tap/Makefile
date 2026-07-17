# dsp-tap — Luma 3GX DSP-tap plugin. OPTIONAL, devkitARM-only build.
#
# This Makefile is NEVER invoked by caesar's root CMake build or CI. Like
# tools/dsp-oracle keeps teakra out of caesar_core, the tap keeps devkitARM out
# of the core. It DETECTS the toolchain and degrades gracefully: with no
# devkitARM it prints how to get it and exits 0, so a `make` here can never break
# a machine that only builds the converter.
#
# Requires (on a devkitARM machine):
#   - devkitARM + libctru       (pacman -S 3ds-dev)  -> sets DEVKITARM/DEVKITPRO
#   - 3gxtool                    (PabloMK7/3gxtool)   -> packs ELF -> .3gx
#     3gxtool is a BUILD tool only; it is not linked into the plugin, so its
#     license does not affect the GPLv3 plugin output (see DSP-TAP-DESIGN.md).
#
# Output: build/dsp-tap.3gx  (install to sd:/luma/plugins/<TitleID>/)
#
# caesar (GPLv3). The plugin sources are UNTESTED without devkitARM + a console.

TARGET      := dsp-tap
BUILD       := build
INCLUDE     := include
SRCDIR      := src

TITLE_ID    := 0004001000021800   # StreetPass Mii Plaza (host title)

# Route-a gate: 0 = do NOT capture final_mix (default, until route-a confirms the
# firmware populates it). Build with `make FINAL_SAMPLES=1` to enable.
FINAL_SAMPLES ?= 0

# --- toolchain detection --------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
  ifneq ($(strip $(DEVKITPRO)),)
    DEVKITARM := $(DEVKITPRO)/devkitARM
  endif
endif

# Look for the cross-gcc in $(DEVKITARM)/bin (with or without a .exe suffix on
# Windows/msys), then fall back to one on PATH.
HAVE_DKA := $(and $(wildcard $(DEVKITARM)/bin/arm-none-eabi-gcc $(DEVKITARM)/bin/arm-none-eabi-gcc.exe),yes)
ifeq ($(HAVE_DKA),)
  HAVE_DKA := $(and $(shell command -v arm-none-eabi-gcc 2>/dev/null),yes)
endif

.PHONY: all clean plugin no-toolchain

ifeq ($(HAVE_DKA),yes)
all: plugin
else
all: no-toolchain
endif

no-toolchain:
	@echo "dsp-tap: devkitARM not found (DEVKITARM/DEVKITPRO unset and"
	@echo "         arm-none-eabi-gcc not on PATH) — skipping the plugin build."
	@echo "         This is expected off a devkitARM machine; the plugin is an"
	@echo "         optional console tool and never part of caesar's core build."
	@echo "         To build it: install devkitPro '3ds-dev' + PabloMK7/3gxtool,"
	@echo "         then re-run 'make' in tools/dsp-tap/."
	@exit 0

# --- real build (devkitARM present) --------------------------------------
ifeq ($(HAVE_DKA),yes)

PREFIX  := $(DEVKITARM)/bin/arm-none-eabi-
ifeq ($(wildcard $(DEVKITARM)/bin/arm-none-eabi-gcc),)
  PREFIX :=            # fall back to PATH-resolved cross tools
endif
CC      := $(PREFIX)gcc
CXX     := $(PREFIX)g++
LD      := $(PREFIX)gcc
OBJCOPY := $(PREFIX)objcopy

ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mfpu=vfpv2 -mtp=soft -marm
CFLAGS  := $(ARCH) -Os -Wall -Wextra -ffunction-sections -fdata-sections \
           -fomit-frame-pointer -fno-strict-aliasing \
           -I$(INCLUDE) -I$(DEVKITPRO)/libctru/include \
           -DDSPTAP_CAPTURE_FINAL_SAMPLES=$(FINAL_SAMPLES) -std=gnu11
LDFLAGS := $(ARCH) -T 3gx.ld -nostartfiles -Wl,--gc-sections \
           -L$(DEVKITPRO)/libctru/lib
LIBS    := -lctru

SRCS    := $(SRCDIR)/main.c $(SRCDIR)/tap.c $(SRCDIR)/ring.c
OBJS    := $(patsubst $(SRCDIR)/%.c,$(BUILD)/%.o,$(SRCS))

# The .plgInfo (author/title/targets) drives 3gxtool. See dsp-tap.plgInfo.
PLGINFO := dsp-tap.plgInfo
GXTOOL  := 3gxtool

plugin: $(BUILD)/$(TARGET).3gx

$(BUILD)/$(TARGET).3gx: $(BUILD)/$(TARGET).elf $(PLGINFO)
	@echo "[3gxtool] $@"
	$(GXTOOL) -d $< $(PLGINFO) $@ || \
	  (echo "3gxtool failed or missing — install PabloMK7/3gxtool"; exit 1)

$(BUILD)/$(TARGET).elf: $(OBJS) 3gx.ld
	@echo "[LD] $@"
	$(LD) $(OBJS) $(LDFLAGS) $(LIBS) -o $@

$(BUILD)/%.o: $(SRCDIR)/%.c | $(BUILD)
	@echo "[CC] $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	@mkdir -p $(BUILD)

endif # HAVE_DKA

clean:
	@rm -rf $(BUILD)
