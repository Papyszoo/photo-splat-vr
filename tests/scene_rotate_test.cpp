// Turning a scene about its viewing axis as a file action: the splat data,
// original.jpg, thumb.jpg and the intrinsics all turn together, the scale and
// unknown meta.json keys survive, and the replace order keeps the scene
// openable when something fails. The .psplat store is fp16, so repeated file
// turns are lossy; the drift is measured here and reported.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/image.h"
#include "common/path.h"
#include "jobs/image_io.h"
#include "render/project_math.h"
#include "scene/psplat.h"
#include "scene/scene_folder.h"
#include "scene/scene_rotate.h"
#include "scene/scene_view.h"
#include "test_util.h"
#include "vfs/io.h"

namespace {

using splat::Path;
namespace vfs = splat::vfs;

std::vector<uint8_t> jpeg(int width, int height, uint8_t seed) {
    splat::Image image;
    image.resize(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t* pixel = image.pixel(x, y);
            pixel[0] = static_cast<uint8_t>(seed + x * 3);
            pixel[1] = static_cast<uint8_t>(seed + y * 5);
            pixel[2] = static_cast<uint8_t>(seed + (x + y) * 2);
        }
    }
    const auto encoded = splat::encode_jpeg(image, 92);
    REQUIRE_MESSAGE(encoded.ok(), encoded.message());
    return encoded.value();
}

splat::SceneMeta meta_for(const splat::SplatScene& scene) {
    splat::SceneMeta meta;
    meta.backend = "sharp";
    meta.seconds_taken = 1.5;
    meta.source = "inbox";
    meta.intrinsics.width = 64;
    meta.intrinsics.height = 32;
    meta.intrinsics.fx = 80.0f;
    meta.intrinsics.fy = 70.0f;
    meta.intrinsics.cx = 20.0f;
    meta.intrinsics.cy = 10.0f;
    meta.splat_count = static_cast<uint32_t>(scene.count());
    return meta;
}

Path make_scene(const Path& folder, bool imported = false) {
    const splat::SplatScene scene = splat_test::make_test_scene(8);
    splat::SceneMeta meta = meta_for(scene);
    if (imported) {
        meta.origin_format = "splat";
        meta.origin_count = 8;
    }
    const std::vector<uint8_t> original = jpeg(64, 32, 10);
    const std::vector<uint8_t> thumbnail = jpeg(64, 32, 40);
    const splat::Status status =
        splat::write_scene_folder(folder, scene, meta, original, thumbnail);
    REQUIRE_MESSAGE(status.ok(), status.message());
    return folder;
}

std::vector<uint8_t> read_bytes(const Path& path) {
    const auto bytes = vfs::read_file(path);
    REQUIRE_MESSAGE(bytes.ok(), bytes.message());
    return bytes.value();
}

void put_bytes(const Path& path, const std::vector<uint8_t>& bytes) {
    REQUIRE_MESSAGE(vfs::write_file(path, bytes).ok(), "write " << path.string());
}

nlohmann::json read_json(const Path& path) {
    const std::vector<uint8_t> bytes = read_bytes(path);
    return nlohmann::json::parse(bytes.begin(), bytes.end());
}

void write_json(const Path& path, const nlohmann::json& json) {
    const std::string text = json.dump(2) + "\n";
    put_bytes(path, std::vector<uint8_t>(text.begin(), text.end()));
}

double max_position_drift(const splat::SplatScene& a, const splat::SplatScene& b) {
    REQUIRE(a.position.size() == b.position.size());
    double worst = 0.0;
    for (size_t i = 0; i < a.position.size(); ++i) {
        worst = std::max(worst, std::fabs(static_cast<double>(a.position[i]) - b.position[i]));
    }
    return worst;
}

// The largest angular difference between two per-splat orientations, in
// radians, treating q and -q as the same rotation.
double max_orientation_drift(const splat::SplatScene& a, const splat::SplatScene& b) {
    REQUIRE(a.rotation.size() == b.rotation.size());
    double worst = 0.0;
    for (size_t i = 0; i + 3 < a.rotation.size(); i += 4) {
        double length_a = 0.0;
        double length_b = 0.0;
        double dot = 0.0;
        for (int c = 0; c < 4; ++c) {
            const double va = a.rotation[i + c];
            const double vb = b.rotation[i + c];
            length_a += va * va;
            length_b += vb * vb;
            dot += va * vb;
        }
        if (length_a <= 0.0 || length_b <= 0.0) continue;
        const double clamped =
            std::min(1.0, std::fabs(dot / (std::sqrt(length_a) * std::sqrt(length_b))));
        worst = std::max(worst, 2.0 * std::acos(clamped));
    }
    return worst;
}

