# Builds libhmd2.a with the ps5-payload SDK, which lives outside the repo:
#   export PS5_PAYLOAD_SDK="$SPLAT_DATA_DIR/ref/toolchains/ps5-payload-sdk"
#   export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config
#   make
# hmd2_exports.h is generated; regenerate it after editing
# tools/export-tables/tables.json with
#   python3 tools/export-tables/gen_export_table.py --emit-c -o platform/ps5/hmd2/hmd2_exports.h
# SPDX-License-Identifier: GPL-3.0-or-later

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

LIB  := libhmd2.a
OBJS := hmd2.o

CXXFLAGS := -std=c++17 -Wall -Werror -O2 -g -fno-exceptions -fno-rtti

all: $(LIB)

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

hmd2.o: hmd2.cpp hmd2.h hmd2_exports.h
	$(CXX) $(CXXFLAGS) -c -o $@ hmd2.cpp

clean:
	rm -f $(OBJS) $(LIB)

.PHONY: all clean
