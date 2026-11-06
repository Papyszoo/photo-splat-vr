// v0.2 RC integration: the library track (photo formats, splat import, network
// locations through core/vfs) and the scaling/rotation track (capture-pivot
// scale, full 3D scene rotation stored per scene) working as one build. These
// are the cases neither worktree could test alone:
//
//   a) a rotation saved on a network-library scene goes through the
//      file-system interface to the share and is restored on reopening;
//   b) a scene imported from .ply/.splat/.spz keeps a saved rotation and its
//      saved scale;
//   c) a splat opened directly from a network library (no copy) works with
//      rotation, and a read-only share says "this console only" instead of
//      crashing - down to the panel's toast;
//   d) a photo with an orientation coming from a network source converts, with
//      its manual 90-degree rotate, into a network library, and the local
//      staging folder is left empty;
//   e) capture-pivot scaling still works on all of the above.
//
// Every network location is an in-memory IFileSystem registered as "fake" /
// "ro", the same mechanism tests/vfs_test.cpp and tests/jobs_upload_test.cpp
// use; nothing here contacts a share.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <miniz/miniz.h>
#include <miniz/miniz_tdef.h>
#include <nlohmann/json.hpp>

#include "common/exif.h"
#include "common/image.h"
#include "common/path.h"
#include "data_dir.h"
#include "jobs/gpu_phase_guard.h"
#include "jobs/image_io.h"
#include "jobs/job_queue.h"
#include "jobs/job_runner.h"
#include "recon/reconstructor.h"
#include "render/camera.h"
#include "render/hmd_view.h"
#include "scene/imported_scene.h"
#include "scene/ply.h"
#include "scene/scene_folder.h"
#include "scene/scene_view.h"
#include "scene/splat_scene.h"
#include "test_util.h"
#include "ui/screen.h"
#include "ui/screen_stack.h"
#include "ui/screens/scenes.h"
#include "ui/ui_input.h"
#include "gallery/photo_turns.h"
#include "vfs/io.h"

using splat::Path;

namespace {

constexpr float kPi = 3.14159265358979323846f;

std::vector<uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

std::string read_text_file(const std::filesystem::path& path) {
    const std::vector<uint8_t> bytes = read_bytes(path);
    return std::string(bytes.begin(), bytes.end());
}

std::vector<uint8_t> read_vfs_bytes(const Path& path) {
    const splat::Result<std::vector<uint8_t>> bytes = splat::vfs::read_file(path);
    REQUIRE_MESSAGE(bytes.ok(), bytes.message());
    return bytes.value();
}

std::string read_vfs_text(const Path& path) {
    const std::vector<uint8_t> bytes = read_vfs_bytes(path);
    return std::string(bytes.begin(), bytes.end());
}

// The four scene files as a JPEG pair: a minimal but valid-enough payload (the
// gallery only checks presence and lets stb fail into the placeholder tile).
const std::vector<uint8_t>& placeholder_jpeg() {
    static const std::vector<uint8_t> jpeg{0xFF, 0xD8, 0xFF, 0xD9};
    return jpeg;
}

splat::SplatScene test_scene(size_t count) { return splat_test::make_test_scene(count); }

// ---- the fake share ---------------------------------------------------------

// One in-memory network location. `writable` false is a read-only export: every
// write is refused with the share's own message, which is what the store's
// fallback and the panel's "this console only" toast must handle.
class FakeLibrary : public splat::vfs::IFileSystem {
public:
    FakeLibrary(std::string id, bool writable) : id_(std::move(id)), writable_(writable) {}

    std::string id() const override { return id_; }
    std::string describe() const override { return id_ + "://test"; }
    bool writable() const override { return writable_; }

    splat::Result<std::vector<uint8_t>> read(const std::string& relative, size_t) override {
        ++reads;
        const auto found = files.find(relative);
        if (found == files.end()) {
            return splat::Result<std::vector<uint8_t>>::failure("no such file: " + relative);
        }
        return splat::Result<std::vector<uint8_t>>::success(found->second);
    }
    splat::Status write(const std::string& relative, const uint8_t* data, size_t size) override {
        ++writes;
        ++write_log[relative];
        if (!writable_) return splat::Status::error("the share " + id_ + " is read-only");
        files[relative] = std::vector<uint8_t>(data, data + size);
        add_parents(relative);
        return splat::Status::success();
    }
    splat::Status list(const std::string& relative,
                       std::vector<splat::vfs::DirEntry>* entries) override {
        if (!directory_exists(relative)) {
            return splat::Status::error("not a directory: " + relative);
        }
        for (const std::string& directory : directories) {
            if (leaf(directory) == "." || !child_of(relative, directory)) continue;
            splat::vfs::DirEntry entry;
            entry.name = leaf(directory);
            entry.directory = true;
            entries->push_back(entry);
        }
        for (const auto& file : files) {
            if (!child_of(relative, file.first)) continue;
            splat::vfs::DirEntry entry;
            entry.name = leaf(file.first);
            entry.size = file.second.size();
            entries->push_back(entry);
        }
        return splat::Status::success();
    }
    splat::Status stat(const std::string& relative, splat::vfs::FileStat* out) override {
        *out = splat::vfs::FileStat{};
        const auto found = files.find(relative);
        if (found != files.end()) {
            out->exists = true;
            out->regular = true;
            out->size = found->second.size();
            return splat::Status::success();
        }
        if (directory_exists(relative)) {
            out->exists = true;
            out->directory = true;
        }
        return splat::Status::success();
    }
    splat::Status make_dirs(const std::string& relative) override {
        if (!writable_) return splat::Status::error("the share " + id_ + " is read-only");
        directories.insert(relative);
        add_parents(relative);
        return splat::Status::success();
    }
    splat::Status remove_file(const std::string& relative) override {
        ++writes;
        if (!writable_) return splat::Status::error("the share " + id_ + " is read-only");
        if (files.erase(relative) == 0) {
            return splat::Status::error("no such file: " + relative);
        }
        return splat::Status::success();
    }
    splat::Status remove_tree(const std::string& relative) override {
        ++writes;
        if (!writable_) return splat::Status::error("the share " + id_ + " is read-only");
        for (auto it = files.begin(); it != files.end();) {
            it = under(relative, it->first) ? files.erase(it) : std::next(it);
        }
        return splat::Status::success();
    }
    splat::Status rename(const std::string& from, const std::string& to) override {
        ++writes;
        if (!writable_) return splat::Status::error("the share " + id_ + " is read-only");
        const auto found = files.find(from);
        if (found == files.end()) return splat::Status::error("no such file: " + from);
        files[to] = std::move(found->second);
        files.erase(from);
        add_parents(to);
        return splat::Status::success();
    }

