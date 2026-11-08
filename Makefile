# mpc-fm build
#
#   make toolchain   download the pinned zig (no root needed)
#   make check       host build + tests (launcher, autorun chain, probe)
#   make arm         aarch64 build for the MPC Sample + static checks
#   make dist        SD card folder in dist/autorun/
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

FM_SRC = src/fm.c src/alsa_probe.c
FM_HDR = src/probe.h src/version.h

ARM = build/aarch64
HOST = build/host

.PHONY: all toolchain arm check host dist clean
all: check arm

toolchain: $(ZIG)
$(ZIG):
	sh tools/fetch-toolchain.sh

# ---- device build ----
arm: $(ARM)/fm.so $(ARM)/setarch
	sh tools/verify-arm.sh $(ARM)

$(ARM)/fm.so: $(FM_SRC) $(FM_HDR) | $(ZIG)
	@mkdir -p $(@D)
	$(CC_ARM) $(CFLAGS) $(SO_FLAGS) -o $@ $(FM_SRC) -ldl -lpthread

$(ARM)/setarch: launcher/setarch.c | $(ZIG)
	@mkdir -p $(@D)
	$(CC_ARM_STATIC) $(CFLAGS) -o $@ $<

dist: arm
	sh tools/mkdist.sh $(ARM) dist

# ---- host build and tests ----
host: $(HOST)/fm.so $(HOST)/setarch $(HOST)/fake_app

$(HOST)/fm.so: $(FM_SRC) $(FM_HDR)
	@mkdir -p $(@D)
	$(CC_HOST) $(CFLAGS) $(SO_FLAGS) -DMPCFM_TARGET_EXE='"$(abspath $(HOST)/fake_app)"' \
		-o $@ $(FM_SRC) -ldl -lpthread

$(HOST)/setarch: launcher/setarch.c
	@mkdir -p $(@D)
	$(CC_HOST) $(CFLAGS) -DMPCFM_TEST -o $@ $<

$(HOST)/libfakeasound.so: tests/fake_asound.c
	@mkdir -p $(@D)
	$(CC_HOST) $(CFLAGS) -shared -fPIC -o $@ $<

$(HOST)/fake_app: tests/fake_app.c $(HOST)/libfakeasound.so
	$(CC_HOST) $(CFLAGS) -o $@ $< -L$(HOST) -lfakeasound -Wl,-rpath,'$$ORIGIN'

check: host
	sh tests/test_launcher.sh $(HOST)/setarch
	sh tests/test_autorun.sh $(HOST)
	sh tests/test_probe.sh $(HOST)

clean:
	rm -rf build dist
