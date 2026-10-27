# Package a payload-style program as a PS5 homebrew title.
#
# "Payload-style" means what platform/ps5/vk_hello, platform/ps5/bench and the
# other probes already are: a normal C++ program with main(), built for
# x86_64-sie-ps5, that links the RADV archive directly. This file turns exactly
# that program into the folder ShadowMountPlus registers and the home screen
# starts (docs/research/spike-b-memory.md sections 5-7):
#
#   build/ps5/title/<TITLE_ID>/
#     eboot.bin                 # signed fSELF, magic 0x1D3D154F
#     sce_module/libc.prx       # C-library companion module (hash-checked)
#     sce_sys/param.json        # generated: title id, content id, amm/kernel sizes
#     sce_sys/icon0.png         # generated placeholder, "Photo Splat VR"
#
# The identity is `identities.json` in this directory: the dev build by default,
# the release build with `RELEASE=1`, and `TITLE_ID=…` selects one of the two by
# id (identity.py refuses any other id, so a command-line value cannot get past
# it). It decides the package directory, the generated param.json, the data
# folder compiled into the title (`SPLAT_DATA_ROOT`) and the `SPLAT_TITLE_ID`
# string a title can log.
#
# Usage, from a program's Makefile that already has PS5_PAYLOAD_SDK (the SDK's
# prospero.mk) before this include:
#
#   TITLE_SOURCES  := main.cpp
#   TITLE_CPPFLAGS := -DPS5_TITLE -I$(GEN)
#   TITLE_STEM     := mytitle
#   TITLE_EXTRA_LIBS := libfoo.a             # a program with its own archives
#   TITLE_EXTRA_LDFLAGS := --wrap=some_call  # extra lld arguments
#   include ../title/build-title.mk
#   all: title-package
#
# Every program packaged this way is the same title (TITLE_ID from
# identities.json), so TITLE_STEM — not the id — is what keeps their objects,
# PIE and eboot.elf apart under $(TITLE_BUILD)/obj/$(TITLE_ID)/ (one folder per
# identity, so the two builds never reuse each other's objects).
#
# What differs from the payload link (radv-link.mk), and why:
#   - the SDK's crt1.o is replaced by tooling/native/app_crt.cpp, whose
#     _start(process_parameters, loader_teardown) the console's title loader
#     calls (a payload's crt gets (argc, argv) from elfldr instead);
#   - tooling/psbc/ps5-pie-unwind.ld supplies the unwind boundaries ACO's
#     libunwind looks for, and tooling/native/ps5-pie.ld the segment layout;
#     this directory's ps5-pie-title.ld is that layout plus one reserved page
#     for the process parameters ps5-native-tool writes into the RELRO tail
#     (see its header: without the reserve the headroom is luck);
#   - the linked PIE is converted to a title ELF by `ps5-native-tool link`
#     (imports become NIDs, the SDK's .so stubs name the modules) and signed as
#     a fake SELF by `ps5-native-tool self --sign`.
# Those three pieces are the ps5-native-app-boilerplate tooling, kept outside
# this repo like the RADV archive; a title that swaps in its own copies only
# has to point TITLE_TOOLING and TITLE_LIBC_PRX at them.
#
# Expects the including Makefile to have defined, before the include:
#   RADV_PS5      the RADV build directory (lib/, sdk/, include/)
#   RADV_SDK      the SDK RADV was built against
#   RADV_LIB      libvulkan_radeon.ps5.a
#   RADV_BUILTINS libclang_rt.builtins-x86_64.a
# (the same four radv-link.mk needs; it is included here if not already)
#
# SPDX-License-Identifier: GPL-3.0-or-later

TITLE_RECIPE_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
TITLE_REPO_ROOT  := $(abspath $(TITLE_RECIPE_DIR)/../../..)
SPLAT_DATA_DIR   ?= $(abspath $(TITLE_REPO_ROOT)/..)

# --- the identity ------------------------------------------------------------
# Three identities in identities.json: the dev build (default), the fresh
# pre-release check build (`IDENTITY=fresh`) and the release build
# (`RELEASE=1`). `TITLE_ID=…` selects one of them by id, and any other id is
# refused there rather than silently producing a package whose contentId does
# not carry it, or whose data dir is not its own. The table is the only place an
# id or a console data path is written down: the package's param.json is
# generated from the identity-free param.json template below, and the data root
# is compiled in from the same entry.
#
# The rules themselves live in identity.mk, the one file every console Makefile
# includes (platform/ps5/helper/Makefile too), so a title and the helper it ships
# always resolve IDENTITY / RELEASE / TITLE_ID the same way.
include $(TITLE_RECIPE_DIR)/identity.mk

