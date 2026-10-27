# The viewer title: a .psplat scene on the TV, through the project's own
# renderer and VideoOut.
#
#   platform/ps5/ggml/build.sh                 # once: ggml + core (includes core/render)
#   export PS5_PAYLOAD_SDK="$SPLAT_DATA_DIR/ref/radv-ps5/sdk"
#   export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config
#   make -C platform/ps5/viewer                # -> build/ps5/title/PPSA99202/ (dev)
#   make -C platform/ps5/viewer RELEASE=1      # -> build/ps5/title/PPSA99770/ (release)
#   platform/ps5/viewer/verify-package.sh build/ps5/title/PPSA99202
#
# Two output modes in one title (viewer.env MODE): vr (default) renders the
# scene stereo into Hmd2's eye buffers for PSVR2 (vr_mode.cpp, ../hmd2), tv
# renders it mono to VideoOut (main.cpp, videoout.cpp).
#
# The package is the title folder ShadowMountPlus registers (build-title.mk)
# plus `shaders/`: the six SPIR-V modules the C6 renderer loads and the two
# overlay (scene label/UI) modules, compiled by the cross build into
# $(BUILD)/shaders and read at run time from /app0/shaders inside the title.
# `samples/` (W31) is optional: the scene material from $SPLAT_DATA_DIR/assets/
# samples, copied in when that folder exists, and read from /app0/samples.
# `helper/splat-helper.elf` is the capture helper payload (platform/ps5/helper):
# the title pushes it to the payload loader on 127.0.0.1:9021 itself, so the
# console needs nothing installed beyond the title.
#
# The program links libsplat_core_ps5.a (core/common, core/scene, core/render,
# core/recon/sharp, ...) and RADV's archive, with the title recipe in
# platform/ps5/title/. It has no Vulkan loader:
# platform/ps5/viewer/vk_entrypoints.cpp forwards every vk* name the renderer
# calls to RADV's ICD entry, and stays local to the title.
#
# It also carries the in-app converter's engine: convert_session.cpp is compiled
# into the title and the ggml/SHARP libraries are linked with the same recipe as
# platform/ps5/bench/Makefile (glue archive, --wrap, one group), so the title
# really contains the reconstruction chain. W18 decides when it runs.
# SPDX-License-Identifier: GPL-3.0-or-later

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

CXXFLAGS := -std=c++17 -Wall -Wextra -Werror -O2 -g

# build-title.mk defines the object/map rules first, so the default goal has to
# be named here (the bench title's Makefile sets `all:` before its include
# instead; this directory includes the recipe before the package rule).
.DEFAULT_GOAL := viewer-package

REPO     := $(abspath $(CURDIR)/../../..)
SPLAT_DATA_DIR ?= $(abspath $(REPO)/..)
RADV_PS5 ?= $(SPLAT_DATA_DIR)/ref/radv-ps5
RADV_SDK ?= $(RADV_PS5)/sdk
RADV_LIB := $(RADV_PS5)/lib/libvulkan_radeon.ps5.a
RADV_BUILTINS := $(RADV_PS5)/lib/libclang_rt.builtins-x86_64.a
BUILD    ?= $(REPO)/build-ps5-ggml
GGML_LIB := $(BUILD)/ggml/src
GLUE     := $(REPO)/platform/ps5/ggml

