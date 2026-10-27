#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "scene/psplat.h"
#include "test_util.h"

using splat::PsplatInfo;
using splat::SplatScene;

TEST_CASE(".psplat round trip keeps attributes within fp16 tolerance") {
    splat_test::TempDir temp("psplat-roundtrip");
    const auto path = temp.path() / "scene.psplat";

    const SplatScene original = splat_test::make_test_scene(97);
    const float focal_px = 1018.29f;
    const auto status = splat::write_psplat(path, original, focal_px, 1280, 720);
    REQUIRE_MESSAGE(status.ok(), status.message());

    CHECK(std::filesystem::file_size(path) == splat::psplat_file_size(original.count()));

    PsplatInfo info;
    const auto loaded = splat::read_psplat(path, &info);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    const SplatScene& scene = loaded.value();

    REQUIRE(scene.count() == original.count());
    CHECK(info.version == splat::kPsplatVersion);
    CHECK(info.count == original.count());
    CHECK(info.values_per_splat == SplatScene::kValuesPerSplat);
    CHECK(info.layout == SplatScene::kLayout);
    CHECK(info.image_width == 1280);
    CHECK(info.image_height == 720);
    CHECK(info.focal_px == doctest::Approx(focal_px).epsilon(1e-4));

    const auto bounds = original.bounds();
    REQUIRE(bounds.valid);
    for (int i = 0; i < 3; ++i) {
        CHECK(info.bounds_min[i] == doctest::Approx(bounds.min[i]).epsilon(1e-5));
        CHECK(info.bounds_max[i] == doctest::Approx(bounds.max[i]).epsilon(1e-5));
    }

    for (size_t i = 0; i < original.count(); ++i) {
        for (size_t c = 0; c < 3; ++c) {
            CHECK(splat_test::close_enough(scene.position[i * 3 + c], original.position[i * 3 + c]));
            CHECK(splat_test::close_enough(scene.log_scale[i * 3 + c], original.log_scale[i * 3 + c]));
            CHECK(splat_test::close_enough(scene.sh_dc[i * 3 + c], original.sh_dc[i * 3 + c]));
        }
        for (size_t c = 0; c < 4; ++c) {
            CHECK(splat_test::close_enough(scene.rotation[i * 4 + c], original.rotation[i * 4 + c]));
        }
        CHECK(splat_test::close_enough(scene.opacity_logit[i], original.opacity_logit[i]));
    }
}

TEST_CASE(".psplat round trips an empty scene") {
    splat_test::TempDir temp("psplat-empty");
    const auto path = temp.path() / "empty.psplat";

    SplatScene empty;
    REQUIRE(splat::write_psplat(path, empty).ok());
    CHECK(std::filesystem::file_size(path) == splat::kPsplatHeaderSize);

    const auto loaded = splat::read_psplat(path);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(loaded.value().count() == 0);
}

TEST_CASE(".psplat rejects bad magic, truncation and unsupported versions") {
    const SplatScene scene = splat_test::make_test_scene(4);

    std::vector<uint8_t> bytes(splat::psplat_file_size(scene.count()), 0);
    CHECK_FALSE(splat::read_psplat(bytes.data(), bytes.size()).ok());

    splat_test::TempDir temp("psplat-reject");
    const auto path = temp.path() / "scene.psplat";
    REQUIRE(splat::write_psplat(path, scene).ok());

    std::ifstream file(path, std::ios::binary);
    const std::vector<uint8_t> valid((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
    REQUIRE(valid.size() == splat::psplat_file_size(scene.count()));

    CHECK_FALSE(splat::read_psplat(valid.data(), valid.size() - 2).ok());

    std::vector<uint8_t> wrong_version = valid;
    wrong_version[8] = 99;  // version is a little-endian uint32 at offset 8
    CHECK_FALSE(splat::read_psplat(wrong_version.data(), wrong_version.size()).ok());
}
