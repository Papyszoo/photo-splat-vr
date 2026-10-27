#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "scene/ply.h"
#include "test_util.h"

using splat::SplatScene;

TEST_CASE(".ply export and import round trip is lossless in fp32") {
    splat_test::TempDir temp("ply-roundtrip");
    const auto path = temp.path() / "scene.ply";

    const SplatScene original = splat_test::make_test_scene(41);
    const auto status = splat::export_ply(path, original);
    REQUIRE_MESSAGE(status.ok(), status.message());

    SplatScene loaded;
    const auto imported = splat::import_ply(path, loaded);
    REQUIRE_MESSAGE(imported.ok(), imported.message());

    REQUIRE(loaded.count() == original.count());
    for (size_t i = 0; i < original.count() * 3; ++i) {
        CHECK(loaded.position[i] == original.position[i]);
        CHECK(loaded.log_scale[i] == original.log_scale[i]);
        CHECK(loaded.sh_dc[i] == original.sh_dc[i]);
    }
    for (size_t i = 0; i < original.count() * 4; ++i) CHECK(loaded.rotation[i] == original.rotation[i]);
    for (size_t i = 0; i < original.count(); ++i) CHECK(loaded.opacity_logit[i] == original.opacity_logit[i]);
}

TEST_CASE(".ply import ignores f_rest_* and extra elements") {
    splat_test::TempDir temp("ply-frest");
    const auto path = temp.path() / "frest.ply";

    const std::string header =
        "ply\n"
        "format binary_little_endian 1.0\n"
        "element vertex 1\n"
        "property float x\n"
        "property float y\n"
        "property float z\n"
        "property float f_dc_0\n"
        "property float f_dc_1\n"
        "property float f_dc_2\n"
        "property float opacity\n"
        "property float scale_0\n"
        "property float scale_1\n"
        "property float scale_2\n"
        "property float rot_0\n"
        "property float rot_1\n"
        "property float rot_2\n"
        "property float rot_3\n"
        "property float f_rest_0\n"
        "property float f_rest_1\n"
        "element intrinsic 1\n"
        "property float intrinsic\n"
        "end_header\n";

    const float values[16] = {1.0f,  2.0f,   3.0f,   0.25f,  -0.5f, 0.75f, -1.5f,
                              -4.0f, -4.5f,  -5.0f,  0.9f,   0.1f,  -0.2f, 0.3f,
                              9.0f,  -9.0f};  // last two are f_rest, must be ignored
    std::vector<uint8_t> bytes(header.begin(), header.end());
    const size_t offset = bytes.size();
    bytes.resize(offset + sizeof(values));
    std::memcpy(bytes.data() + offset, values, sizeof(values));

    std::ofstream out(path, std::ios::binary);
    REQUIRE(static_cast<bool>(out));
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();

    SplatScene scene;
    const auto status = splat::import_ply(path, scene);
    REQUIRE_MESSAGE(status.ok(), status.message());
    REQUIRE(scene.count() == 1);
    CHECK(scene.position[0] == 1.0f);
    CHECK(scene.position[1] == 2.0f);
    CHECK(scene.position[2] == 3.0f);
    CHECK(scene.sh_dc[0] == 0.25f);
    CHECK(scene.sh_dc[1] == -0.5f);
    CHECK(scene.sh_dc[2] == 0.75f);
    CHECK(scene.opacity_logit[0] == -1.5f);
    CHECK(scene.log_scale[0] == -4.0f);
    CHECK(scene.log_scale[1] == -4.5f);
    CHECK(scene.log_scale[2] == -5.0f);
    CHECK(scene.rotation[0] == 0.9f);
    CHECK(scene.rotation[3] == 0.3f);
}

TEST_CASE(".ply import rejects ASCII and missing attributes") {
    splat_test::TempDir temp("ply-reject");

    const auto ascii = temp.path() / "ascii.ply";
    {
        std::ofstream out(ascii);
        out << "ply\nformat ascii 1.0\nelement vertex 0\nend_header\n";
    }
    SplatScene scene;
    CHECK_FALSE(splat::import_ply(ascii, scene).ok());

    const auto incomplete = temp.path() / "incomplete.ply";
    {
        std::ofstream out(incomplete);
        out << "ply\nformat binary_little_endian 1.0\nelement vertex 0\n"
               "property float x\nproperty float y\nend_header\n";
    }
    CHECK_FALSE(splat::import_ply(incomplete, scene).ok());
}

TEST_CASE("read_ply_intrinsics reads SHARP's intrinsic and image_size elements") {
    splat_test::TempDir temp("ply-intrinsics");
    const auto path = temp.path() / "sharp.ply";

    // 1 vertex, then the extra elements `sharp predict` appends: intrinsic is
    // nine floats of a row-major 3x3 (fx 0 cx / 0 fy cy / 0 0 1) and image_size
    // is two uints.
    const std::string header =
        "ply\n"
        "format binary_little_endian 1.0\n"
        "element vertex 1\n"
        "property float x\n"
        "property float y\n"
        "property float z\n"
        "property float f_dc_0\n"
        "property float f_dc_1\n"
        "property float f_dc_2\n"
        "property float opacity\n"
        "property float scale_0\n"
        "property float scale_1\n"
        "property float scale_2\n"
        "property float rot_0\n"
        "property float rot_1\n"
        "property float rot_2\n"
        "property float rot_3\n"
        "element extrinsic 16\n"
        "property float extrinsic\n"
        "element intrinsic 9\n"
        "property float intrinsic\n"
        "element image_size 2\n"
        "property uint image_size\n"
        "end_header\n";

    std::vector<uint8_t> bytes(header.begin(), header.end());
    const float vertex[14] = {0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                              0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    const float intrinsic[9] = {1074.29f, 0.0f, 640.0f, 0.0f, 1041.5f, 436.5f, 0.0f, 0.0f, 1.0f};
    const float extrinsic[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    const uint32_t image_size[2] = {1280u, 873u};
    auto append = [&bytes](const void* data, size_t size) {
        const size_t offset = bytes.size();
        bytes.resize(offset + size);
        std::memcpy(bytes.data() + offset, data, size);
    };
    append(vertex, sizeof(vertex));
    append(extrinsic, sizeof(extrinsic));
    append(intrinsic, sizeof(intrinsic));
    append(image_size, sizeof(image_size));

    std::ofstream out(path, std::ios::binary);
    REQUIRE(static_cast<bool>(out));
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();

    const auto intrinsics = splat::read_ply_intrinsics(path);
    REQUIRE_MESSAGE(intrinsics.ok(), intrinsics.message());
    CHECK(intrinsics.value().fx == doctest::Approx(1074.29f));
    CHECK(intrinsics.value().fy == doctest::Approx(1041.5f));
    CHECK(intrinsics.value().cx == doctest::Approx(640.0f));
    CHECK(intrinsics.value().cy == doctest::Approx(436.5f));
    CHECK(intrinsics.value().width == 1280u);
    CHECK(intrinsics.value().height == 873u);

    // export_ply writes no extra elements: intrinsics come back empty rather
    // than as garbage, so the viewer can fall back to a default field of view.
    const auto plain = temp.path() / "plain.ply";
    REQUIRE_MESSAGE(splat::export_ply(plain, splat_test::make_test_scene(3)).ok(), "export");
    const auto none = splat::read_ply_intrinsics(plain);
    REQUIRE_MESSAGE(none.ok(), none.message());
    CHECK(none.value().fx == 0.0f);
    CHECK(none.value().width == 0u);
}