// decode_image(Path) uses std::fopen (a local path); a path on a fake network
// location must be read through the VFS first, as the app does.
splat::Result<splat::Image> decode_vfs(const Path& path) {
    const auto bytes = vfs::read_file(path);
    if (!bytes.ok()) return splat::Result<splat::Image>::failure(bytes.message());
    return splat::decode_image(bytes.value().data(), bytes.value().size());
}

class FakeShare : public vfs::IFileSystem {
public:
    explicit FakeShare(std::string id) : id_(std::move(id)) {}
    std::string id() const override { return id_; }
    std::string describe() const override { return id_ + ":/"; }
    bool writable() const override { return true; }

    splat::Result<std::vector<uint8_t>> read(const std::string& relative, size_t) override {
        const auto found = files.find(relative);
        if (found == files.end()) {
            return splat::Result<std::vector<uint8_t>>::failure("fake: no such file: " + relative);
        }
        return splat::Result<std::vector<uint8_t>>::success(found->second);
    }
    splat::Status write(const std::string& relative, const uint8_t* data, size_t size) override {
        files[relative] = std::vector<uint8_t>(data, data + size);
        return splat::Status::success();
    }
    splat::Status list(const std::string&, std::vector<vfs::DirEntry>* entries) override {
        for (const auto& file : files) {
            vfs::DirEntry entry;
            const std::size_t slash = file.first.find_last_of('/');
            entry.name = slash == std::string::npos ? file.first : file.first.substr(slash + 1);
            entry.size = file.second.size();
            entries->push_back(entry);
        }
        return splat::Status::success();
    }
    splat::Status stat(const std::string& relative, vfs::FileStat* out) override {
        *out = vfs::FileStat{};
        const auto found = files.find(relative);
        if (found == files.end()) return splat::Status::success();
        out->exists = true;
        out->regular = true;
        out->size = found->second.size();
        return splat::Status::success();
    }
    splat::Status make_dirs(const std::string&) override { return splat::Status::success(); }
    splat::Status remove_file(const std::string& relative) override {
        files.erase(relative);
        return splat::Status::success();
    }
    splat::Status remove_tree(const std::string&) override { return splat::Status::success(); }
    splat::Status rename(const std::string& from, const std::string& to) override {
        if (!can_rename) return splat::Status::error("the share refuses a rename");
        const auto found = files.find(from);
        if (found == files.end()) return splat::Status::error("fake: no such file: " + from);
        // A server that honours SMB2 ReplaceIfExists=0: the name must be free.
        if (!replaces && files.find(to) != files.end()) {
            return splat::Status::error("fake: the target exists: " + to);
        }
        files[to] = std::move(found->second);
        files.erase(from);
        return splat::Status::success();
    }
    // libsmb2's rename cannot replace an existing name; the fake can model it.
    bool rename_replaces() const override { return replaces; }

    std::map<std::string, std::vector<uint8_t>> files;
    bool can_rename = true;
    bool replaces = true;

private:
    std::string id_;
};

struct Mounted {
    explicit Mounted(std::shared_ptr<FakeShare> share) : share_(std::move(share)) {
        vfs::file_systems().add(share_);
    }
    ~Mounted() { vfs::file_systems().remove(share_->id()); }
    std::shared_ptr<FakeShare> share_;
};

}  // namespace