    void put(const std::string& relative, const std::vector<uint8_t>& bytes) {
        files[relative] = bytes;
        add_parents(relative);
    }
    bool contains(const std::string& relative) const { return files.count(relative) != 0; }
    std::size_t writes_to(const std::string& relative) const {
        const auto found = write_log.find(relative);
        return found == write_log.end() ? 0 : found->second;
    }

    static std::string leaf(const std::string& path) {
        const std::size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }
    static bool under(const std::string& parent, const std::string& path) {
        return path.size() > parent.size() + 1 && path.compare(0, parent.size(), parent) == 0 &&
               path[parent.size()] == '/';
    }
    static bool child_of(const std::string& parent, const std::string& path) {
        const std::string prefix = parent.empty() ? std::string() : parent + "/";
        if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0) {
            return false;
        }
        return path.find('/', prefix.size()) == std::string::npos;
    }

    std::map<std::string, std::vector<uint8_t>> files;
    std::set<std::string> directories;
    int reads = 0;   // every read the file-system interface handed here
    int writes = 0;  // every write attempt, refused ones included
    std::map<std::string, int> write_log;

private:
    void add_parents(const std::string& relative) {
        std::size_t slash = relative.find_last_of('/');
        while (slash != std::string::npos) {
            directories.insert(relative.substr(0, slash));
            slash = relative.substr(0, slash).find_last_of('/');
        }
    }
    bool directory_exists(const std::string& relative) const {
        return relative.empty() || directories.count(relative) != 0;
    }

    std::string id_;
    bool writable_;
};

// Registers a fake for the scope of one test and removes it afterwards, so a
// leftover id cannot answer another test's path.
struct MountedShare {
    MountedShare(const std::string& id, bool writable)
        : share(std::make_shared<FakeLibrary>(id, writable)) {
        splat::vfs::file_systems().add(share);
    }
    ~MountedShare() { splat::vfs::file_systems().remove(share->id()); }

    std::shared_ptr<FakeLibrary> share;
};

// Writes a normal converted scene folder through the file-system interface, so
// a location path gets the same four files a local conversion would.
Path write_scene_on(const Path& folder, const std::string& backend, bool imported = false) {
    splat::SceneMeta meta;
    meta.backend = backend;
    meta.seconds_taken = 2.0;
    meta.source = "inbox/IMG_0001.jpg";
    meta.intrinsics.fx = 900.0f;
    meta.intrinsics.fy = 900.0f;
    meta.intrinsics.cx = 640.0f;
    meta.intrinsics.cy = 436.0f;
    if (imported) {
        // Only an imported splat has a rotation the panel may change (the user's
        // decision of 2026-11-05 leaves converted scenes none).
        meta.origin_format = "splat";
        meta.origin_count = 5;
    }
    const splat::Status status =
        splat::write_scene_folder(folder, test_scene(5), meta, placeholder_jpeg(),
                                  placeholder_jpeg());
    REQUIRE_MESSAGE(status.ok(), status.message());
    return folder;
}

// An imported scene's own scale, the one view member that survives: the viewer
// has no scene rotation (DECISIONS.md, "Change 2026-11-05").
splat::SceneView scaled_view(float scale) {
    splat::SceneView view;
    view.scale = scale;
    return view;
}

// ---- source files the import and the conversion read ------------------------

void put_u32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFFu));
}

void put_f32(std::vector<uint8_t>& out, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    put_u32(out, bits);
}

// A .splat record: position, linear scale, sRGB colour+alpha, quaternion (w
// first). 32 bytes, the layout in SPEC.md Appendix A.1.
void put_splat_record(std::vector<uint8_t>& out, float x, float y, float z, float s, uint8_t r) {
    put_f32(out, x);
    put_f32(out, y);
    put_f32(out, z);
    put_f32(out, s);
    put_f32(out, s * 2.0f);
    put_f32(out, s * 0.5f);
    out.push_back(r);
    out.push_back(128);
    out.push_back(64);
    out.push_back(255);
    out.push_back(255);  // qw
    out.push_back(128);  // qx
    out.push_back(128);  // qy
    out.push_back(128);  // qz
}

