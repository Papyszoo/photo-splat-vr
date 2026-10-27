#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>

#include "data_dir.h"
#include "scene/ply.h"
#include "scene/psplat.h"
#include "test_util.h"

// Golden data lives outside the repo and is not always present (see
// docs/research/sharp-reference.md). The test is a no-op without it.
namespace {
const std::filesystem::path kGoldenPly =
    splat_test::data_path("golden/photo01_living_room/reference.ply");
constexpr size_t kPhoto01Splats = 1179648;
}  // namespace

TEST_CASE("golden photo01 reference.ply converts to .psplat") {
    if (!std::filesystem::exists(kGoldenPly)) {
        MESSAGE("skipping: " << kGoldenPly << " is not present");
        return;
    }

    splat::SplatScene scene;
    const auto status = splat::import_ply(kGoldenPly, scene);
    REQUIRE_MESSAGE(status.ok(), status.message());
    REQUIRE(scene.count() == kPhoto01Splats);

    splat_test::TempDir temp("golden-photo01");
    const auto path = temp.path() / "photo01.psplat";
    const auto written = splat::write_psplat(path, scene, 1074.2907f, 1280, 873);
    REQUIRE_MESSAGE(written.ok(), written.message());

    const auto size = static_cast<size_t>(std::filesystem::file_size(path));
    CHECK(size == splat::psplat_file_size(kPhoto01Splats));
    CHECK(size == 33030224);  // 80-byte header + 1,179,648 * 28 bytes
    CHECK(size > 33000000);
    CHECK(size < 34000000);
    MESSAGE("photo01 reference.ply -> .psplat: " << size << " bytes");

    splat::PsplatInfo info;
    const auto loaded = splat::read_psplat(path, &info);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(info.count == kPhoto01Splats);
    CHECK(info.image_width == 1280);
    CHECK(info.image_height == 873);
    REQUIRE(loaded.value().count() == scene.count());

    // The first Gaussian and a sample near the end survive the fp16 conversion.
    for (size_t i : {size_t{0}, kPhoto01Splats - 1}) {
        for (size_t c = 0; c < 3; ++c) {
            CHECK(splat_test::close_enough(loaded.value().position[i * 3 + c],
                                           scene.position[i * 3 + c]));
            CHECK(splat_test::close_enough(loaded.value().log_scale[i * 3 + c],
                                           scene.log_scale[i * 3 + c]));
            CHECK(splat_test::close_enough(loaded.value().sh_dc[i * 3 + c],
                                           scene.sh_dc[i * 3 + c]));
        }
        for (size_t c = 0; c < 4; ++c) {
            CHECK(splat_test::close_enough(loaded.value().rotation[i * 4 + c],
                                           scene.rotation[i * 4 + c]));
        }
    }
}
