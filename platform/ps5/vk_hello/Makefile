# Spike B payloads: the memory/AGC probes and vk_hello, which links the PS5
# RADV archive directly (no Vulkan loader).
#
#   platform/ps5/radv/build-in-docker.sh               # once: the RADV archive
#   export PS5_PAYLOAD_SDK="$SPLAT_DATA_DIR/ref/radv-ps5/sdk"
#   export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config
#   make -C platform/ps5/vk_hello                     # probes + vk_hello
#   make -C platform/ps5/vk_hello probe_mem.elf
#
# vk_hello needs the RADV archive built outside the repo (platform/ps5/radv/
# build-in-docker.sh); its directory comes from RADV_PS5, default
# $SPLAT_DATA_DIR/ref/radv-ps5, and must hold:
#   lib/libvulkan_radeon.ps5.a
#   lib/libclang_rt.builtins-x86_64.a
#   lib/libSceAgc.so, lib/libSceAgcDriver.so   (import stubs, built there)
#   include/vulkan/...
#   sdk/                       (PS5_PayloadSDK with libps5platform.a)
# See docs/research/spike-b.md for the build route and the revisions.
# SPDX-License-Identifier: GPL-3.0-or-later

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

CXXFLAGS := -std=c++17 -Wall -Werror -O2 -g
PROBES   := probe_mem.elf probe_agc.elf
# A fixed build-id makes a rebuilt probe byte-identical, as vk_hello is; the
# toolchain's default is a random UUID.
PROBE_LDFLAGS := -Wl,--build-id=sha1

# The RADV build, its headers and the SDK it was built against. Spelled without
# a default that expands to an absolute path in the repo.
SPLAT_DATA_DIR ?= $(abspath $(CURDIR)/../../../..)
RADV_PS5       ?= $(SPLAT_DATA_DIR)/ref/radv-ps5
RADV_SDK       ?= $(RADV_PS5)/sdk
RADV_LIB       ?= $(RADV_PS5)/lib/libvulkan_radeon.ps5.a
RADV_BUILTINS  ?= $(RADV_PS5)/lib/libclang_rt.builtins-x86_64.a
GLSLANG        ?= glslangValidator
GEN            := build/gen
FILL_SHADER    := ../../../core/render/shaders/fill.comp

all: $(PROBES) vk_hello.elf

probe_mem.elf: probe_mem.cpp
	$(CXX) $(CXXFLAGS) $(PROBE_LDFLAGS) -o $@ $^

# The SDK's own link line names no AGC module: a payload that calls them
# imports libSceAgc/libSceAgcDriver explicitly, as a title does, and RADV's
# archive needs libSceVideoOut for its WSI. All three, so the probe tests what
# vk_hello's NEEDED list names.
AGC_STUBS := $(RADV_PS5)/lib/libSceAgc.so $(RADV_PS5)/lib/libSceAgcDriver.so
probe_agc.elf: probe_agc.cpp $(AGC_STUBS) $(RADV_SDK)/target/lib/libSceVideoOut.so
	$(CXX) $(CXXFLAGS) $(PROBE_LDFLAGS) -o $@ $< -L$(RADV_PS5)/lib -lSceAgc -lSceAgcDriver -lSceVideoOut

# fill.comp -> SPIR-V as a uint32_t array (glslangValidator --vn), embedded in
# the payload: no shader file has to reach the console.
$(GEN)/fill_spv.h: $(FILL_SHADER)
	@mkdir -p $(GEN)
	$(GLSLANG) -V --target-env vulkan1.1 --vn vk_hello_fill_spv $< -o $@

# The same link inputs PS5_Vulkan's tools/radv-link.sh gives a title, with the
# payload SDK's crt and libc instead of the title converter: RADV whole (its
# dispatch tables name entry points through weak references), libc++/libc++abi
# for ACO, libunwind, Clang's builtins for __emutls_get_address, and the SDK
# fork's platform layer for the libc functions no system module exports. The
# lists and flags live in platform/ps5/radv-link.mk, shared with the bench
# payloads so the recipe cannot drift.
include $(CURDIR)/../radv-link.mk

VK_LDFLAGS := $(RADV_LINK_BASE) \
              -Wl,--start-group $(RADV_SDK_LIBS) -Wl,--end-group \
              $(RADV_WRAPS) $(RADV_DEFSYMS) $(RADV_SYSTEM_LIBS)

vk_hello.elf: main.cpp $(GEN)/fill_spv.h $(RADV_LIB) $(RADV_BUILTINS) \
              $(RADV_SDK)/target/lib/libps5platform.a \
              $(RADV_PS5)/lib/libSceAgc.so $(RADV_PS5)/lib/libSceAgcDriver.so
	$(CXX) $(CXXFLAGS) -fno-exceptions -fno-rtti \
	    -I$(GEN) -I$(RADV_PS5)/include -o $@ main.cpp $(VK_LDFLAGS)

# A clear message instead of "No rule to make target" when the archive has not
# been built yet.
$(RADV_LIB) $(RADV_BUILTINS) $(RADV_SDK)/target/lib/libps5platform.a \
$(RADV_PS5)/lib/libSceAgc.so $(RADV_PS5)/lib/libSceAgcDriver.so:
	@echo "missing $@" >&2
	@echo "build the RADV side first: platform/ps5/radv/build-in-docker.sh" >&2
	@exit 1

# The same main.cpp as a homebrew title: platform/ps5/title/ compiles it with
# -DPS5_TITLE, links it with the title CRT and packages
# build/ps5/title/<TITLE_ID>/. The payload targets above are unaffected.
vk_hello-title:
	$(MAKE) -C ../title

vk_hello-title-clean:
	$(MAKE) -C ../title title-clean

clean:
	rm -rf $(GEN) $(PROBES) vk_hello.elf

.PHONY: all clean vk_hello-title vk_hello-title-clean