std::vector<uint8_t> splat_file_bytes() {
    std::vector<uint8_t> bytes;
    put_splat_record(bytes, 0.0f, 0.0f, 2.0f, 0.01f, 255);
    put_splat_record(bytes, 0.2f, 0.0f, 2.0f, 0.02f, 200);
    put_splat_record(bytes, -0.2f, 0.1f, 2.2f, 0.03f, 160);
    return bytes;
}

// gzip around a raw deflate stream: miniz's tdefl writes deflate, not gzip (the
// same helper tests/splat_import_test.cpp uses).
std::vector<uint8_t> gzip(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out = {0x1f, 0x8b, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0xFF};
    size_t deflated_size = 0;
    void* deflated = tdefl_compress_mem_to_heap(payload.data(), payload.size(), &deflated_size, 0);
    REQUIRE(deflated != nullptr);
    out.insert(out.end(), static_cast<const uint8_t*>(deflated),
               static_cast<const uint8_t*>(deflated) + deflated_size);
    std::free(deflated);
    put_u32(out, static_cast<uint32_t>(mz_crc32(0, payload.data(), payload.size())));
    put_u32(out, static_cast<uint32_t>(payload.size()));
    return out;
}

// spz v3 legacy body with one point: header, position (24-bit fixed point at
// fractionalBits 12), alpha, colour, scales (b/16 - 10) and an identity
// smallest-three rotation. RUB -> OpenCV is the reader's job, not this test's.
std::vector<uint8_t> spz_v3_bytes() {
    std::vector<uint8_t> body;
    put_u32(body, 0x5053474eu);  // "NGSP"
    put_u32(body, 3);            // version
    put_u32(body, 1);            // numPoints
    body.push_back(0);           // shDegree
    body.push_back(12);          // fractionalBits
    body.push_back(0);           // flags
    body.push_back(0);           // reserved
    const int32_t x = 0;
    const int32_t y = 0;
    const int32_t z = 2 << 12;  // 2.0 m forward
    for (int32_t value : {x, y, z}) {
        body.push_back(static_cast<uint8_t>(value & 0xFF));
        body.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        body.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    }
    body.push_back(255);  // alpha -> a large opacity logit
    body.push_back(128);  // colour r/g/b
    body.push_back(128);
    body.push_back(128);
    body.push_back(144);  // scale -> log_scale 144/16 - 10 = -1.0
    body.push_back(144);
    body.push_back(144);
    // Smallest-three identity: largest component index 3 (w), zero magnitudes.
    put_u32(body, (3u << 30) | (511u << 20) | (511u << 10) | 511u);
    return gzip(body);
}

// A JPEG with an Exif APP1 segment right after SOI carrying Orientation. This is
// what a portrait phone photo looks like; the same builder as
// tests/jobs_runner_test.cpp's.
std::vector<uint8_t> with_exif_orientation(const std::vector<uint8_t>& jpeg, int orientation) {
    std::vector<uint8_t> tiff = {'I', 'I', 0x2A, 0x00, 8, 0, 0, 0};  // II, 42, IFD0 at 8
    tiff.push_back(1);  // one entry
    tiff.push_back(0);
    tiff.push_back(0x12);  // tag 0x0112 Orientation
    tiff.push_back(0x01);
    tiff.push_back(3);  // SHORT
    tiff.push_back(0);
    tiff.push_back(1);  // count 1
    tiff.push_back(0);
    tiff.push_back(0);
    tiff.push_back(0);
    tiff.push_back(static_cast<uint8_t>(orientation));
    tiff.push_back(0);
    tiff.push_back(0);
    tiff.push_back(0);
    tiff.push_back(0);  // no next IFD
    tiff.push_back(0);
    tiff.push_back(0);
    tiff.push_back(0);

    std::vector<uint8_t> app1 = {0xFF, 0xE1};
    const auto length = static_cast<uint16_t>(2 + 6 + tiff.size());
    app1.push_back(static_cast<uint8_t>(length >> 8));
    app1.push_back(static_cast<uint8_t>(length));
    const char exif_header[] = {'E', 'x', 'i', 'f', 0, 0};
    app1.insert(app1.end(), exif_header, exif_header + 6);
    app1.insert(app1.end(), tiff.begin(), tiff.end());

    std::vector<uint8_t> out(jpeg.begin(), jpeg.begin() + 2);
    out.insert(out.end(), app1.begin(), app1.end());
    out.insert(out.end(), jpeg.begin() + 2, jpeg.end());
    return out;
}

std::vector<uint8_t> encode_test_jpeg(int width, int height) {
    splat::Image image;
    image.resize(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t* pixel = image.pixel(x, y);
            pixel[0] = static_cast<uint8_t>(x * 255 / width);
            pixel[1] = static_cast<uint8_t>(y * 255 / height);
            pixel[2] = static_cast<uint8_t>((x + y) & 0xff);
        }
    }
    const auto encoded = splat::encode_jpeg(image, 90);
    REQUIRE_MESSAGE(encoded.ok(), encoded.message());
    return encoded.value();
}

Path fixture(const char* name) {
    return Path(__FILE__).parent_path() / "fixtures" / "photos" / name;
}

