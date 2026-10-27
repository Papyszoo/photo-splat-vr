# Builds splat-helper.elf, the resident capture-mailbox payload, with the
# ps5-payload SDK outside the repo:
#   export PS5_PAYLOAD_SDK="$SPLAT_DATA_DIR/ref/radv-ps5/sdk"
#   export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config
#   make -C platform/ps5/helper                 # dev identity (default)
#   make -C platform/ps5/helper RELEASE=1       # release identity
#
# The protocol lives in core/capture/helper_protocol.* and is compiled here by
# path, so the payload and the desktop/title build answer with the same code.
# Deploy with platform/ps5/scripts/deploy.sh (ProsperoMgr streams the ELF to
# elfldr on the console's loopback); the app pushes it itself on 127.0.0.1:9021.
#
# The payload carries the data folder of the identity it was built as
# (platform/ps5/title/identities.json), exactly like the title that pushes it:
# its mailbox, its pid lock and the inbox it copies into all live under that
# folder, so a dev helper and a release helper running on the same console never
# share a lock, a request file or a copy target.
# SPDX-License-Identifier: GPL-3.0-or-later

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF  := splat-helper.elf
REPO := $(abspath $(CURDIR)/../../..)

# --- the identity ------------------------------------------------------------
# Same table and the same selector as build-title.mk (one source, no second
# copy): RELEASE=1 builds the release identity, TITLE_ID=… selects one by id,
# otherwise the dev identity is used.
IDENTITY_PY := $(REPO)/platform/ps5/title/identity.py
TITLE_RELEASE ?= $(if $(strip $(RELEASE)),$(strip $(RELEASE)),0)
IDENTITY_SELECTOR := $(if $(strip $(TITLE_ID)),--title-id $(strip $(TITLE_ID)),$(if $(filter-out 0 no false,$(strip $(TITLE_RELEASE))),--name release,--name dev))
IDENTITY_FIELDS := $(shell python3 $(IDENTITY_PY) $(IDENTITY_SELECTOR))
TITLE_ID     := $(word 1,$(IDENTITY_FIELDS))
DATA_ROOT    := $(word 4,$(IDENTITY_FIELDS))
ifeq ($(strip $(DATA_ROOT)),)
$(error no data dir: identity.py printed nothing for '$(IDENTITY_SELECTOR)' (see its message above; platform/ps5/title/identities.json holds the table))
endif

# core/common/path.cpp provides normal_absolute(), the lexical normal form the
# protocol compares capture paths with; it is compiled in by path because the
# payload links no library. copy_captures() itself resolves each source with the
# helper's own realpath/lstat.
SOURCES := main.cpp $(REPO)/core/capture/helper_protocol.cpp $(REPO)/core/common/path.cpp
HEADERS := $(REPO)/core/capture/helper_protocol.h $(REPO)/core/common/path.h

# Size matters twice: the whole ELF is streamed to elfldr over the loopback
# socket when the title starts it, and the package check keeps the shipped copy
# under 1 MiB (platform/ps5/viewer/verify-package.sh). -Os plus section GC and a
# strip of the packaged copy (the viewer Makefile) leaves ~0.7 MiB.
CXXFLAGS := -std=c++17 -Wall -Werror -Os -g -ffunction-sections -fdata-sections \
            -DSPLAT_TITLE_ID=\"$(TITLE_ID)\" -DSPLAT_DATA_ROOT=\"$(DATA_ROOT)\"
LDFLAGS  := -Wl,--gc-sections

# nlohmann json is vendored (third_party/json); the protocol uses it for both
# directions of the mailbox.
INCLUDES := -I$(REPO)/core -I$(REPO)/third_party/json

# One file, a second to compile: always rebuild it. The data root is a compile-
# time string, so an ELF left from the other identity would write the other
# install's mailbox — a stale binary is worse than the rebuild.
.PHONY: all clean force

all: $(ELF)

$(ELF): $(SOURCES) $(HEADERS) force
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ $(SOURCES)

force:

clean:
	rm -f $(ELF)