TEST_CASE("scene rotate: left then right and four turns restore the in-memory scene") {
    const splat::SplatScene original = splat_test::make_test_scene(200);

    splat::SplatScene round_trip = original;
    splat::rotate_scene_quarters(round_trip, 1);
    splat::rotate_scene_quarters(round_trip, 3);   // three clockwise = one left
    CHECK(max_position_drift(original, round_trip) <= 1.0e-5);
    CHECK(max_orientation_drift(original, round_trip) <= 1.0e-5);
    for (size_t i = 0; i < original.log_scale.size(); ++i) {
        CHECK(round_trip.log_scale[i] == original.log_scale[i]);
    }
    for (size_t i = 0; i < original.sh_dc.size(); ++i) {
        CHECK(round_trip.sh_dc[i] == original.sh_dc[i]);
    }
    for (size_t i = 0; i < original.opacity_logit.size(); ++i) {
        CHECK(round_trip.opacity_logit[i] == original.opacity_logit[i]);
    }

    splat::SplatScene four = original;
    for (int i = 0; i < 4; ++i) splat::rotate_scene_quarters(four, 1);
    CHECK(max_position_drift(original, four) <= 1.0e-5);
    CHECK(max_orientation_drift(original, four) <= 1.0e-5);

    // One right is a 90 degree rotation about +z, the viewing axis: a point on
    // +x goes to +y (down on screen), the same way a photo turns clockwise.
    splat::SplatScene axis;
    axis.resize(1);
    axis.position = {1.0f, 0.0f, 0.0f};
    axis.log_scale = {0.0f, 0.0f, 0.0f};
    axis.rotation = {1.0f, 0.0f, 0.0f, 0.0f};  // identity (w, x, y, z)
    axis.sh_dc = {0.0f, 0.0f, 0.0f};
    axis.opacity_logit = {0.0f};
    splat::rotate_scene_quarters(axis, 1);
    CHECK(axis.position[0] == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(axis.position[1] == doctest::Approx(1.0f).epsilon(1e-6));
    CHECK(axis.position[2] == doctest::Approx(0.0f).epsilon(1e-6));
    // The Gaussian turned too: q = qz * identity = (cos45, 0, 0, sin45).
    CHECK(std::fabs(axis.rotation[0]) == doctest::Approx(std::sqrt(0.5f)).epsilon(1e-5));
    CHECK(std::fabs(axis.rotation[3]) == doctest::Approx(std::sqrt(0.5f)).epsilon(1e-5));
}

TEST_CASE("scene rotate: a Gaussian's orientation takes the world turn on the left") {
    // cov3d = R S S^T R^T (core/render/project_math.h); turning the world means
    // R' = Rz R, i.e. q' = qz * q. A right multiply (q * qz) leaves the
    // ellipsoid in the wrong frame, which only an anisotropic Gaussian shows.
    const auto covariance = [](const float q[4], const float log_scale[3]) {
        const splat::Mat3 rotation =
            splat::quaternion_to_rotation(q[0], q[1], q[2], q[3]);
        splat::Mat3 scale;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) scale.at(i, j) = 0.0f;
            const double sigma = std::exp(static_cast<double>(log_scale[i]));
            scale.at(i, i) = static_cast<float>(sigma * sigma);
        }
        return rotation * scale * splat::transpose(rotation);
    };

    splat::SplatScene scene;
    scene.resize(1);
    scene.position = {0.0f, 0.0f, 0.0f};
    scene.rotation = {std::sqrt(0.5f), std::sqrt(0.5f), 0.0f, 0.0f};  // 90 deg about x
    scene.log_scale = {static_cast<float>(std::log(2.0)), 0.0f,
                       static_cast<float>(std::log(0.5))};
    scene.sh_dc = {0.0f, 0.0f, 0.0f};
    scene.opacity_logit = {0.0f};

    const splat::Mat3 before = covariance(scene.rotation.data(), scene.log_scale.data());
    splat::rotate_scene_quarters(scene, 1);
    const splat::Mat3 after = covariance(scene.rotation.data(), scene.log_scale.data());

    splat::Mat3 turn;
    turn.at(0, 0) = 0.0f;
    turn.at(0, 1) = -1.0f;
    turn.at(1, 0) = 1.0f;
    turn.at(1, 1) = 0.0f;
    const splat::Mat3 expected = turn * before * splat::transpose(turn);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            CHECK(after.at(r, c) == doctest::Approx(expected.at(r, c)).epsilon(1e-5));
        }
    }
    // The concrete numbers for this case: the long axis (x, 2 sigma) stays in
    // the plane and turns from x to y; the thin axis (0.5 sigma) stays on z.
    CHECK(after.at(0, 0) == doctest::Approx(0.25).epsilon(1e-5));
    CHECK(after.at(1, 1) == doctest::Approx(4.0).epsilon(1e-5));
    CHECK(after.at(2, 2) == doctest::Approx(1.0).epsilon(1e-5));
    CHECK(std::fabs(after.at(0, 1)) < 1.0e-5);
    CHECK(std::fabs(after.at(0, 2)) < 1.0e-5);
    CHECK(std::fabs(after.at(1, 2)) < 1.0e-5);
}