// wide.heic stores 32x16 with irot angle 0. Setting the angle byte to 1 makes
// libheif turn it while decoding (16x32), which is the container-transform arm
// of the orientation handling, exercised through the job pipeline here.
std::vector<uint8_t> rotated_heic_bytes() {
    std::vector<uint8_t> bytes = read_bytes(fixture("wide.heic"));
    REQUIRE_FALSE(bytes.empty());
    for (size_t i = 0; i + 4 <= bytes.size(); ++i) {
        if (bytes[i] == 'i' && bytes[i + 1] == 'r' && bytes[i + 2] == 'o' &&
            bytes[i + 3] == 't') {
            bytes[i + 4] = 1;
            return bytes;
        }
    }
    FAIL("wide.heic has no irot property");
    return bytes;
}

// ---- the conversion backend for test (d) ------------------------------------

class FakeReconstructor : public splat::IReconstructor {
public:
    splat::Status load(const Path&) override { return splat::Status::success(); }
    void unload() override {}
    splat::BackendInfo info() const override {
        splat::BackendInfo out;
        out.name = "fake";
        return out;
    }
    splat::Result<splat::SplatScene> run(const splat::ReconstructionInput& input,
                                         splat::Progress&) override {
        last_width = input.rgb.width;
        last_height = input.rgb.height;
        return splat::Result<splat::SplatScene>::success(test_scene(4));
    }

    int last_width = 0;
    int last_height = 0;
};

struct ConvertFixture {
    splat_test::TempDir dir{"v02-rc-convert"};
    Path scenes = dir.path() / "scenes";
    Path staging = dir.path() / "staging";
    splat::GpuPhaseGuard guard;
    splat::JobQueue queue;

    ConvertFixture() {
        std::error_code error;
        std::filesystem::create_directories(staging, error);
        const auto opened = splat::JobQueue::open(dir.path() / "jobs.json");
        REQUIRE_MESSAGE(opened.ok(), opened.message());
        queue = opened.value();
    }

    splat::JobRunner runner(Path output_root) {
        splat::JobRunnerConfig config;
        config.output_root = output_root;
        config.staging_root = staging;
        config.model_path = dir.path() / "model.gguf";
        config.thumbnail_max_edge = 128;
        return splat::JobRunner(
            queue, guard,
            [](const std::string& backend) -> std::unique_ptr<splat::IReconstructor> {
                if (backend != "fake") return nullptr;
                return std::make_unique<FakeReconstructor>();
            },
            config);
    }
};

// The meta.json a finished scene left on the location.
nlohmann::json remote_meta(const Path& folder) {
    return nlohmann::json::parse(read_vfs_text(folder / "meta.json"));
}

// ---- the head pose and the projection helpers for test (e) ------------------

splat::HeadPoses identity_head() {
    splat::HeadPoses poses;
    for (splat::TrackedPose* pose : {&poses.head, &poses.left, &poses.right}) {
        pose->tracked = true;
        pose->position[0] = pose->position[1] = pose->position[2] = 0.0f;
        pose->quaternion[0] = pose->quaternion[1] = pose->quaternion[2] = 0.0f;
        pose->quaternion[3] = 1.0f;
    }
    return poses;
}

}  // namespace

// --- (a) a saved rotation on a network scene reaches the share --------------

TEST_CASE("v02 rc: a legacy rotation on a network scene is ignored and a save drops it") {
    splat_test::TempDir temp("v02-rc-network-view");
    MountedShare mounted("fake", true);
    const Path state = temp.path() / "state";
    const Path folder("fake:/library/Album/net-scene");
    write_scene_on(folder, "sharp-smb", true);

    // A Track R build left a rotation and a scale in the share's meta.json.
    nlohmann::json legacy = remote_meta(folder);
    legacy["view"] = {{"rotation", {0.0, std::sqrt(0.5), 0.0, std::sqrt(0.5)}}, {"scale", 0.5}};
    const std::string text = legacy.dump(2) + "\n";
    REQUIRE_MESSAGE(splat::vfs::write_file(folder / "meta.json",
                                           reinterpret_cast<const uint8_t*>(text.data()),
                                           text.size())
                        .ok(),
                    "legacy meta");
    const int writes_before = mounted.share->writes;

    splat::SceneViewStore store(state);
    const auto before = store.load(folder);
    REQUIRE_MESSAGE(before.ok(), before.message());
    // The rotation has no reader any more; the scale still does.
    CHECK(before.value().scale == doctest::Approx(0.5f));
    CHECK_FALSE(std::filesystem::exists(state / "views.json"));

    // A save keeps the scale, drops the rotation, and writes through the share.
    bool local_only = false;
    const splat::Status saved = store.save(folder, before.value(), &local_only);
    REQUIRE_MESSAGE(saved.ok(), saved.message());
    CHECK_FALSE(local_only);
    CHECK(mounted.share->writes == writes_before + 1);
    const nlohmann::json stored = remote_meta(folder);
    REQUIRE(stored.contains("view"));
    CHECK_FALSE(stored["view"].contains("rotation"));
    CHECK(stored["view"]["scale"].get<float>() == doctest::Approx(0.5f));
    CHECK(stored["backend"].get<std::string>() == "sharp-smb");
    CHECK(stored["source"].get<std::string>() == "inbox/IMG_0001.jpg");
    CHECK(stored["splat_count"].get<uint32_t>() == 5);

    // Restored on reopening from the share, with a store that has no cache.
    splat::SceneViewStore reopened(temp.path() / "other-state");
    const auto after = reopened.load(folder);
    REQUIRE_MESSAGE(after.ok(), after.message());
    CHECK(after.value().scale == doctest::Approx(0.5f));

    const auto loaded = splat::read_scene_folder(folder);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(loaded.value().scene.count() == 5);
}

