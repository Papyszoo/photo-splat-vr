# Build a Photo Splat VR homebrew title folder.
#
#   export PS5_PAYLOAD_SDK="$SPLAT_DATA_DIR/ref/radv-ps5/sdk"
#   export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config
#   make -C platform/ps5/title                     # build/ps5/title/PPSA99202/ (dev)
#   make -C platform/ps5/title RELEASE=1           # build/ps5/title/PPSA99770/
#   make -C platform/ps5/title title-clean
#
# The identity comes from identities.json in this directory (build-title.mk):
# the dev build by default, the release build with RELEASE=1, and TITLE_ID=…
# selects one of the two by id. param.json here is the identity-free template;
# the package's sce_sys/param.json is generated from it at package time, and the
# identity's data folder is compiled in, so no file repeats an id or a console
# data path.
#
# The recipe is build-title.mk (this directory): it links the program with the
# title CRT, converts and signs it, and writes the folder ShadowMountPlus
# registers. Every program in this tree is that same title (docs/PLAN.md, "One
# app on the console"): vk_hello from this directory, the bench title from
# platform/ps5/bench, vr_hello and the viewer — one id, one tile, and an install
# updates it in place.
#
#   platform/ps5/vk_hello/main.cpp                  platform/ps5/title/Makefile
#   platform/ps5/bench/                             platform/ps5/bench/Makefile
#     bench_title.cpp + the three bench sources
#
# so `make -C platform/ps5/vk_hello` still produces the payload ELFs and
# `make -C platform/ps5/vk_hello vk_hello-title` forwards here. Any other
# payload-style program becomes this title by setting TITLE_SOURCES,
# TITLE_STEM and TITLE_CPPFLAGS and including build-title.mk the same way.
#
# Output and inputs are documented in README.md here; the package is under
# build/ps5/title/ (gitignored, like every build*/ directory).
#
# SPDX-License-Identifier: GPL-3.0-or-later

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

CXXFLAGS := -std=c++17 -Wall -Werror -O2 -g

# External material lives outside the repo (AGENTS.md): SPLAT_DATA_DIR is the
# repository's parent directory, the same default build-title.mk uses.
SPLAT_DATA_DIR ?= $(abspath $(CURDIR)/../../../..)
RADV_PS5       ?= $(SPLAT_DATA_DIR)/ref/radv-ps5
RADV_SDK       ?= $(RADV_PS5)/sdk
RADV_LIB       ?= $(RADV_PS5)/lib/libvulkan_radeon.ps5.a
RADV_BUILTINS  ?= $(RADV_PS5)/lib/libclang_rt.builtins-x86_64.a
GLSLANG        ?= glslangValidator

PROGRAM_DIR := ../vk_hello
PROGRAM     := vk_hello
TITLE_BUILD ?= $(CURDIR)/../../../build/ps5/title
TITLE_GEN   := $(TITLE_BUILD)/gen
FILL_SHADER := $(PROGRAM_DIR)/../../../core/render/shaders/fill.comp

TITLE_STEM     := $(PROGRAM)
TITLE_SOURCES  := $(PROGRAM_DIR)/main.cpp
TITLE_CPPFLAGS := -DPS5_TITLE -I$(TITLE_GEN) -I$(RADV_PS5)/include
# Same warnings as the payload build (CXXFLAGS above), plus the section flags
# the title link's --gc-sections-free layout expects.
TITLE_CXXFLAGS := $(CXXFLAGS) -ffunction-sections -fdata-sections

all: title-package

# The same embedded compute shader the payload uses, compiled here so this
# directory builds on its own (vk_hello's own build/gen is the payload's).
$(TITLE_GEN)/fill_spv.h: $(FILL_SHADER)
	@mkdir -p $(TITLE_GEN)
	$(GLSLANG) -V --target-env vulkan1.1 --vn vk_hello_fill_spv $< -o $@

include build-title.mk

# The shader header is a prerequisite of the program object (TITLE_OBJDIR is
# defined by build-title.mk, so this cannot come before the include).
$(TITLE_OBJDIR)/$(TITLE_STEM)/main.o: $(TITLE_GEN)/fill_spv.h

$(RADV_LIB) $(RADV_BUILTINS) $(RADV_SDK)/target/lib/libps5platform.a:
	@echo "missing $@" >&2
	@echo "build the RADV side first: platform/ps5/radv/build-in-docker.sh" >&2
	@exit 1

clean: title-clean
	rm -rf $(TITLE_GEN)

.PHONY: all clean