TEST_CASE("scene rotate: intrinsics and image dimensions swap, scale and keys survive") {
    splat_test::TempDir temp("scene-rotate-folder");
    const Path folder = make_scene(temp.path() / "scenes" / "s");
    // A legacy rotation and a scale, plus a key this build does not know.
    nlohmann::json meta = read_json(splat::scene_files(folder).meta);
    meta["view"] = {{"rotation", {0.0, std::sqrt(0.5), 0.0, std::sqrt(0.5)}}, {"scale", 0.5}};
    meta["future_field"] = {{"kept", true}};
    write_json(splat::scene_files(folder).meta, meta);

    const splat::Intrinsics expected = splat::rotate_scene_intrinsics(
        splat::Intrinsics{64, 32, 80.0f, 70.0f, 20.0f, 10.0f}, 1);
    CHECK(expected.width == 32);
    CHECK(expected.height == 64);
    CHECK(expected.fx == doctest::Approx(70.0f));
    CHECK(expected.fy == doctest::Approx(80.0f));

    REQUIRE_MESSAGE(splat::rotate_scene_folder(folder, 1).ok(), "rotate");
    const auto loaded = splat::read_scene_folder(folder);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(loaded.value().scene.count() == 8);
    CHECK(loaded.value().meta.intrinsics.width == 32);
    CHECK(loaded.value().meta.intrinsics.height == 64);
    CHECK(loaded.value().meta.intrinsics.fx == doctest::Approx(expected.fx));
    CHECK(loaded.value().meta.intrinsics.fy == doctest::Approx(expected.fy));
    CHECK(loaded.value().meta.intrinsics.cx == doctest::Approx(expected.cx));
    CHECK(loaded.value().meta.intrinsics.cy == doctest::Approx(expected.cy));
    CHECK(loaded.value().meta.backend == "sharp");
    CHECK(loaded.value().meta.source == "inbox");

    // original.jpg and thumb.jpg turned: 64x32 -> 32x64.
    const auto original = decode_vfs(splat::scene_files(folder).original);
    REQUIRE_MESSAGE(original.ok(), original.message());
    CHECK(original.value().width == 32);
    CHECK(original.value().height == 64);
    const auto thumb = decode_vfs(splat::scene_files(folder).thumbnail);
    REQUIRE_MESSAGE(thumb.ok(), thumb.message());
    CHECK(thumb.value().width == 32);
    CHECK(thumb.value().height == 64);

    // meta.json: rotation gone, scale and unknown keys kept.
    const nlohmann::json after = read_json(splat::scene_files(folder).meta);
    REQUIRE(after.contains("view"));
    CHECK_FALSE(after["view"].contains("rotation"));
    CHECK(after["view"]["scale"].get<float>() == doctest::Approx(0.5f));
    CHECK(after["future_field"]["kept"].get<bool>());
}

TEST_CASE("scene rotate: a failed replace keeps the old scene whole and openable") {
    splat_test::TempDir temp("scene-rotate-failed");
    auto share = std::make_shared<FakeShare>("sr0");
    Mounted mounted(share);
    const Path folder("sr0:/library/scene");
    make_scene(folder);
    const std::vector<uint8_t> splat_before = read_bytes(splat::scene_files(folder).splat);
    const std::vector<uint8_t> meta_before = read_bytes(splat::scene_files(folder).meta);
    const std::vector<uint8_t> original_before = read_bytes(splat::scene_files(folder).original);
    share->can_rename = false;

    const splat::Status status = splat::rotate_scene_folder(folder, 1);
    CHECK_FALSE(status.ok());
    // Every old file is still exactly what it was, and the scene still reads.
    CHECK(read_bytes(splat::scene_files(folder).splat) == splat_before);
    CHECK(read_bytes(splat::scene_files(folder).meta) == meta_before);
    CHECK(read_bytes(splat::scene_files(folder).original) == original_before);
    const auto loaded = splat::read_scene_folder(folder);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(loaded.value().scene.count() == 8);
    // No temporary is left behind.
    for (const auto& file : share->files) {
        CHECK(file.first.find(".rotate-tmp") == std::string::npos);
    }
}

TEST_CASE("scene rotate: a scene on a fake network location turns through its adapter") {
    splat_test::TempDir temp("scene-rotate-network");
    auto share = std::make_shared<FakeShare>("sr1");
    Mounted mounted(share);
    const Path folder("sr1:/library/scene");
    make_scene(folder, true);
    const std::vector<uint8_t> splat_before = read_bytes(splat::scene_files(folder).splat);

    REQUIRE_MESSAGE(splat::rotate_scene_folder(folder, 1).ok(), "rotate");
    CHECK(read_bytes(splat::scene_files(folder).splat) != splat_before);
    const auto loaded = splat::read_scene_folder(folder);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(loaded.value().meta.intrinsics.width == 32);
    CHECK(loaded.value().meta.intrinsics.height == 64);
    CHECK(loaded.value().meta.origin_format == "splat");  // provenance kept
    const auto original = decode_vfs(splat::scene_files(folder).original);
    REQUIRE_MESSAGE(original.ok(), original.message());
    CHECK(original.value().width == 32);
    CHECK(original.value().height == 64);
}