// --- (c) a splat opened in place from a network library ---------------------

TEST_CASE("v02 rc: a network splat opened in place works with rotation and no copy") {
    splat_test::TempDir temp("v02-rc-open-in-place");
    MountedShare mounted("fake", true);
    const Path state = temp.path() / "state";
    mounted.share->put("library/imports/tiny.splat", splat_file_bytes());

    // A .splat read straight from the share, the route main.cpp's OpenPath uses:
    // no folder, no meta, no copy.
    const Path splat_file("fake:/library/imports/tiny.splat");
    const auto opened = splat::read_scene_path(splat_file);
    REQUIRE_MESSAGE(opened.ok(), opened.message());
    CHECK(opened.value().scene.count() == 3);
    CHECK(opened.value().meta.origin_format == "splat");
    CHECK(opened.value().meta.origin_count == 3);

    // The panel imports it into the share's own library; the scene folder then
    // carries "origin" and takes a saved rotation through the same interface.
    splat::SceneMeta meta;
    meta.backend = "import";
    meta.origin_format = "splat";
    meta.origin_count = 3;
    const std::vector<uint8_t> thumb =
        splat::make_import_thumb(opened.value().scene, 64, 36);
    REQUIRE_FALSE(thumb.empty());
    const Path folder("fake:/library/Album/imported");
    const splat::Status written =
        splat::write_imported_scene(folder, opened.value().scene, meta, thumb);
    REQUIRE_MESSAGE(written.ok(), written.message());
    // An imported folder has no original.jpg, and the gallery still reads it.
    CHECK_FALSE(mounted.share->contains("library/Album/imported/original.jpg"));
    CHECK(mounted.share->contains("library/Album/imported/scene.psplat"));

    // An imported scene keeps its provenance and the gallery reads it back.
    const nlohmann::json stored = remote_meta(folder);
    REQUIRE(stored.contains("origin"));
    CHECK(stored["origin"]["format"].get<std::string>() == "splat");
    CHECK(stored["origin"]["count"].get<uint32_t>() == 3);
    const auto scene = splat::read_scene_folder(folder);
    REQUIRE_MESSAGE(scene.ok(), scene.message());
    CHECK(scene.value().meta.origin_format == "splat");

    // Nothing was copied: the console side holds only the state directory.
    CHECK_FALSE(std::filesystem::exists(temp.path() / "imported"));
    CHECK_FALSE(std::filesystem::exists(temp.path() / "tiny.splat"));
    CHECK(mounted.share->reads > 0);
}

// --- (c) a read-only share says so, and the panel shows it -------------------

TEST_CASE("v02 rc: a read-only share keeps an imported scene's scale on this console") {
    splat_test::TempDir temp("v02-rc-read-only");
    MountedShare mounted("ro", false);
    const Path state = temp.path() / "state";
    const Path folder("ro:/library/Album/ro1");
    // The scene is already on the share; this fixture only reads it.
    const Path local = temp.path() / "local-scene";
    write_scene_on(local, "sharp", true);
    const splat::SceneFiles files = splat::scene_files(local);
    mounted.share->put("library/Album/ro1/original.jpg", read_bytes(files.original));
    mounted.share->put("library/Album/ro1/thumb.jpg", read_bytes(files.thumbnail));
    mounted.share->put("library/Album/ro1/scene.psplat", read_bytes(files.splat));
    mounted.share->put("library/Album/ro1/meta.json", read_bytes(files.meta));

    const std::string meta_before = read_vfs_text(folder / "meta.json");

    // The store answers "saved, on this console only" - never a crash, never a
    // silent loss.
    splat::SceneViewStore store(state);
    splat::SceneView view;
    view.scale = 0.4f;
    bool local_only = false;
    const splat::Status saved = store.save(folder, view, &local_only);
    REQUIRE_MESSAGE(saved.ok(), saved.message());
    CHECK(local_only);
    CHECK(std::filesystem::exists(state / "views.json"));
    // The share refused it, and its meta.json is byte-identical.
    CHECK(mounted.share->writes >= 1);
    CHECK(read_vfs_text(folder / "meta.json") == meta_before);
    // The cache entry is keyed by the location id and the path inside it.
    CHECK(splat::scene_view_key(folder) == Path("ro:library/Album/ro1"));
    const nlohmann::json cache = nlohmann::json::parse(read_text_file(state / "views.json"));
    REQUIRE(cache.contains("ro:library/Album/ro1"));
    CHECK(cache["ro:library/Album/ro1"]["scale"].get<float>() == doctest::Approx(0.4f));

    // Reopening restores it from the console cache.
    const auto reopened = splat::SceneViewStore(state).load(folder);
    REQUIRE_MESSAGE(reopened.ok(), reopened.message());
    CHECK(reopened.value().scale == doctest::Approx(0.4f));
}

// --- (b) an imported scene keeps its rotation and its scale -----------------

