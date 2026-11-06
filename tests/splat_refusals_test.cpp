// The four malformed splat fixtures the console's import case pushes: each one
// names a real container and then fails in its own reader, so the refusal is
// the reader's answer and not a generic "bad file". The console case
// (import_refusals in platform/ps5/scripts/console-run.sh) asserts the same
// four lines from the panel; this test pins the messages and proves the
// desktop reader refuses each file for the right reason.
//
// The fixtures live outside the repo (AGENTS.md): $SPLAT_DATA_DIR/runs/
// v0.2-library/console/splats-bad. The case skips itself when they are absent.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "data_dir.h"
#include "splat_import/splat_import.h"
#include "vfs/io.h"

namespace {

std::vector<uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("the four malformed splat fixtures are refused, each with its own reason") {
    const std::filesystem::path folder =
        splat_test::data_path("runs/v0.2-library/console/splats-bad");
    if (!std::filesystem::exists(folder)) {
        MESSAGE("no " << folder.string() << ": the malformed-fixture check is skipped");
        return;
    }

    struct Case {
        const char* name;
        const char* expected;  // a fragment the reader's message must carry
    };
    // The console case greps these exact fragments on the job's failed line.
    const Case cases[] = {
        {"bad.splat", "unsupported splat file"},  // 100 zeros: no known header
        {"bad.spz", "gzip stream ended before the declared splat data"},
        {"bad.ply", ".ply vertex data is truncated"},
        {"bad.compressed.ply", "compressed PLY has too few chunks"},
    };
    for (const Case& item : cases) {
        const std::filesystem::path path = folder / item.name;
        REQUIRE_MESSAGE(std::filesystem::exists(path), path.string());
        const std::vector<uint8_t> bytes = read_bytes(path);
        REQUIRE_FALSE(bytes.empty());
        splat::ImportLimits limits;
        splat::SplatScene scene;
        const splat::Result<splat::SplatScene> imported =
            splat::import_splat(splat::Path(path.string()), bytes.data(), bytes.size(), limits,
                                nullptr);
        CHECK_MESSAGE(!imported.ok(), item.name << " was accepted");
        if (!imported.ok()) {
            MESSAGE(item.name << ": " << imported.message());
            if (item.expected[0] != '\0') {
                CHECK(imported.message().find(item.expected) != std::string::npos);
            }
        }
    }
}
