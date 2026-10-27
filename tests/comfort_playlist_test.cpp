#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <thread>

#include "render/scene_playlist.h"
#include "scene/psplat.h"
#include "scene/splat_scene.h"

namespace {

splat::SplatScene make_scene(size_t count, float marker) {
    splat::SplatScene scene;
    scene.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const float x = static_cast<float>(i) * 0.01f;
        scene.position[3 * i + 0] = x;
        scene.position[3 * i + 1] = 0.0f;
        scene.position[3 * i + 2] = 1.0f + marker;
        scene.log_scale[3 * i + 0] = -3.0f;
        scene.log_scale[3 * i + 1] = -3.0f;
        scene.log_scale[3 * i + 2] = -3.0f;
        scene.rotation[4 * i + 0] = 1.0f;
        scene.sh_dc[3 * i + 0] = 0.5f / 0.28209479177387814f;
        scene.sh_dc[3 * i + 1] = 0.5f / 0.28209479177387814f;
        scene.sh_dc[3 * i + 2] = 0.5f / 0.28209479177387814f;
        scene.opacity_logit[i] = 10.0f;
    }
    return scene;
}

struct TempPlaylist {
    splat::Path root;

    TempPlaylist() {
        std::random_device random;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
               ("splat_playlist_" + std::to_string(stamp) + "_" + std::to_string(random()));
        std::filesystem::create_directories(root);
    }
    ~TempPlaylist() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    splat::Path folder(const std::string& name) const {
        const splat::Path dir = root / name;
        std::filesystem::create_directories(dir);
        return dir;
    }
};

bool wait_ready(const splat::ScenePreloader& preloader, int timeout_ms = 5000) {
    const auto start = std::chrono::steady_clock::now();
    while (!preloader.ready() && !preloader.failed()) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count() > timeout_ms) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return preloader.ready();
}

}  // namespace

TEST_CASE("playlist sorts scene folders and wraps previous/next") {
    TempPlaylist temp;
    // Created out of order on purpose.
    const auto c = temp.folder("c_scene");
    const auto a = temp.folder("a_scene");
    const auto b = temp.folder("b_scene");
    REQUIRE(splat::write_psplat(a / "scene.psplat", make_scene(1, 0.0f)).ok());
    REQUIRE(splat::write_psplat(b / "scene.psplat", make_scene(2, 0.0f)).ok());
    REQUIRE(splat::write_psplat(c / "scene.psplat", make_scene(3, 0.0f)).ok());
    // A folder without scene.psplat is ignored.
    temp.folder("not_a_scene");

    auto opened = splat::ScenePlaylist::from_folder(temp.root);
    REQUIRE_MESSAGE(opened.ok(), opened.message());
    splat::ScenePlaylist playlist = opened.take();
    REQUIRE(playlist.size() == 3);
    CHECK(playlist.index() == 0);
    CHECK(playlist.current().folder.filename() == "a_scene");

    playlist.next();
    CHECK(playlist.current().folder.filename() == "b_scene");
    playlist.next();
    CHECK(playlist.current().folder.filename() == "c_scene");
    playlist.next();  // wrap forward
    CHECK(playlist.current().folder.filename() == "a_scene");
    playlist.previous();  // wrap backward
    CHECK(playlist.current().folder.filename() == "c_scene");
    playlist.set_index(7);
    CHECK(playlist.index() == 1);
}

TEST_CASE("a single scene folder is a one-entry playlist") {
    TempPlaylist temp;
    const auto scene = temp.folder("only");
    REQUIRE(splat::write_psplat(scene / "scene.psplat", make_scene(1, 0.0f)).ok());

    auto opened = splat::ScenePlaylist::from_folder(scene);
    REQUIRE_MESSAGE(opened.ok(), opened.message());
    CHECK(opened.value().size() == 1);
    opened.value().next();
    CHECK(opened.value().index() == 0);
}