TEST_CASE("v02 rc: an imported .splat, .ply or .spz keeps a saved rotation and scale") {
    splat_test::TempDir temp("v02-rc-import-view");
    ConvertFixture fixture;
    const Path uploads = temp.path() / "uploads";
    std::error_code error;
    std::filesystem::create_directories(uploads, error);

    // The three source files, with their expected per-splat log scale.
    {
        std::ofstream out(uploads / "cloud.splat", std::ios::binary | std::ios::trunc);
        const std::vector<uint8_t> bytes = splat_file_bytes();
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        REQUIRE(static_cast<bool>(out));
    }
    {
        const splat::SplatScene source = test_scene(4);
        REQUIRE_MESSAGE(splat::export_ply(uploads / "cloud.ply", source).ok(), "export failed");
    }
    {
        std::ofstream out(uploads / "cloud.spz", std::ios::binary | std::ios::trunc);
        const std::vector<uint8_t> bytes = spz_v3_bytes();
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        REQUIRE(static_cast<bool>(out));
    }

    struct Case {
        const char* file;
        const char* origin;
    };
    for (const Case& item : {Case{"cloud.splat", "splat"}, Case{"cloud.ply", "ply"},
                             Case{"cloud.spz", "spz"}}) {
        CAPTURE(item.file);
        const Path source = uploads / item.file;
        const auto id = fixture.queue.enqueue_import(source, "Imports");
        REQUIRE_MESSAGE(id.ok(), id.message());
        splat::JobRunner runner = fixture.runner(fixture.scenes);
        const auto outcome = runner.run_next();
        REQUIRE_MESSAGE(outcome.ok(), outcome.message());
        REQUIRE(outcome.value().has_value());
        REQUIRE_MESSAGE(outcome.value()->state == splat::JobState::Done,
                        fixture.queue.find(id.value())->message);

        const Path folder = outcome.value()->scene_folder;
        const auto scene = splat::read_scene_folder(folder);
        REQUIRE_MESSAGE(scene.ok(), scene.message());
        CHECK(scene.value().meta.origin_format == item.origin);
        CHECK(scene.value().meta.origin_count > 0);
        // The imported splats keep the file's own scales (fp16 in the scene
        // file, so a relative bound).
        const splat::SplatScene& stored_scene = scene.value().scene;
        REQUIRE(stored_scene.count() > 0);
        if (std::string(item.origin) == "spz") {
            CHECK(stored_scene.log_scale[0] == doctest::Approx(-1.0f).epsilon(1e-3));
        } else if (std::string(item.origin) == "splat") {
            CHECK(stored_scene.log_scale[0] == doctest::Approx(std::log(0.01f)).epsilon(1e-3));
            CHECK(stored_scene.log_scale[3] == doctest::Approx(std::log(0.02f)).epsilon(1e-3));
        } else {
            // export_ply writes the source's log scales verbatim.
            const splat::SplatScene source_scene = test_scene(4);
            for (size_t i = 0; i < source_scene.log_scale.size(); ++i) {
                CHECK(splat_test::close_enough(stored_scene.log_scale[i],
                                               source_scene.log_scale[i]));
            }
        }

        // The saved scale: the imported scene's own size lands in meta.json
        // beside "origin" and reads back.
        splat::SceneViewStore store(temp.path() / "state");
        const splat::Status saved = store.save(folder, scaled_view(1.75f), nullptr);
        REQUIRE_MESSAGE(saved.ok(), saved.message());
        const nlohmann::json meta_json = nlohmann::json::parse(read_text_file(
            splat::scene_files(folder).meta));
        REQUIRE(meta_json.contains("origin"));
        REQUIRE(meta_json.contains("view"));
        CHECK_FALSE(meta_json["view"].contains("rotation"));
        CHECK(meta_json["view"]["scale"].get<float>() == doctest::Approx(1.75f));
        const auto view = splat::SceneViewStore(temp.path() / "state").load(folder);
        REQUIRE_MESSAGE(view.ok(), view.message());
        CHECK(view.value().scale == doctest::Approx(1.75f));
        CHECK(splat::scene_view_has_scale(view.value()));
    }

    // A converted scene (no "origin") stores no scale: its size is the
    // session's.
    const Path converted = temp.path() / "scenes" / "converted";
    write_scene_on(converted, "sharp");
    splat::SceneViewStore store(temp.path() / "state");
    REQUIRE_MESSAGE(store.save(converted, scaled_view(2.0f), nullptr).ok(), "save failed");
    const nlohmann::json converted_json =
        nlohmann::json::parse(read_text_file(splat::scene_files(converted).meta));
    CHECK_FALSE(converted_json.contains("view"));
    const auto converted_view = store.load(converted);
    REQUIRE_MESSAGE(converted_view.ok(), converted_view.message());
    CHECK(converted_view.value().scale == doctest::Approx(1.0f));
    CHECK_FALSE(splat::scene_view_has_scale(converted_view.value()));
}

// --- (d) a network photo with an orientation converts into a network library --