TITLE_PARAM_JSON ?= $(TITLE_RECIPE_DIR)/param.json
TITLE_STEM     ?= $(notdir $(CURDIR))
TITLE_SOURCES  ?=
TITLE_CPPFLAGS ?=
TITLE_BUILD    ?= $(TITLE_REPO_ROOT)/build/ps5/title
TITLE_DIR      := $(TITLE_BUILD)/$(TITLE_ID)

# Extra link inputs for a program that is more than one translation unit: the
# program's own static libraries (placed in the SDK group below) and extra lld
# arguments (--wrap=..., for example). Empty for vk_hello.
TITLE_EXTRA_LIBS    ?=
TITLE_EXTRA_LDFLAGS ?=
# Names the program defines that must not be exported (the viewer's Vulkan
# entry-point shims: a title exports nothing, and the converter refuses an
# executable that does). Empty for vk_hello.
TITLE_EXTRA_LOCAL   ?=
# Headers every object depends on (a multi-source program has its own); the
# single-source vk_hello needs none.
TITLE_HEADERS       ?=

# The title id is a compile-time string, so a program can print which title it
# runs as (vr_hello's start line) without naming an id in its source. The data
# root is compiled in from the same identity entry: a title and its helper
# write only their own folder, and a second install has its own. Both are the
# values the package's param.json is generated from.
TITLE_CPPFLAGS += -DSPLAT_TITLE_ID=\"$(TITLE_ID)\" -DSPLAT_DATA_ROOT=\"$(TITLE_DATA_DIR)\"

# External material, never in the repo (AGENTS.md): the title CRT and the
# companion libc module come from the reference clones, the same way the RADV
# archive does.
TITLE_TOOLING       ?= $(SPLAT_DATA_DIR)/ref/PS5_Vulkan/tooling
# The PIE layout is a repo copy of the boilerplate's ps5-pie.ld (plus the
# unwind bounds) because it reserves a page for the process parameters
# ps5-native-tool writes into the RELRO tail; see the script's header.
TITLE_PIE_LAYOUT    ?= $(TITLE_RECIPE_DIR)/ps5-pie-title.ld
TITLE_LIBC_PRX      ?= $(SPLAT_DATA_DIR)/ref/PS5VR/app/tools/native-app/runtime/libc.prx
TITLE_LIBC_PRX_SHA256 ?= e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036
# A macOS arm64 build of the converter/signer, PS5VR's copy of the same
# boilerplate: a dev-loop tool. The Linux/Docker toolchain builds it from
# source for a release (tools/build-native.sh in that project).
PS5_NATIVE_TOOL ?= $(SPLAT_DATA_DIR)/ref/PS5VR/app/tools/native-mac/ps5-native-tool

MODULE_SDK    := 0x02000009
COMPANION_SDK := 0x08050001
FSELF_MAGIC   := 0x1D3D154F

# --- the link inputs every title shares --------------------------------------
RADV_PS5      ?= $(SPLAT_DATA_DIR)/ref/radv-ps5
RADV_SDK      ?= $(RADV_PS5)/sdk
RADV_LIB      ?= $(RADV_PS5)/lib/libvulkan_radeon.ps5.a
RADV_BUILTINS ?= $(RADV_PS5)/lib/libclang_rt.builtins-x86_64.a

ifeq ($(origin RADV_LINK_BASE),undefined)
include $(TITLE_REPO_ROOT)/platform/ps5/radv-link.mk
endif

# Intermediates are per identity: the compile carries SPLAT_TITLE_ID and
# SPLAT_DATA_ROOT, so an object built for the dev folder may never be linked
# into the release package (and a PIE/eboot.elf may never be signed twice for
# two identities). The package directory stays build/ps5/title/<TITLE_ID>.
TITLE_OBJDIR   := $(TITLE_BUILD)/obj/$(TITLE_ID)
TITLE_OBJS     := $(patsubst %,$(TITLE_OBJDIR)/$(TITLE_STEM)/%.o,$(basename $(notdir $(TITLE_SOURCES))))
TITLE_CRT_OBJS := $(TITLE_OBJDIR)/app_crt.o $(TITLE_OBJDIR)/app_cpp_runtime.o
TITLE_PIE      := $(TITLE_OBJDIR)/$(TITLE_STEM)/llvm-pie.elf
TITLE_ELF      := $(TITLE_OBJDIR)/$(TITLE_STEM)/eboot.elf
TITLE_MAP      := $(TITLE_OBJDIR)/$(TITLE_STEM)/title-local.map
TITLE_CXXFLAGS ?= -std=c++17 -O2 -g -ffunction-sections -fdata-sections
TITLE_CRT_FLAGS := -std=c++20 -O2 -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti

