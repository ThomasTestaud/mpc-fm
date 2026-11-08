# mpc-fm build
#
#   make toolchain   download the pinned zig (no root needed)
#   make check       host build + tests (engine, launcher, autorun chain, probe)
#   make arm         aarch64 build for the MPC Sample + static checks
#   make dist        SD card folder in dist/autorun/
#   make bench       FM engine benchmark on this computer (the device one is
#                    build/aarch64/fmbench: copy it to the MPC and run it there)
#   make wavs        render every preset to build/wav/ to listen to them
#   make samples     FM sounds as WAV one-shots for pads: dist/MPC-FM-Samples/
#
# Cross build: zig cc. It links fm.so against glibc $(GLIBC) symbol
# versions, so it runs on the device's glibc (>= 2.30, as SampQOL needs).

ZIG_VERSION := 0.16.0
ZIG ?= $(CURDIR)/.tools/zig-$(ZIG_VERSION)/zig
export ZIG_GLOBAL_CACHE_DIR := $(CURDIR)/.tools/zig-cache
export ZIG_LOCAL_CACHE_DIR := $(CURDIR)/.tools/zig-cache

GLIBC := 2.30
CPU := cortex_a55
CC_ARM = $(ZIG) cc -target aarch64-linux-gnu.$(GLIBC) -mcpu=$(CPU)
CC_ARM_STATIC = $(ZIG) cc -target aarch64-linux-musl -mcpu=$(CPU) -static -s
CC_HOST ?= cc

CFLAGS = -std=c11 -O2 -Wall -Wextra -Werror -fno-strict-aliasing
SO_FLAGS = -shared -fPIC -fvisibility=hidden -Wl,--build-id=sha1
# The engine: -O3 to vectorize the operator loops (NEON), FMA allowed.
# No -ffast-math: sanitize() relies on NaN checks.
ENGINE_FLAGS = -O3 -fno-math-errno -ffp-contract=fast -Isrc/fm

ENGINE_SRC = src/fm/fm_engine.c src/fm/fm_presets.c src/fm/fm_carrier.c
ENGINE_HDR = src/fm/fm_engine.h src/fm/fm_presets.h src/fm/fm_carrier.h
FM_SRC = src/fm.c src/alsa_probe.c src/voice_hook.c src/discover.c src/crash.c src/sampleinfo.c src/fm/fm_engine.c src/fm/fm_carrier.c
ARM_ASM = src/tramp_hooks.S src/tramp_aarch64.inc
FM_HDR = src/probe.h src/version.h src/voice_hook.h src/discover.h src/crash.h src/sampleinfo.h $(ENGINE_HDR)
# fm.so: the engine runs in it, so the engine flags; BTI/PAC landing pads
# like the stock app (no-ops on the A55).
FM_FLAGS = $(ENGINE_FLAGS) -Isrc
SHIM_SRC = launcher/setarch.c launcher/strip.c
SHIM_HDR = launcher/strip.h launcher/font.h src/version.h
SHIM_FLAGS = -Isrc -Ilauncher

ARM = build/aarch64
HOST = build/host

.PHONY: all toolchain arm check check-arm host dist bench wavs samples clean
all: check arm

toolchain: $(ZIG)
$(ZIG):
	sh tools/fetch-toolchain.sh

# ---- device build ----
arm: $(ARM)/fm.so $(ARM)/setarch $(ARM)/fmbench
	sh tools/verify-arm.sh $(ARM)

# aarch64 tests under qemu (tools/fetch-qemu.sh): the real trampolines.
QEMU ?= $(CURDIR)/.tools/qemu/usr/bin/qemu-aarch64-static
$(ARM)/test_tramp: tests/test_tramp.c tests/tramp_test.S src/tramp_aarch64.inc | $(ZIG)
	@mkdir -p $(@D)
	$(CC_ARM_STATIC) $(CFLAGS) -Isrc -o $@ tests/test_tramp.c tests/tramp_test.S
check-arm: $(ARM)/test_tramp
	$(QEMU) $(ARM)/test_tramp

$(ARM)/fm.so: $(FM_SRC) $(FM_HDR) $(ARM_ASM) | $(ZIG)
	@mkdir -p $(@D)
	$(CC_ARM) $(CFLAGS) $(SO_FLAGS) $(FM_FLAGS) -mbranch-protection=standard -o $@ $(FM_SRC) \
		src/tramp_hooks.S -ldl -lpthread -lm

