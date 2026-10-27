# Spike A bench payloads: op_audit, da2_bench, sharp_bench.
#
# `bench-title` packages them (plus the title-only convert_inbox and http
# modes) as the one homebrew title (the dev identity by default; RELEASE=1 /
# TITLE_ID=… selects the release one, platform/ps5/title/identities.json).
#
# They link the libraries cross-compiled by platform/ps5/ggml/build.sh and the
# same RADV archive vk_hello links, with the same whole-archive link recipe
# (no Vulkan loader), shared through platform/ps5/radv-link.mk. Read that
# directory's README and docs/research/spike-b.md for why every input is here.
#
#   platform/ps5/ggml/build.sh              # once: ggml + core, PS5 target
#   export PS5_PAYLOAD_SDK="$SPLAT_DATA_DIR/ref/radv-ps5/sdk"
#   export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config
#   make -C platform/ps5/bench              # the three ELFs
#   platform/ps5/bench/verify-elfs.sh       # static checks
# SPDX-License-Identifier: GPL-3.0-or-later

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

CXXFLAGS := -std=c++17 -Wall -Wextra -Werror -O2 -g

REPO     := $(abspath $(CURDIR)/../../..)
SPLAT_DATA_DIR ?= $(abspath $(REPO)/..)
RADV_PS5 ?= $(SPLAT_DATA_DIR)/ref/radv-ps5
RADV_SDK ?= $(RADV_PS5)/sdk
RADV_LIB := $(RADV_PS5)/lib/libvulkan_radeon.ps5.a
RADV_BUILTINS := $(RADV_PS5)/lib/libclang_rt.builtins-x86_64.a
BUILD    ?= $(REPO)/build-ps5-ggml
GGML_LIB := $(BUILD)/ggml/src
GLUE     := $(REPO)/platform/ps5/ggml

PAYLOADS := op_audit.elf da2_bench.elf sharp_bench.elf
ALL      := $(PAYLOADS)

INCLUDES := -I$(REPO)/core -I$(REPO)/third_party -I$(REPO)/third_party/json \
            -I$(GLUE) -I$(REPO)/third_party/ggml/include

# Statically linked project + ggml, scanned as one group: the reconstruction
# code, the two ggml backends and the core reference each reference the other.
SPLAT_LIBS := $(BUILD)/libsplat_core_ps5.a $(BUILD)/libsplat_ps5_glue.a \
              $(BUILD)/libsplat_miniz.a \
              $(GGML_LIB)/ggml-vulkan/libggml-vulkan.a \
              $(GGML_LIB)/libggml.a $(GGML_LIB)/libggml-base.a $(GGML_LIB)/libggml-cpu.a

# RADV's title recipe (PS5_Vulkan tools/radv-link.sh) with the payload SDK's
# crt1.o and system imports instead of the title converter, exactly as
# platform/ps5/vk_hello uses it. The lists are shared through
# platform/ps5/radv-link.mk.
include $(REPO)/platform/ps5/radv-link.mk

# The submit counter in bench_env.cpp observes every graph submission without a
# core/ API change.
LDFLAGS := $(RADV_LINK_BASE) \
           -Wl,--wrap=ggml_backend_graph_compute \
           -Wl,--start-group \
             $(SPLAT_LIBS) $(RADV_SDK_LIBS) \
           -Wl,--end-group \
           $(RADV_WRAPS) $(RADV_DEFSYMS) $(RADV_SYSTEM_LIBS)

# --- the benches as one homebrew title ----------------------------------------
# all: the payload ELFs. bench-title: the title package (see below).
all: $(ALL)

