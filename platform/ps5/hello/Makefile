# Builds hello.elf with the ps5-payload SDK, which lives outside the repo:
#   export PS5_PAYLOAD_SDK="$SPLAT_DATA_DIR/ref/toolchains/ps5-payload-sdk"
#   export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config
#   make
# ($SPLAT_DATA_DIR is the data directory, by default this repo's parent. On
# macOS LLVM_CONFIG is Homebrew's llvm@21; `brew install llvm@21 lld` first.)
# SPDX-License-Identifier: GPL-3.0-or-later

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := hello.elf

CXXFLAGS := -std=c++17 -Wall -Werror -O2 -g

all: $(ELF)

$(ELF): main.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -f $(ELF)

.PHONY: all clean