TEST_CASE("v02 rc: an oriented network photo converts with its rotate into a share") {
    MountedShare mounted("share", true);

    // Case 1: HEIC from the share, container rotation 90 degrees, manual 90
    // degrees back: 32x16 stored -> 16x32 on decode -> 32x16 written.
    SUBCASE("HEIC with a container rotation") {
#if !defined(SPLAT_HAVE_HEIF)
        MESSAGE("libheif is not built in; the HEIC conversion arm is skipped");
        return;
#else
        ConvertFixture fixture;
        mounted.share->put("photos/rotated.heic", rotated_heic_bytes());
        const Path photo("share:/photos/rotated.heic");
        const auto id = fixture.queue.enqueue(photo, "fake", "Album", 1, Path("share:/library"));
        REQUIRE_MESSAGE(id.ok(), id.message());
        splat::JobRunner runner = fixture.runner(fixture.scenes);
        const auto outcome = runner.run_next();
        REQUIRE_MESSAGE(outcome.ok(), outcome.message());
        REQUIRE(outcome.value().has_value());
        REQUIRE_MESSAGE(outcome.value()->state == splat::JobState::Done,
                        fixture.queue.find(id.value())->message);

        const Path folder("share:/library/Album/rotated");
        CHECK(outcome.value()->scene_folder == folder);
        for (const char* name : {"original.jpg", "thumb.jpg", "scene.psplat", "meta.json"}) {
            CHECK_MESSAGE(mounted.share->contains("library/Album/rotated/" + std::string(name)),
                          name);
        }
        const nlohmann::json meta = remote_meta(folder);
        CHECK(meta["intrinsics"]["width"].get<uint32_t>() == 32);
        CHECK(meta["intrinsics"]["height"].get<uint32_t>() == 16);
        // The source was not a JPEG, so original.jpg is a re-encode of the
        // oriented, rotated frame - never the HEIC bytes.
        const std::vector<uint8_t> original = read_vfs_bytes(folder / "original.jpg");
        REQUIRE(original.size() > 3);
        CHECK(original[0] == 0xFF);
        CHECK(original[1] == 0xD8);
        // The local staging folder is empty: the console kept nothing.
        CHECK(std::filesystem::is_empty(fixture.staging));

        // A converted scene on the share stores no scale: its size is the
        // session's. The imported scene's network save is covered by the first
        // network test.
        splat::SceneViewStore store(fixture.dir.path() / "state");
        REQUIRE_MESSAGE(store.save(folder, scaled_view(2.0f), nullptr).ok(), "remote save failed");
        const auto view = store.load(folder);
        REQUIRE_MESSAGE(view.ok(), view.message());
        CHECK(view.value().scale == doctest::Approx(1.0f));
        CHECK_FALSE(remote_meta(folder).contains("view"));
#endif
    }

    // Case 2: the EXIF arm. 40x24 stored, Orientation 6 (portrait on display:
    // 24x40), then the manual quarter turn writes 40x24 - the opposite of what
    // "rotate only" would produce, so the assertion really covers the EXIF step.
    SUBCASE("JPEG with an EXIF orientation") {
        ConvertFixture fixture;
        const std::vector<uint8_t> jpeg = with_exif_orientation(encode_test_jpeg(40, 24), 6);
        mounted.share->put("photos/portrait.jpg", jpeg);
        const Path photo("share:/photos/portrait.jpg");
        const auto id = fixture.queue.enqueue(photo, "fake", "Album", 1, Path("share:/library"));
        REQUIRE_MESSAGE(id.ok(), id.message());
        splat::JobRunner runner = fixture.runner(fixture.scenes);
        const auto outcome = runner.run_next();
        REQUIRE_MESSAGE(outcome.ok(), outcome.message());
        REQUIRE(outcome.value().has_value());
        REQUIRE_MESSAGE(outcome.value()->state == splat::JobState::Done,
                        fixture.queue.find(id.value())->message);

        const Path folder("share:/library/Album/portrait");
        const nlohmann::json meta = remote_meta(folder);
        CHECK(meta["intrinsics"]["width"].get<uint32_t>() == 40);
        CHECK(meta["intrinsics"]["height"].get<uint32_t>() == 24);
        CHECK(std::filesystem::is_empty(fixture.staging));
        // The oriented frame is not the file's own JPEG, so it is re-encoded.
        CHECK(read_vfs_bytes(folder / "original.jpg") != jpeg);
    }

    // Case 3: RAW, when the external golden file is present (it is not
    // committed; the test skips, as the repository's rule says).
    SUBCASE("RAW from the golden data directory") {
        const std::filesystem::path raw = splat_test::data_path("golden/raw/sample.dng");
        if (!std::filesystem::exists(raw)) {
            MESSAGE("no " << raw.string() << "; the RAW conversion arm is skipped");
            return;
        }
        ConvertFixture fixture;
        mounted.share->put("photos/raw.dng", read_bytes(raw));
        const Path photo("share:/photos/raw.dng");
        const auto id = fixture.queue.enqueue(photo, "fake", "Album", 1, Path("share:/library"));
        REQUIRE_MESSAGE(id.ok(), id.message());
        splat::JobRunner runner = fixture.runner(fixture.scenes);
        const auto outcome = runner.run_next();
        REQUIRE_MESSAGE(outcome.ok(), outcome.message());
        REQUIRE_MESSAGE(outcome.value()->state == splat::JobState::Done,
                        fixture.queue.find(id.value())->message);
        CHECK(std::filesystem::is_empty(fixture.staging));
        CHECK(mounted.share->contains("library/Album/raw/meta.json"));
    }
}

// --- (d2) a turned network photo converts with exactly one turn ------------

