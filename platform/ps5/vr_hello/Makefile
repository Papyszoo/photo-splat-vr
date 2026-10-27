# Builds two things from the same directory:
#
#   vr_hello.elf            the payload (main.cpp + ../hmd2/libhmd2.a), the
#                           Spike B step 2 probe; the default goal.
#   build/ps5/title/PPSA99202/
#                           the title (title.cpp + ../hmd2/hmd2.cpp + the
#                           viewer's Vulkan entry-point shims, packaged by
#                           ../title/build-title.mk), the Spike B step 3 probe:
#                           a RADV-rendered pattern flipped to PSVR2. It is the
#                           same title as the viewer and the benches, so it
#                           replaces the installed eboot (run-vr-hello.sh says
#                           so; run-viewer.sh installs the viewer again).
#
#   export PS5_PAYLOAD_SDK="$SPLAT_DATA_DIR/ref/toolchains/ps5-payload-sdk"
#   export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config
#   make                                   # vr_hello.elf
#   make vr-hello-title                    # the package (needs the RADV SDK,
#                                          # see ../title/build-title.mk)
#
# The pattern shaders are compiled to SPIR-V and embedded as headers, so the
# package carries no shader files and the title reads nothing at run time. That
# needs glslangValidator on the build host (Homebrew's `glslang`).
# SPDX-License-Identifier: GPL-3.0-or-later

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := vr_hello.elf
LIB := ../hmd2/libhmd2.a

CXXFLAGS := -std=c++17 -Wall -Werror -O2 -g -fno-exceptions -fno-rtti
# The libraries the payload imports by name: VideoOut for the VR port, Sysmodule
# to load Hmd2/VrTracker2, UserService for the user the headset session is for.
LDLIBS := -lSceVideoOut -lSceSysmodule -lSceUserService

all: $(ELF)

$(ELF): main.cpp $(LIB) ../hmd2/hmd2.h
	$(CXX) $(CXXFLAGS) -o $@ main.cpp $(LIB) $(LDLIBS)

# Always recurse: ../hmd2 decides what needs recompiling.
$(LIB): FORCE
	$(MAKE) -C ../hmd2

FORCE:

# ---- the title package ------------------------------------------------------
REPO            := $(abspath $(CURDIR)/../../..)
SPLAT_DATA_DIR  ?= $(abspath $(REPO)/..)
RADV_PS5        ?= $(SPLAT_DATA_DIR)/ref/radv-ps5
RADV_SDK        ?= $(RADV_PS5)/sdk
RADV_LIB        := $(RADV_PS5)/lib/libvulkan_radeon.ps5.a
RADV_BUILTINS   := $(RADV_PS5)/lib/libclang_rt.builtins-x86_64.a
GLSLANG         ?= glslangValidator
TITLE_BUILD     ?= $(REPO)/build/ps5/title
TITLE_GEN       := $(TITLE_BUILD)/gen
PATTERN_VERT_H  := $(TITLE_GEN)/pattern_vert.h
PATTERN_FRAG_H  := $(TITLE_GEN)/pattern_frag.h
# The viewer's loader-less Vulkan entry points; reused, not copied, so there is
# one shim list for every title that links RADV directly.
VK_SHIMS        := ../viewer/vk_entrypoints.cpp
# The eye buffers this title hands Hmd2 are the shared implementation
# (platform/ps5/viewer/eye_buffers.cpp), also used by the viewer title's VR mode.
EYE_BUFFERS     := ../viewer/eye_buffers.cpp

TITLE_STEM       := vr_hello_title
TITLE_SOURCES    := title.cpp ../hmd2/hmd2.cpp $(VK_SHIMS) $(EYE_BUFFERS)
TITLE_HEADERS    := ../hmd2/hmd2.h ../viewer/eye_buffers.h ../viewer/vk_common.h \
                    $(PATTERN_VERT_H) $(PATTERN_FRAG_H)
TITLE_CPPFLAGS   := -DPS5_TITLE -I$(TITLE_GEN) -I$(RADV_PS5)/include
TITLE_CXXFLAGS   := $(CXXFLAGS) -ffunction-sections -fdata-sections
TITLE_EXTRA_LOCAL := $(shell grep -ohE '\bvk[A-Z][A-Za-z0-9_]*' $(VK_SHIMS) | sort -u)

$(PATTERN_VERT_H): shaders/pattern.vert
	@mkdir -p $(@D)
	$(GLSLANG) -V --target-env vulkan1.3 --vn vrhello_pattern_vert $< -o $@

$(PATTERN_FRAG_H): shaders/pattern.frag
	@mkdir -p $(@D)
	$(GLSLANG) -V --target-env vulkan1.3 --vn vrhello_pattern_frag $< -o $@

include $(REPO)/platform/ps5/title/build-title.mk

# TITLE_OBJDIR and TITLE_STEM exist only after the include.
$(TITLE_OBJDIR)/$(TITLE_STEM)/title.o: $(PATTERN_VERT_H) $(PATTERN_FRAG_H)

# One command for the package; `make` still builds the payload ELF.
vr-hello-title: title-package

$(RADV_LIB) $(RADV_BUILTINS) $(RADV_SDK)/target/lib/libps5platform.a:
	@echo "missing $@" >&2
	@echo "build the RADV side first: platform/ps5/radv/build-in-docker.sh" >&2
	@exit 1

clean:
	rm -f $(ELF)
	$(MAKE) title-clean

.PHONY: all clean FORCE vr-hello-title