TITLE_STEM       := viewer
# TITLE_DIR comes from build-title.mk below ($(TITLE_BUILD)/$(TITLE_ID)); it is
# not redefined here because TITLE_BUILD does not exist until the include.
# The id and identity are the title recipe's single identities.json: the viewer
# is the same app as the benches and vr_hello (docs/PLAN.md, "One app on the
# console"), so an install updates that one title in place. RELEASE=1 builds the
# release identity (PPSA99770, the user's install) instead of the dev one
# (PPSA99202); the two installs have separate data folders, and the viewer's
# compiled-in data root comes from the same table (build-title.mk).
# hmd2 is compiled into the title from ../hmd2, as vr_hello's title does:
# one source for both, no separate title-target archive.
# convert_session.cpp is the SHARP converter engine (W4 links it; W18 drove it):
# it is a TITLE_SOURCES object, not an archive member, so the whole
# SharpReconstructor/ggml chain it names really ends up in the eboot.
# ps5_jobs.cpp is the panel's conversion chain (W19): core/ui's JobService over
# core/jobs' queue and runner, with the console's paths and backend factory.
# core/gallery, core/ui (canvas, input script, UiApp, screens, fakes, services,
# job_service), core/model, core/jobs and core/capture are in the cross archive's
# explicit core directory list (platform/ps5/ggml/CMakeLists.txt), so the panel
# needs no title sources for them beyond that assembly. W21's real models chain
# adds two title-side pieces: ui_setup.cpp (the ModelManager + converter +
# fetcher assembly, see ui_setup.h) and W3's sceHttp fetcher, which is
# PS5_TITLE-only and therefore compiled into the title rather than the archive.
TITLE_SOURCES    := main.cpp videoout.cpp vk_entrypoints.cpp eye_buffers.cpp \
                    convert_session.cpp ui_host.cpp ui_setup.cpp ps5_jobs.cpp \
                    ../net/ps5_http_fetcher.cpp \
                    vr_mode.cpp ../hmd2/hmd2.cpp
TITLE_HEADERS    := videoout.h title_common.h vk_common.h eye_buffers.h pad_input.h vr_mode.h \
                    scene_switch.h convert_session.h ui_host.h ui_setup.h ui_mode.h \
                    ps5_jobs.h convert_gate.h ../hmd2/hmd2.h
# -I$(REPO): the fetcher is included by its repo path
# (platform/ps5/net/ps5_http_fetcher.h), the same include root the bench title
# uses.
TITLE_CPPFLAGS   := -DPS5_TITLE -DSPLAT_HAVE_VULKAN=1 -DSPLAT_VERSION=\"0.1.0\" \
                    -I$(REPO) -I$(REPO)/core -I$(REPO)/third_party -I$(REPO)/third_party/json \
                    -I$(RADV_PS5)/include -I$(GLUE) -I$(REPO)/third_party/ggml/include
TITLE_CXXFLAGS   := $(CXXFLAGS) -ffunction-sections -fdata-sections
# Inside the SDK's --start-group with libc++: the project's core archive (which
# now really includes core/recon/sharp), the ggml backends, the PS5 glue
# (ps5_vulkan.cpp's four ggml-vulkan shims and bench_env.cpp's --wrap target),
# and its vendored miniz. The same list platform/ps5/bench/Makefile links.
TITLE_EXTRA_LIBS := $(BUILD)/libsplat_core_ps5.a $(BUILD)/libsplat_ps5_glue.a \
                    $(BUILD)/libsplat_miniz.a \
                    $(GGML_LIB)/ggml-vulkan/libggml-vulkan.a \
                    $(GGML_LIB)/libggml.a $(GGML_LIB)/libggml-base.a $(GGML_LIB)/libggml-cpu.a \
                    -lSceHttp -lSceSsl -lSceNet -lSceNetCtl
# core/recon/sharp submits its graphs through ggml_backend_graph_compute, and
# the glue's submit counter is the only definition of the wrapped name.
TITLE_EXTRA_LDFLAGS := --wrap=ggml_backend_graph_compute
# Every Vulkan entry point the shims define stays local: a title exports
# nothing, and the converter refuses anything that does. The glue archive's
# copies (vkGetInstanceProcAddr and the three shared with the renderer) are
# local through --exclude-libs=ALL, but they are named here as well so the rule
# does not depend on that. The grep also picks up names mentioned only in
# comments; an unused local: entry is harmless. klog_puts is convert_session.cpp's
# title-side bridge for the glue's log line -- also not an export.
TITLE_EXTRA_LOCAL := $(shell grep -ohE '\bvk[A-Z][A-Za-z0-9_]*' vk_entrypoints.cpp \
                        ../ggml/ps5_vulkan.cpp | sort -u) klog_puts

include $(REPO)/platform/ps5/title/build-title.mk