# The link is done with lld directly (as PS5LM and PS5_Vulkan link titles), so
# radv-link.mk's compiler-driver spellings are rewritten to lld's own arguments
# and its -nodefaultlibs is dropped (lld has no default libraries to drop).
TITLE_WRAPS    := $(subst -Wl$(RADV_COMMA)--wrap$(RADV_COMMA),--wrap=,$(RADV_WRAPS))
TITLE_LINK_INPUTS := --build-id=sha1 --whole-archive $(RADV_LIB) --no-whole-archive

# Names the SDK fork's platform layer supplies that radv-link.mk's list does
# not carry (radv-link.sh binds these when the pinned SDK defines them). A
# title's import of readlink/link/symlink/strcasestr/mkstemp is NULL at run
# time otherwise; RADV itself calls readlink and mkstemp.
TITLE_PLATFORM_EXTRA := $(shell for n in localeconv strcasestr mkstemp readlink link symlink; do \
    if $(RADV_SDK)/bin/llvm-nm --defined-only $(RADV_SDK)/target/lib/libps5platform.a 2>/dev/null \
        | grep -q "^ T ps5_$$n$$"; then printf '%s ' $$n; fi; done)
TITLE_EXTRA_DEFSYMS := $(foreach n,$(TITLE_PLATFORM_EXTRA),--defsym=$(n)=ps5_$(n))
TITLE_DEFSYMS := $(subst -Wl$(RADV_COMMA)--defsym$(RADV_COMMA),--defsym=,$(RADV_DEFSYMS)) $(TITLE_EXTRA_DEFSYMS)

# Every name this link binds stays local: the title converter refuses an
# executable that exports symbols, and the SDK's stubs define some of the same
# names. The C++ allocation operators stay internal too, as the boilerplate's
# app-symbols.map has them.
TITLE_LOCAL_SYMBOLS := $(RADV_DEFSYM_NAMES) $(TITLE_PLATFORM_EXTRA) $(TITLE_EXTRA_LOCAL)

$(TITLE_MAP): $(TITLE_RECIPE_DIR)/build-title.mk
	@mkdir -p $(@D)
	@printf '{\n    local:\n' > $@
	@for n in $(TITLE_LOCAL_SYMBOLS); do printf '        %s;\n' "$$n" >> $@; done
	@printf '        _Zn*;\n        _Zd*;\n};\n' >> $@