$(ARM)/setarch: $(SHIM_SRC) $(SHIM_HDR) | $(ZIG)
	@mkdir -p $(@D)
	$(CC_ARM_STATIC) $(CFLAGS) $(SHIM_FLAGS) -o $@ $(SHIM_SRC)

$(ARM)/fmbench: tools/fmbench.c $(ENGINE_SRC) $(ENGINE_HDR) | $(ZIG)
	@mkdir -p $(@D)
	$(CC_ARM_STATIC) $(CFLAGS) $(ENGINE_FLAGS) -o $@ tools/fmbench.c $(ENGINE_SRC) -lm

dist: arm check-arm
	sh tools/mkdist.sh $(ARM) dist

# ---- host build and tests ----
host: $(HOST)/fm.so $(HOST)/setarch $(HOST)/fake_app $(HOST)/test_fm $(HOST)/test_hook \
	$(HOST)/fmbench $(HOST)/fmrender

$(HOST)/fm.so: $(FM_SRC) $(FM_HDR)
	@mkdir -p $(@D)
	$(CC_HOST) $(CFLAGS) $(SO_FLAGS) $(FM_FLAGS) -DMPCFM_TARGET_EXE='"$(abspath $(HOST)/fake_app)"' \
		-o $@ $(FM_SRC) -ldl -lpthread -lm

$(HOST)/test_hook: tests/test_hook.c src/voice_hook.c src/voice_hook.h src/discover.c src/discover.h src/sampleinfo.c src/sampleinfo.h $(ENGINE_SRC) $(ENGINE_HDR)
	@mkdir -p $(@D)
	$(CC_HOST) $(CFLAGS) -DMPCFM_TEST $(FM_FLAGS) -o $@ tests/test_hook.c \
		src/voice_hook.c src/discover.c src/sampleinfo.c $(ENGINE_SRC) -lm

$(HOST)/setarch: $(SHIM_SRC) $(SHIM_HDR)
	@mkdir -p $(@D)
	$(CC_HOST) $(CFLAGS) $(SHIM_FLAGS) -DMPCFM_TEST -o $@ $(SHIM_SRC)

$(HOST)/libfakeasound.so: tests/fake_asound.c
	@mkdir -p $(@D)
	$(CC_HOST) $(CFLAGS) -shared -fPIC -o $@ $<

$(HOST)/fake_app: tests/fake_app.c $(HOST)/libfakeasound.so
	$(CC_HOST) $(CFLAGS) -o $@ $< -L$(HOST) -lfakeasound -Wl,-rpath,'$$ORIGIN'

$(HOST)/test_fm: tests/test_fm.c $(ENGINE_SRC) $(ENGINE_HDR)
	@mkdir -p $(@D)
	$(CC_HOST) $(CFLAGS) -D_GNU_SOURCE $(ENGINE_FLAGS) -o $@ tests/test_fm.c $(ENGINE_SRC) -lm

$(HOST)/fmbench: tools/fmbench.c $(ENGINE_SRC) $(ENGINE_HDR)
	@mkdir -p $(@D)
	$(CC_HOST) $(CFLAGS) $(ENGINE_FLAGS) -o $@ tools/fmbench.c $(ENGINE_SRC) -lm

$(HOST)/fmrender: tools/fmrender.c $(ENGINE_SRC) $(ENGINE_HDR)
	@mkdir -p $(@D)
	$(CC_HOST) $(CFLAGS) $(ENGINE_FLAGS) -o $@ tools/fmrender.c $(ENGINE_SRC) -lm

check: host
	$(HOST)/test_fm
	$(HOST)/test_hook
	sh tests/test_launcher.sh $(HOST)/setarch
	sh tests/test_autorun.sh $(HOST)
	sh tests/test_probe.sh $(HOST)

bench: $(HOST)/fmbench
	$(HOST)/fmbench -q

wavs: $(HOST)/fmrender
	@mkdir -p build/wav
	$(HOST)/fmrender all build/wav

# FM sounds as WAV one-shots for MPC pads (copy the folder to the SD card).
samples: $(HOST)/fmrender
	@mkdir -p dist/MPC-FM-Samples
	$(HOST)/fmrender samples dist/MPC-FM-Samples

# Keeps dist/autorun/logs and anything you put in dist/ by hand.
clean:
	rm -rf build dist/autorun/mpcfm dist/autorun/autorun.sh dist/autorun/autorun.json \
		dist/autorun/autorun.check