# ---- shaders ---------------------------------------------------------------
# core/render loads <name>.spv next to each other; the cross build compiles
# core/render/shaders into $(BUILD)/shaders (platform/ps5/ggml/CMakeLists.txt),
# and the package carries them under /app0/shaders.
SHADER_NAMES := project.comp finalize.comp splat.vert splat.frag comfort.vert comfort.frag \
                overlay.vert overlay.frag overlay_rgba.frag
SHADERS      := $(addprefix $(TITLE_DIR)/shaders/,$(addsuffix .spv,$(SHADER_NAMES)))

# The capture helper ships inside the package (user decision: the console gets
# nothing extra to install). The title pushes this ELF to the payload loader on
# 127.0.0.1:9021 itself (core/capture/helper_client.cpp, start_helper), reading
# it from /app0/helper/splat-helper.elf. Built first because it is a payload,
# not a title source: platform/ps5/helper has its own Makefile. It is passed
# this package's TITLE_ID, so the payload carries the same data folder as the
# title and two installs get two mailboxes.
HELPER_ELF      := $(REPO)/platform/ps5/helper/splat-helper.elf
PACKAGE_HELPER  := $(TITLE_DIR)/helper/splat-helper.elf
# The packaged copy is stripped (the build keeps its debug info): the whole file
# is streamed to elfldr, and verify-package keeps it under 1 MiB.
STRIP           ?= $(PS5_PAYLOAD_SDK)/bin/prospero-strip

$(HELPER_ELF): force-helper
	$(MAKE) -C $(REPO)/platform/ps5/helper TITLE_ID=$(TITLE_ID)

$(PACKAGE_HELPER): $(HELPER_ELF)
	@mkdir -p $(@D)
	cp $< $@
	$(STRIP) -s $@

$(TITLE_DIR)/shaders/%.spv: $(BUILD)/shaders/%.spv
	@mkdir -p $(@D)
	cp $< $@

# ---- bundled sample scenes (W31) -------------------------------------------
# v0.1 ships a few ready-made scenes so a fresh install has something to show.
# The material is external ($SPLAT_DATA_DIR/assets/samples, never in git):
# scene folders under out/ (the same layout a converted scene has, i.e. a
# folder with scene.psplat, meta.json, original.jpg and thumb.jpg) and the
# photo CREDITS.md. The title seeds them into the gallery on its first start
# (/app0/samples, core/gallery/samples.cpp). A build machine without that
# folder still produces a valid package, just without samples, and a stale
# samples/ left by an earlier build is removed.
SAMPLES_SRC     := $(SPLAT_DATA_DIR)/assets/samples/out
SAMPLES_CREDITS := $(SPLAT_DATA_DIR)/assets/samples/CREDITS.md
PACKAGE_SAMPLES := $(TITLE_DIR)/samples