# A payload has no direct-memory pool, so RADV gives it no heaps
# (docs/research/spike-b-memory.md sections 1-2) and these benches cannot
# measure GPU memory in a payload. The title target in ../title/ is the route:
# one package whose program reads BENCH=op_audit|da2_bench|sharp_bench|convert|http
# from /data/ps5-splat-vr/bench/bench.env and calls that entry point. One title
# with a selector instead of one package per program, and the same title as
# every other test (docs/PLAN.md, "One app on the console"):
# docs/research/spike-a.md 2.4. `convert` has no payload ELF: it is the inbox
# batch (convert_inbox.cpp) that turns photos into scenes on the console.
#
#   make -C platform/ps5/bench bench-title      # build/ps5/title/<dev id>/
#   platform/ps5/bench/run-bench.sh --title op_audit 1200
#
# The payload ELFs above are unchanged and still build with plain `make`.
# BENCH=http hashes the probe file with core/model/sha256.cpp. core/model is
# not in build-ps5-ggml's splat_core_ps5.a (it leaves out the converter and the
# zip/http code), so the one file the probe needs is compiled here.
TITLE_STEM       := bench_title
TITLE_SOURCES    := bench_title.cpp op_audit.cpp da2_bench.cpp sharp_bench.cpp convert_inbox.cpp \
                    http_probe.cpp ../net/ps5_http_fetcher.cpp $(REPO)/core/model/sha256.cpp
# platform/ps5/net is a new include root (http_probe.cpp includes it by repo
# path); only the title build needs it.
TITLE_CPPFLAGS   := -DPS5_TITLE $(INCLUDES) -I$(REPO)
TITLE_CXXFLAGS   := $(CXXFLAGS) -ffunction-sections -fdata-sections
# The project/ggml archives go inside the SDK's --start-group (they and libc++
# reference each other), and the submit counter needs the same --wrap as the
# payload link.
# BENCH=http is the only mode that calls sceHttp/sceSsl/sceNet, but the title is
# one ELF, so the four stubs are linked here (before build-title.mk's trailing
# --as-needed, which is what makes the imports NEEDED). libSceNetCtl is what
# sceNetInit's stack uses for link state on this console; no sceSysmodule load
# is needed (ProsperoMgr's bridge calls none).
TITLE_EXTRA_LIBS    := $(SPLAT_LIBS) -lSceHttp -lSceSsl -lSceNet -lSceNetCtl
TITLE_EXTRA_LDFLAGS := --wrap=ggml_backend_graph_compute
TITLE_HEADERS       := bench_title.h bench_dispatch.h bench_common.h \
                       $(GLUE)/bench_env.h $(GLUE)/ggml_logging.h \
                       $(REPO)/platform/ps5/net/ps5_http_fetcher.h \
                       $(REPO)/platform/ps5/net/sce_http.h \
                       $(REPO)/platform/ps5/net/http_range.h

include $(REPO)/platform/ps5/title/build-title.mk

bench-title: title-package

$(PAYLOADS): %.elf: %.cpp bench_common.h bench_title.h $(GLUE)/bench_env.h $(GLUE)/ggml_logging.h $(SPLAT_LIBS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< $(LDFLAGS)

# Debug sections are 90 MB of a payload and a payload cannot symbolicate a
# backtrace on the console anyway; these copies upload ~2x faster and are what
# the runbook deploys. The full ELFs stay for local llvm-symbolizer use.
strip: $(ALL)
	@for elf in $(ALL); do \
	    cp "$$elf" "$${elf%.elf}.stripped.elf"; \
	    $(STRIP) --strip-debug "$${elf%.elf}.stripped.elf"; \
	    printf '  %-26s %s\n' "$${elf%.elf}.stripped.elf" "$$(wc -c < "$${elf%.elf}.stripped.elf") bytes"; \
	done

$(SPLAT_LIBS) $(RADV_LIB) $(RADV_BUILTINS) $(RADV_SDK)/target/lib/libps5platform.a \
$(RADV_PS5)/lib/libSceAgc.so $(RADV_PS5)/lib/libSceAgcDriver.so:
	@echo "missing $@" >&2
	@echo "build the libraries first: platform/ps5/ggml/build.sh" >&2
	@echo "(and, for RADV: platform/ps5/radv/build-in-docker.sh)" >&2
	@exit 1

clean: title-clean
	rm -f $(ALL) $(ALL:.elf=.stripped.elf)

.PHONY: all clean strip bench-title
