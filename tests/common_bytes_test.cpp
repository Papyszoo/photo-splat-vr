// The one byte formatter the CLIs print sizes with (core/common/bytes.h). It
// used to exist as four identical copies (tools/convert, tools/reconstruct,
// tools/model_cli, core/recon/ggml/backend.cpp).

#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "common/bytes.h"

TEST_CASE("common human_bytes") {
    CHECK(splat::human_bytes(0) == "0.00 B");
    CHECK(splat::human_bytes(1) == "1.00 B");
    CHECK(splat::human_bytes(1023) == "1023.00 B");
    CHECK(splat::human_bytes(1024) == "1.00 KiB");
    CHECK(splat::human_bytes(1536) == "1.50 KiB");
    CHECK(splat::human_bytes(1024ull * 1024) == "1.00 MiB");
    // 1.31 GiB, the fp16 SHARP weights.
    CHECK(splat::human_bytes(1406601784ull) == "1.31 GiB");
    CHECK(splat::human_bytes(1ull << 40) == "1.00 TiB");
    // The largest value must not run past the unit table.
    CHECK(splat::human_bytes(~0ull).find("TiB") != std::string::npos);
}