.PHONY: samples-package
samples-package:
	@if [ -d "$(SAMPLES_SRC)" ]; then \
	    mkdir -p "$(PACKAGE_SAMPLES)"; \
	    found=0; \
	    for scene in "$(SAMPLES_SRC)"/*/; do \
	        [ -d "$$scene" ] || continue; \
	        name=$$(basename "$$scene"); \
	        rm -rf "$(PACKAGE_SAMPLES)/$$name"; \
	        cp -R "$$scene" "$(PACKAGE_SAMPLES)/$$name"; \
	        found=$$((found + 1)); \
	    done; \
	    if [ -f "$(SAMPLES_CREDITS)" ]; then \
	        cp "$(SAMPLES_CREDITS)" "$(PACKAGE_SAMPLES)/CREDITS.md"; \
	    fi; \
	    printf 'viewer: samples %s scene(s), %s bytes\n' "$$found" \
	        "$$(du -sk "$(PACKAGE_SAMPLES)" | awk '{print $$1 * 1024}')"; \
	else \
	    rm -rf "$(PACKAGE_SAMPLES)"; \
	    printf 'viewer: samples: none (%s missing)\n' "$(SAMPLES_SRC)"; \
	fi

$(BUILD)/shaders/%.spv:
	@echo "missing $@" >&2
	@echo "build the cross-built libraries first: platform/ps5/ggml/build.sh" >&2
	@exit 1

# A build dir made before core/render joined the cross build has the archive
# without the renderer in it; the link would fail with a page of undefined
# splat::SplatRenderer symbols, so say so once instead.
CORE_ARCHIVE := $(BUILD)/libsplat_core_ps5.a
MINIZ_ARCHIVE := $(BUILD)/libsplat_miniz.a
GLUE_ARCHIVE := $(BUILD)/libsplat_ps5_glue.a
GGML_ARCHIVES := $(GGML_LIB)/ggml-vulkan/libggml-vulkan.a \
                 $(GGML_LIB)/libggml.a $(GGML_LIB)/libggml-base.a $(GGML_LIB)/libggml-cpu.a
check-core:
	@grep -qa 'splat_renderer.cpp.o' $(CORE_ARCHIVE) 2>/dev/null || { \
	    echo "$(CORE_ARCHIVE) has no core/render objects:" >&2; \
	    echo "re-run platform/ps5/ggml/build.sh (BUILD_DIR=$(BUILD))" >&2; \
	    exit 1; }
	@grep -qa 'sharp_reconstructor.cpp.o' $(CORE_ARCHIVE) 2>/dev/null || { \
	    echo "$(CORE_ARCHIVE) has no core/recon/sharp objects:" >&2; \
	    echo "re-run platform/ps5/ggml/build.sh (BUILD_DIR=$(BUILD))" >&2; \
	    exit 1; }
	@grep -qa 'app.cpp.o' $(CORE_ARCHIVE) 2>/dev/null || { \
	    echo "$(CORE_ARCHIVE) has no core/ui objects (UiApp, screens):" >&2; \
	    echo "re-run platform/ps5/ggml/build.sh (BUILD_DIR=$(BUILD))" >&2; \
	    exit 1; }
	@grep -qa 'model_service.cpp.o' $(CORE_ARCHIVE) 2>/dev/null || { \
	    echo "$(CORE_ARCHIVE) has no core/ui/model_service.cpp (the models adapter):" >&2; \
	    echo "re-run platform/ps5/ggml/build.sh (BUILD_DIR=$(BUILD))" >&2; \
	    exit 1; }

# Relink when the cross-built archives change, not only when the program does.
$(TITLE_PIE): $(CORE_ARCHIVE) $(MINIZ_ARCHIVE) $(GLUE_ARCHIVE) $(GGML_ARCHIVES) | check-core

# The local-symbol list is computed from this Makefile (TITLE_EXTRA_LOCAL), but
# build-title.mk only makes the map depend on itself; rebuild the map when this
# file changes so a new shim name cannot silently stay exported.
$(TITLE_MAP): Makefile

all: viewer-package

viewer-package: title-package $(SHADERS) $(PACKAGE_HELPER) samples-package
	@printf 'viewer: %s shader module(s), %s bytes\n' \
	    "$$(ls $(TITLE_DIR)/shaders/*.spv | wc -l | tr -d ' ')" \
	    "$$(du -sk $(TITLE_DIR)/shaders | awk '{print $$1 * 1024}')"
	@printf 'viewer: helper/splat-helper.elf %s bytes\n' \
	    "$$(stat -f%z $(PACKAGE_HELPER) 2>/dev/null || stat -c%s $(PACKAGE_HELPER))"
	@printf 'title: %s bytes total (samples included)\n' \
	    "$$(du -sk $(TITLE_DIR) | awk '{print $$1 * 1024}')"

$(CORE_ARCHIVE) $(MINIZ_ARCHIVE) $(GLUE_ARCHIVE) $(GGML_ARCHIVES) \
$(RADV_LIB) $(RADV_BUILTINS) \
$(RADV_SDK)/target/lib/libps5platform.a:
	@echo "missing $@" >&2
	@echo "build the libraries first: platform/ps5/ggml/build.sh" >&2
	@exit 1

clean: title-clean

.PHONY: all clean viewer-package check-core force-helper