TEST_CASE("scene rotate: a share whose rename cannot replace still overrides the scene") {
    // libsmb2 6.3 sends ReplaceIfExists=0, so every SMB scene turn hits an
    // existing target. vfs::rename_file() publishes through a sidecar backup;
    // the turn must still land and leave no sidecar or temporary behind.
    splat_test::TempDir temp("scene-rotate-no-replace");
    auto share = std::make_shared<FakeShare>("sr2");
    Mounted mounted(share);
    share->replaces = false;
    const Path folder("sr2:/library/scene");
    make_scene(folder, true);
    const std::vector<uint8_t> splat_before = read_bytes(splat::scene_files(folder).splat);

    const splat::Status status = splat::rotate_scene_folder(folder, 1);
    REQUIRE_MESSAGE(status.ok(), status.message());
    CHECK(read_bytes(splat::scene_files(folder).splat) != splat_before);
    const auto loaded = splat::read_scene_folder(folder);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(loaded.value().meta.intrinsics.width == 32);
    CHECK(loaded.value().meta.intrinsics.height == 64);
    const auto original = decode_vfs(splat::scene_files(folder).original);
    REQUIRE_MESSAGE(original.ok(), original.message());
    CHECK(original.value().width == 32);
    CHECK(original.value().height == 64);
    for (const auto& file : share->files) {
        CHECK(file.first.find(".rotate-tmp") == std::string::npos);
        CHECK(file.first.find(".replace-old") == std::string::npos);
    }
}

TEST_CASE("scene rotate: the fp16 file round trip drift after 40 turns is measured") {
    splat_test::TempDir temp("scene-rotate-drift");
    const Path folder = make_scene(temp.path() / "scenes" / "drift");
    const auto before = splat::read_scene_folder(folder);
    REQUIRE_MESSAGE(before.ok(), before.message());
    const splat::SplatScene original = before.value().scene;
    const auto original_jpeg = decode_vfs(splat::scene_files(folder).original);
    REQUIRE_MESSAGE(original_jpeg.ok(), original_jpeg.message());

    // Forty turns = ten full circles.
    for (int i = 0; i < 40; ++i) {
        REQUIRE_MESSAGE(splat::rotate_scene_folder(folder, 1).ok(), "turn " << i);
    }
    const auto after = splat::read_scene_folder(folder);
    REQUIRE_MESSAGE(after.ok(), after.message());
    const double position_drift = max_position_drift(original, after.value().scene);
    const double orientation_drift = max_orientation_drift(original, after.value().scene);
    const auto after_jpeg = decode_vfs(splat::scene_files(folder).original);
    REQUIRE_MESSAGE(after_jpeg.ok(), after_jpeg.message());
    double jpeg_drift = 0.0;
    double jpeg_mean = 0.0;
    REQUIRE(original_jpeg.value().rgb.size() == after_jpeg.value().rgb.size());
    for (size_t i = 0; i < original_jpeg.value().rgb.size(); ++i) {
        const double difference = std::fabs(static_cast<double>(original_jpeg.value().rgb[i]) -
                                            after_jpeg.value().rgb[i]);
        jpeg_drift = std::max(jpeg_drift, difference);
        jpeg_mean += difference;
    }
    jpeg_mean /= static_cast<double>(std::max<size_t>(1, original_jpeg.value().rgb.size()));

    MESSAGE("40 turns of a .psplat scene: position max drift " << position_drift
                                                               << " m, orientation max drift "
                                                               << orientation_drift
                                                               << " rad, original.jpg max channel "
                                                               << jpeg_drift << " (mean "
                                                               << jpeg_mean << ") of 255");
    // The format is fp16 for positions/orientations and JPEG re-encodes the
    // images: the drift must be small, not zero. These bounds are the report's
    // numbers, not a promise of lossless turns.
    CHECK(position_drift < 0.01);
    CHECK(orientation_drift < 0.02);
    CHECK(jpeg_drift < 64.0);
}