TEST_CASE("playlist rejects a folder with no scenes") {
    TempPlaylist temp;
    temp.folder("empty");
    auto opened = splat::ScenePlaylist::from_folder(temp.root);
    CHECK_FALSE(opened.ok());
}

TEST_CASE("preload swap keeps the old scene valid until the new one is taken") {
    TempPlaylist temp;
    const auto a = temp.folder("a");
    const auto b = temp.folder("b");
    REQUIRE(splat::write_psplat(a / "scene.psplat", make_scene(1, 0.0f)).ok());
    REQUIRE(splat::write_psplat(b / "scene.psplat", make_scene(3, 0.0f)).ok());

    splat::ScenePreloader preloader;
    preloader.start(a / "scene.psplat");
    REQUIRE(wait_ready(preloader));
    auto first = preloader.take();
    REQUIRE(first.ok());
    CHECK(first.value().scene.count() == 1);

    // Start loading the next scene while the first is still in use.
    preloader.start(b / "scene.psplat");
    CHECK(preloader.running());
    // The old scene stays valid and unchanged during the preload.
    CHECK(first.value().scene.count() == 1);

    REQUIRE(wait_ready(preloader));
    auto second = preloader.take();
    REQUIRE(second.ok());
    CHECK(second.value().scene.count() == 3);
    CHECK(first.value().scene.count() == 1);
    CHECK(preloader.last_load_ms() >= 0.0);
    CHECK_FALSE(preloader.running());
    CHECK_FALSE(preloader.ready());
}

TEST_CASE("preloader reports a missing scene as a failure") {
    TempPlaylist temp;
    splat::ScenePreloader preloader;
    preloader.start(temp.root / "missing" / "scene.psplat");
    REQUIRE(wait_ready(preloader) == false);
    CHECK(preloader.failed());
    CHECK_FALSE(preloader.error().empty());
    auto taken = preloader.take();
    CHECK_FALSE(taken.ok());
}

TEST_CASE("wait and cancel drop a ready or failed preload and return to idle") {
    TempPlaylist temp;
    const auto a = temp.folder("a");
    REQUIRE(splat::write_psplat(a / "scene.psplat", make_scene(2, 0.0f)).ok());

    splat::ScenePreloader preloader;
    preloader.start(a / "scene.psplat");
    REQUIRE(wait_ready(preloader));
    CHECK(preloader.path() == a / "scene.psplat");
    preloader.cancel();
    // Drops the loaded scene and the ready state; take() then has nothing.
    CHECK_FALSE(preloader.ready());
    CHECK_FALSE(preloader.running());
    CHECK_FALSE(preloader.failed());
    CHECK_FALSE(preloader.take().ok());

    // A failure must not stay latched: the viewer polls failed() every frame
    // and would otherwise log "preload failed" forever.
    preloader.start(temp.root / "missing" / "scene.psplat");
    REQUIRE(wait_ready(preloader) == false);
    CHECK(preloader.failed());
    CHECK_FALSE(preloader.error().empty());
    preloader.cancel();
    CHECK_FALSE(preloader.failed());
    CHECK(preloader.error().empty());

    // The preloader is reusable after a cancel.
    preloader.start(a / "scene.psplat");
    REQUIRE(wait_ready(preloader));
    auto loaded = preloader.take();
    REQUIRE(loaded.ok());
    CHECK(loaded.value().scene.count() == 2);
    CHECK_FALSE(preloader.ready());
}

TEST_CASE("wait drops a ready preload without taking it") {
    TempPlaylist temp;
    const auto a = temp.folder("a");
    REQUIRE(splat::write_psplat(a / "scene.psplat", make_scene(1, 0.0f)).ok());

    splat::ScenePreloader preloader;
    preloader.start(a / "scene.psplat");
    REQUIRE(wait_ready(preloader));
    preloader.wait();
    CHECK_FALSE(preloader.ready());
    CHECK_FALSE(preloader.take().ok());
}