TITLE_SDK_SOS := $(wildcard $(RADV_SDK)/target/lib/*.so)
TITLE_AGC_STUBS := $(RADV_PS5)/lib/libSceAgc.so $(RADV_PS5)/lib/libSceAgcDriver.so

TITLE_LDFLAGS := -T $(TITLE_PIE_LAYOUT) --eh-frame-hdr -e _start --exclude-libs=ALL \
                 --version-script $(TITLE_MAP) \
                 $(TITLE_LINK_INPUTS) $(TITLE_WRAPS) $(TITLE_DEFSYMS) $(TITLE_EXTRA_LDFLAGS)

# --- compile -----------------------------------------------------------------
$(TITLE_OBJDIR)/app_crt.o: $(TITLE_TOOLING)/native/app_crt.cpp
	@mkdir -p $(@D)
	$(CXX) $(TITLE_CRT_FLAGS) -c -o $@ $<

$(TITLE_OBJDIR)/app_cpp_runtime.o: $(TITLE_TOOLING)/native/app_cpp_runtime.cpp
	@mkdir -p $(@D)
	$(CXX) $(TITLE_CRT_FLAGS) -c -o $@ $<

# The program's sources keep their basenames, so two programs in one build
# tree cannot collide.
define TITLE_COMPILE_RULE
$(TITLE_OBJDIR)/$(TITLE_STEM)/$(basename $(notdir $(1))).o: $(1) $(TITLE_HEADERS)
	@mkdir -p $$(@D)
	$(CXX) $(TITLE_CXXFLAGS) $(TITLE_CPPFLAGS) -c -o $$@ $$<
endef
$(foreach source,$(TITLE_SOURCES),$(eval $(call TITLE_COMPILE_RULE,$(source))))

# --- link, convert, sign, package --------------------------------------------
$(TITLE_PIE): $(TITLE_OBJS) $(TITLE_CRT_OBJS) $(TITLE_MAP) $(RADV_LIB) $(RADV_BUILTINS) \
              $(RADV_SDK)/target/lib/libps5platform.a $(TITLE_AGC_STUBS) \
              $(TITLE_PIE_LAYOUT)
	@mkdir -p $(@D)
	$(LD) $(TITLE_LDFLAGS) -L $(RADV_SDK)/target/lib -o $@ \
	    $(TITLE_CRT_OBJS) $(TITLE_OBJS) \
	    --start-group $(TITLE_EXTRA_LIBS) $(RADV_SDK_LIBS) --end-group \
	    $(TITLE_AGC_STUBS) \
	    --as-needed $(TITLE_SDK_SOS)

$(TITLE_ELF): $(TITLE_PIE) $(TITLE_AGC_STUBS)
	$(PS5_NATIVE_TOOL) link --in $< --out $@ \
	    --stub-dir $(RADV_SDK)/target/lib --stub $(RADV_PS5)/lib/libSceAgc.so \
	    --stub $(RADV_PS5)/lib/libSceAgcDriver.so --module-sdk $(MODULE_SDK) \
	    --companion-sdk $(COMPANION_SDK) --file-name eboot.elf

# The package folder is shared by every program (one app, one id), so eboot.bin
# belongs to whichever program signed it last: `make` in one program's directory
# must not leave another program's eboot behind, and mtime cannot tell (a viewer
# build leaves a newer eboot.bin than vr_hello's PIE). A phony prerequisite makes
# the sign step run on every make of a package — tens of milliseconds for 26 MiB
# — so the last program built is the one installed, exactly as a test run
# replaces the viewer's eboot in place. Defined before the rule: make expands a
# rule's prerequisites when it reads the line.
TITLE_OWNER := $(TITLE_ID)-owner
.PHONY: $(TITLE_OWNER)
$(TITLE_OWNER):

$(TITLE_DIR)/eboot.bin: $(TITLE_ELF) $(TITLE_OWNER)
	@mkdir -p $(@D)
	$(PS5_NATIVE_TOOL) self --sign --in $< --out $@ --magic $(FSELF_MAGIC)

$(TITLE_DIR)/sce_sys/param.json: $(TITLE_PARAM_JSON) $(TITLE_IDENTITIES) $(TITLE_IDENTITY_PY)
	@mkdir -p $(@D)
	python3 $(TITLE_IDENTITY_PY) --param $< --out $@ $(TITLE_IDENTITY_SELECTOR)

$(TITLE_DIR)/sce_sys/icon0.png: $(TITLE_RECIPE_DIR)/gen-icon.py
	@mkdir -p $(@D)
	python3 $< -o $@

$(TITLE_DIR)/sce_module/libc.prx: $(TITLE_LIBC_PRX) $(TITLE_RECIPE_DIR)/build-title.mk
	@mkdir -p $(@D)
	python3 -c 'import hashlib,sys; h=hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest(); \
		sys.exit(0 if h == sys.argv[2] else "libc.prx hash %s != published %s (set TITLE_LIBC_PRX)" % (h, sys.argv[2]))' \
		$< $(TITLE_LIBC_PRX_SHA256)
	cp $< $@

title-package: $(TITLE_DIR)/eboot.bin $(TITLE_DIR)/sce_sys/param.json \
               $(TITLE_DIR)/sce_sys/icon0.png $(TITLE_DIR)/sce_module/libc.prx
	@$(PS5_NATIVE_TOOL) self --inspect --file $(TITLE_DIR)/eboot.bin > /dev/null
	@$(PS5_NATIVE_TOOL) self --inspect --file $(TITLE_DIR)/sce_module/libc.prx > /dev/null
	@printf 'title: %s (%s bytes eboot.bin, %s bytes total)\n' "$(TITLE_DIR)" \
	    "$$(stat -f%z $(TITLE_DIR)/eboot.bin 2>/dev/null || stat -c%s $(TITLE_DIR)/eboot.bin)" \
	    "$$(du -sk $(TITLE_DIR) | awk '{print $$1 * 1024}')"

title-clean:
	rm -rf $(TITLE_DIR) $(TITLE_OBJDIR)/$(TITLE_STEM) $(TITLE_CRT_OBJS) $(TITLE_PIE) $(TITLE_ELF) $(TITLE_MAP)

.PHONY: title-package title-clean