TEST_CASE("v02 rc: a turned network photo converts with exactly one turn on both routes") {
    MountedShare mounted("share", true);
    ConvertFixture fixture;
    const splat::PhotoTurnStore turns(fixture.dir.path() / "state");
    const Path output("share:/library");

    SUBCASE("a JPEG is rewritten on the share and the job carries no turn") {
        const splat::Path photo("share:/photos/rewritten.jpg");
        mounted.share->put("photos/rewritten.jpg", encode_test_jpeg(64, 32));
        const auto turned = splat::rotate_photo_file(photo, 1, turns, true);
        REQUIRE_MESSAGE(turned.ok(), turned.message());
        REQUIRE(turned.value().route == splat::PhotoTurnResult::Route::rewritten);
        REQUIRE_MESSAGE(fixture.queue.enqueue(photo, "fake", "Album", turns.load(photo), output).ok(),
                        "enqueue");
        const auto result = fixture.runner(fixture.scenes).run_next();
        REQUIRE_MESSAGE(result.ok(), result.message());
        REQUIRE_MESSAGE(result.value()->state == splat::JobState::Done,
                        fixture.queue.find(result.value()->job_id)->message);
        REQUIRE(mounted.share->contains("library/Album/rewritten/meta.json"));
        const nlohmann::json meta = remote_meta(Path("share:/library/Album/rewritten"));
        CHECK(meta["intrinsics"]["width"].get<uint32_t>() == 32);
        CHECK(meta["intrinsics"]["height"].get<uint32_t>() == 64);
        // The share's source was not re-encoded: it still has the original
        // image data and the new tag.
        const std::vector<uint8_t> source = read_vfs_bytes(photo);
        CHECK(splat::parse_exif(source.data(), source.size()).value().orientation.value_or(1) == 6);
    }

    SUBCASE("a PNG keeps its source and the job applies the saved turn once") {
        std::vector<uint8_t> rgba(64 * 32 * 4, 120);
        const auto png = splat::encode_png(rgba.data(), 64, 32);
        REQUIRE_MESSAGE(png.ok(), png.message());
        const splat::Path photo("share:/photos/saved.png");
        mounted.share->put("photos/saved.png", png.value());
        const auto turned = splat::rotate_photo_file(photo, 1, turns, true);
        REQUIRE_MESSAGE(turned.ok(), turned.message());
        REQUIRE(turned.value().route == splat::PhotoTurnResult::Route::saved);
        REQUIRE(turned.value().quarters == 1);
        CHECK(read_vfs_bytes(photo) == png.value());
        REQUIRE_MESSAGE(fixture.queue.enqueue(photo, "fake", "Album", turns.load(photo), output).ok(),
                        "enqueue");
        const auto result = fixture.runner(fixture.scenes).run_next();
        REQUIRE_MESSAGE(result.ok(), result.message());
        REQUIRE_MESSAGE(result.value()->state == splat::JobState::Done,
                        fixture.queue.find(result.value()->job_id)->message);
        REQUIRE(mounted.share->contains("library/Album/saved/meta.json"));
        const nlohmann::json meta = remote_meta(Path("share:/library/Album/saved"));
        CHECK(meta["intrinsics"]["width"].get<uint32_t>() == 32);
        CHECK(meta["intrinsics"]["height"].get<uint32_t>() == 64);
    }
}

// --- (e) capture-pivot scaling on an imported network scene -----------------

TEST_CASE("v02 rc: capture-pivot scaling works on an imported network scene") {
    splat_test::TempDir temp("v02-rc-pivot");
    MountedShare mounted("fake", true);
    const Path folder("fake:/library/Album/near");
    write_scene_on(folder, "sharp", true);  // an import: it keeps its own scale

    splat::HmdView view;
    REQUIRE(view.place(identity_head()));
    view.set_scale_pivot_mode(splat::ScalePivotMode::capture);
    view.set_median_depth(1.6f);
    const splat::FrameView before = view.frame();

    const float point[3] = {0.30f, 0.20f, 2.0f};
    const float offset_before[3] = {view.scene_offset()[0], view.scene_offset()[1],
                                    view.scene_offset()[2]};
    view.begin_scale_gesture();
    view.scale_scene(0.5f);
    view.end_scale_gesture();
    const splat::FrameView after = view.frame();
    CHECK(view.world_scale() == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK(after.scene_scale == doctest::Approx(0.5f).epsilon(1e-4));
    // Capture pivots at the capture point (the scene origin in display space):
    // the offset does not move and every display point is scaled about it.
    for (int axis = 0; axis < 3; ++axis) {
        CHECK(view.scene_offset()[axis] == doctest::Approx(offset_before[axis]).epsilon(1e-6));
        // display = scale * p, with the camera at the origin.
        CHECK((point[axis] * after.scene_scale - before.scene_scale * point[axis]) ==
              doctest::Approx(-0.5f * point[axis]).epsilon(1e-5));
    }

    // The other pivots still produce a finite placement.
    view.set_scale_pivot_mode(splat::ScalePivotMode::centre);
    view.set_pivot_samples({0.0f, 0.0f, 0.8f, 0.0f, 0.0f, 4.0f});
    view.begin_scale_gesture();
    view.scale_scene(0.8f);
    view.end_scale_gesture();
    CHECK(std::isfinite(view.scene_offset()[0]));
    CHECK(std::isfinite(view.scene_offset()[1]));
    CHECK(std::isfinite(view.scene_offset()[2]));
    CHECK(view.world_scale() > 0.0f);
}
