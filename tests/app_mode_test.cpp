// D1 app-mode integration test: one photo goes in through the real LAN page
// (PIN login + multipart upload), the inbox watcher queues it, the job runner
// converts it with the fallback backend, the C5 scene folder appears and the
// playlist picks it up. The GPU phase ordering is checked from the transition
// log, not from the renderer: a "fake viewer" thread acquires and releases the
// viewing phase exactly like the desktop loop does, so the test runs on a host
// with no Vulkan device. The GPU parts (window, renderer, screenshots) are
// covered by the manual run in docs/desktop-app.md.
//
// The app-mode session lives in platform/desktop because it is the desktop
// app's wiring. splat_tests links splat_core only (adding a target would mean
// editing tests/CMakeLists.txt, outside this task's area), so this test
// compiles the same source file directly.
//
// Needs the converted DA2 GGUF and the golden photo; both come from the data
// directory (AGENTS.md), and the cases skip when they are absent:
//
//   export SPLAT_DA2_GGUF=$SPLAT_DATA_DIR/weights/dav2s-f16.gguf
//   build-*/tests/splat_tests -tc="app mode*"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "httplib/httplib.h"

#include "../platform/desktop/app_mode.cpp"

#include "data_dir.h"
#include "test_util.h"

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

// Converted fallback weights: $SPLAT_DA2_GGUF, else <data>/weights/dav2s-f16.gguf.
fs::path fallback_gguf() {
    const std::string from_env = env_or_empty("SPLAT_DA2_GGUF");
    if (!from_env.empty()) return fs::path(from_env);
    return splat_test::data_path("weights/dav2s-f16.gguf");
}

fs::path golden_photo() { return splat_test::data_path("golden/immichvr/4.jpg"); }

// Everything the app needs except the data directory, which the caller owns.
splat::AppModeOptions test_options(const fs::path& data_dir, const fs::path& gguf) {
    splat::AppModeOptions options;
    options.data_dir = data_dir;
    options.backend = "fallback";
    options.model_path = gguf;
    options.bind_address = "127.0.0.1";
    options.port = 0;  // let the OS pick a free port
    options.poll_interval_ms = 20;
    options.prefer_vulkan = false;  // no device needed for the ordering check
    return options;
}

std::string jpeg_bytes(size_t size = 256) {
    std::string data;
    data.push_back(static_cast<char>(0xff));
    data.push_back(static_cast<char>(0xd8));
    data.push_back(static_cast<char>(0xff));
    data.append(size, 'j');
    return data;
}

std::string login(httplib::Client& client, const std::string& pin) {
    const auto response =
        client.Post("/api/login", "pin=" + pin, "application/x-www-form-urlencoded");
    REQUIRE(response != nullptr);
    REQUIRE(response->status == 200);
    const std::string header = response->get_header_value("Set-Cookie");
    const std::string prefix = "splat_session=";
    REQUIRE(header.compare(0, prefix.size(), prefix) == 0);
    const size_t end = header.find(';');
    const std::string token = header.substr(
        prefix.size(), end == std::string::npos ? std::string::npos : end - prefix.size());
    client.set_default_headers({{"Cookie", prefix + token}});
    return token;
}

// The same two calls a phone makes: PIN login, then the multipart photo field.
splat::Result<std::string> upload(httplib::Client& client, const fs::path& photo) {
    using Outcome = splat::Result<std::string>;
    std::ifstream file(photo, std::ios::binary);
    if (!file) return Outcome::failure("cannot open " + photo.string());
    const std::string body((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    httplib::MultipartFormDataItems items;
    items.push_back({"photo", body, photo.filename().string(), "image/jpeg"});
    const auto response = client.Post("/api/upload", items);
    if (!response) return Outcome::failure("no response");
    if (response->status != 200) {
        return Outcome::failure("HTTP " + std::to_string(response->status) + ": " +
                                response->body);
    }
    const nlohmann::json parsed = nlohmann::json::parse(response->body);
    if (!parsed.value("ok", false)) return Outcome::failure("ok is not true");
    return Outcome::success(parsed.value("file", std::string()));
}

// A viewer that owns no GPU objects but does the phase dance the desktop loop
// does: out when a job is pending, back in when the queue is empty again.
class FakeViewer {
public:
    explicit FakeViewer(splat::AppMode& app) : app_(app), thread_([this] { run(); }) {}
    ~FakeViewer() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
    }

    int allocations() const { return allocations_.load(); }

private:
    void run() {
        bool holding = false;
        while (!stop_.load()) {
            if (holding && (app_.release_requested() || app_.work_pending())) {
                app_.end_viewing();
                holding = false;
            } else if (!holding && !app_.work_pending() && app_.scene_count() > 0 &&
                       app_.try_begin_viewing()) {
                holding = true;
                ++allocations_;
            }
            std::this_thread::sleep_for(2ms);
        }
        if (holding) app_.end_viewing();
    }

    splat::AppMode& app_;
    std::atomic<bool> stop_{false};
    std::atomic<int> allocations_{0};
    std::thread thread_;
};

std::vector<std::string> read_lines(const fs::path& path) {
    std::vector<std::string> lines;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) lines.push_back(line);
    return lines;
}

// Index of the first line containing `token` at or after `from`, -1 when absent.
long find_line(const std::vector<std::string>& lines, const std::string& token, size_t from = 0) {
    for (size_t index = from; index < lines.size(); ++index) {
        if (lines[index].find(token) != std::string::npos) return static_cast<long>(index);
    }
    return -1;
}

}  // namespace

TEST_CASE("app mode: an upload becomes a scene, the playlist picks it up and the log shows the "
          "phase order") {
    const fs::path gguf = fallback_gguf();
    const fs::path photo = golden_photo();
    if (!fs::is_regular_file(gguf) || !fs::is_regular_file(photo)) {
        MESSAGE("skipping: missing " << gguf << " or " << photo);
        return;
    }

    splat_test::TempDir temp("app-mode");
    const fs::path data_dir = temp.path() / "data";
    splat::AppMode app(test_options(data_dir, gguf));
    const splat::Status started = app.start();
    REQUIRE_MESSAGE(started.ok(), started.message());

    CHECK(app.backend() == "fallback");
    CHECK(app.pin().size() == 6);
    CHECK(fs::is_directory(data_dir / "inbox"));
    CHECK(fs::is_directory(data_dir / "scenes"));
    CHECK(fs::is_directory(data_dir / "state"));
    CHECK(fs::is_directory(data_dir / "models"));
    CHECK(app.upload_url().rfind("http://127.0.0.1:", 0) == 0);
    CHECK(app.window_title().find("PIN " + app.pin()) != std::string::npos);
    CHECK(app.scene_count() == 0);
    CHECK(app.current_scene() == std::nullopt);

    FakeViewer viewer(app);

    httplib::Client client("127.0.0.1", app.port());
    login(client, app.pin());
    const splat::Result<std::string> stored = upload(client, photo);
    REQUIRE_MESSAGE(stored.ok(), stored.message());
    CHECK(fs::exists(data_dir / "inbox" / stored.value()));

    REQUIRE_MESSAGE(app.wait_for_jobs(1, 300s), "the conversion did not finish");
    CHECK(app.finished_jobs() == 1);

    // C5 scene folder, written by the runner through write_scene_folder. The
    // folder is named after the photo, not after the job id.
    const std::string folder_name = splat::scene_folder_base(stored.value());
    const fs::path scene_folder = data_dir / "scenes" / folder_name;
    CHECK(fs::is_regular_file(scene_folder / "scene.psplat"));
    CHECK(fs::is_regular_file(scene_folder / "meta.json"));
    CHECK(fs::is_regular_file(scene_folder / "original.jpg"));
    CHECK(fs::is_regular_file(scene_folder / "thumb.jpg"));
    const splat::Result<splat::SceneMeta> meta =
        splat::read_scene_meta(scene_folder / "meta.json");
    REQUIRE(meta.ok());
    CHECK(meta.value().backend == "depth-anything-v2-small");
    CHECK(meta.value().splat_count > 0);
    CHECK(meta.value().seconds_taken > 0.0);

    // The converted photo left the inbox, so a restart cannot convert it twice.
    CHECK(fs::is_regular_file(data_dir / "state" / "ingested" / stored.value()));
    CHECK_FALSE(fs::exists(data_dir / "inbox" / stored.value()));

    // Playlist: the new folder is the only entry and is selected.
    REQUIRE(app.scene_count() == 1);
    const std::optional<splat::PlaylistEntry> current = app.current_scene();
    REQUIRE(current.has_value());
    CHECK(current->folder.filename().string() == folder_name);
    CHECK(current->scene_file.filename().string() == "scene.psplat");
    CHECK(app.current_index() == 0);
    const splat::Result<splat::LoadedScene> reloaded = splat::read_scene_path(current->scene_file);
    REQUIRE(reloaded.ok());
    CHECK(reloaded.value().scene.count() == meta.value().splat_count);

    // The viewer went back in after the job, which is the point of the guard.
    CHECK(viewer.allocations() >= 1);

    // Second photo: now the viewer holds the GPU while the job arrives, so the
    // release-before-inference half of the ordering is exercised too, and the
    // playlist must put the newer scene first.
    const fs::path second_photo =
        fs::is_regular_file(splat_test::data_path("golden/immichvr/5.jpg"))
            ? splat_test::data_path("golden/immichvr/5.jpg")
            : photo;
    const splat::Result<std::string> stored_again = upload(client, second_photo);
    REQUIRE_MESSAGE(stored_again.ok(), stored_again.message());
    REQUIRE_MESSAGE(app.wait_for_jobs(2, 300s), "the second conversion did not finish");
    CHECK(app.finished_jobs() == 2);

    // Same photo name again (the second upload may reuse the first name): the
    // runner adds -2, so the newer scene never overwrites the older one.
    const std::string second_base = splat::scene_folder_base(stored_again.value());
    const std::string second_name =
        second_base == folder_name ? folder_name + "-2" : second_base;
    CHECK(fs::is_regular_file(data_dir / "scenes" / second_name / "scene.psplat"));
    REQUIRE(app.scene_count() == 2);
    REQUIRE(app.current_scene().has_value());
    CHECK(app.current_scene()->folder.filename().string() == second_name);
    REQUIRE(app.scene(1).has_value());
    CHECK(app.scene(1)->folder.filename().string() == folder_name);
    CHECK(app.current_index() == 0);

    // Let the fake viewer take the GPU back (the desktop loop does the same on
    // the next frame after the runner reported the unload).
    for (int attempt = 0; attempt < 400 && viewer.allocations() < 2; ++attempt) {
        std::this_thread::sleep_for(5ms);
    }
    CHECK(viewer.allocations() >= 2);

    app.stop();

    // Transition log: timestamps, measured memory, and the ordering the spec
    // asks for (viewer out before inference, model unloaded before the viewer
    // allocates again).
    const std::vector<std::string> lines = read_lines(app.transition_log_path());
    REQUIRE(lines.size() > 5);
    CHECK(lines.front().find(" app.start ") != std::string::npos);
    CHECK(find_line(lines, " app.backend ") >= 0);
    CHECK(find_line(lines, " app.enqueue ") >= 0);
    CHECK(find_line(lines, " app.job.done ") >= 0);
    CHECK(find_line(lines, " app.playlist ") >= 0);
    for (const std::string& line : lines) {
        CHECK(line.size() > 24);
        CHECK(line[4] == '-');            // 2026-10-26T...
        CHECK(line[10] == 'T');
        CHECK(line.find(" rss=") != std::string::npos);
        CHECK(line.find(" gpu=") != std::string::npos);
        CHECK(line.find(" phase=") != std::string::npos);
        // The guard lines carry the state at the moment of the transition (the
        // observer gets it from the guard's snapshot, it never re-reads a later
        // state), so the trailer is part of the ordering evidence.
        if (line.find(" model.loaded ") != std::string::npos) {
            CHECK(line.find("phase=inference") != std::string::npos);
            CHECK(line.find(" model=1 ") != std::string::npos);
        }
        if (line.find(" model.unloaded ") != std::string::npos) {
            CHECK(line.find(" model=0 ") != std::string::npos);
        }
        if (line.find(" viewer.begin ") != std::string::npos) {
            // The guard refuses viewers while the weights are resident.
            CHECK(line.find(" model=0 ") != std::string::npos);
        }
    }
    // stop() shut the guard down and logged it, after the job work.
    const long shutdown_line = find_line(lines, " guard.shutdown ");
    REQUIRE(shutdown_line >= 0);
    CHECK(find_line(lines, " app.stop ") > shutdown_line);

    const long inference_begin = find_line(lines, " inference.begin ");
    REQUIRE(inference_begin >= 0);
    const long model_loaded = find_line(lines, " model.loaded ", static_cast<size_t>(inference_begin));
    REQUIRE(model_loaded > inference_begin);
    // Weights are only ever resident during inference.
    CHECK(lines[static_cast<size_t>(model_loaded)].find("phase=inference") != std::string::npos);
    const long model_unloaded = find_line(lines, " model.unloaded ", static_cast<size_t>(model_loaded));
    REQUIRE(model_unloaded > model_loaded);
    const long inference_end = find_line(lines, " inference.end ", static_cast<size_t>(model_unloaded));
    REQUIRE(inference_end > model_unloaded);

    // The second job arrives while the fake viewer holds the GPU: the viewer
    // released (viewer.end) before inference.begin, and never re-entered in
    // between.
    const long second_inference_begin =
        find_line(lines, " inference.begin ", static_cast<size_t>(inference_end));
    REQUIRE(second_inference_begin > inference_end);
    long viewer_end_before = -1;
    for (long index = second_inference_begin - 1; index >= 0; --index) {
        if (lines[static_cast<size_t>(index)].find(" viewer.end ") != std::string::npos) {
            viewer_end_before = index;
            break;
        }
    }
    REQUIRE(viewer_end_before >= 0);
    CHECK(viewer_end_before < second_inference_begin);
    for (long index = viewer_end_before + 1; index < second_inference_begin; ++index) {
        CHECK(lines[static_cast<size_t>(index)].find(" viewer.begin ") == std::string::npos);
    }

    // Second job: same unload-before-reallocate order.
    const long second_loaded =
        find_line(lines, " model.loaded ", static_cast<size_t>(second_inference_begin));
    const long second_unloaded =
        find_line(lines, " model.unloaded ", static_cast<size_t>(second_loaded));
    const long second_end = find_line(lines, " inference.end ", static_cast<size_t>(second_unloaded));
    REQUIRE(second_loaded > second_inference_begin);
    REQUIRE(second_unloaded > second_loaded);
    REQUIRE(second_end > second_unloaded);

    // The viewer only came back after the weights were released.
    const long viewer_begin_after =
        find_line(lines, " viewer.begin ", static_cast<size_t>(second_end));
    REQUIRE(viewer_begin_after >= 0);
    CHECK(second_unloaded < viewer_begin_after);
    CHECK(second_end < viewer_begin_after);
}

TEST_CASE("app mode: the playlist is newest first, by scene time not folder name") {
    const fs::path gguf = fallback_gguf();
    if (!fs::is_regular_file(gguf)) {
        MESSAGE("skipping: missing " << gguf);
        return;
    }

    splat_test::TempDir temp("app-playlist");
    const fs::path data_dir = temp.path() / "data";
    const fs::path scenes = data_dir / "scenes";
    fs::create_directories(scenes);

    splat::SceneMeta meta;
    meta.backend = "test";
    meta.intrinsics = {640, 480, 500.0f, 500.0f, 320.0f, 240.0f};
    const std::string jpeg = jpeg_bytes();
    const std::vector<uint8_t> bytes(jpeg.begin(), jpeg.end());
    for (const char* name : {"job-000001", "job-000002"}) {
        const splat::Status written = splat::write_scene_folder(
            scenes / name, splat_test::make_test_scene(32), meta, bytes, bytes);
        REQUIRE_MESSAGE(written.ok(), written.message());
    }
    // The older job folder is the newer scene here, so name order and time
    // order disagree: the playlist must follow the scene time.
    std::error_code error;
    fs::last_write_time(scenes / "job-000001" / "scene.psplat",
                        fs::file_time_type::clock::now() - std::chrono::hours(2), error);
    REQUIRE_FALSE(error);

    splat::AppMode app(test_options(data_dir, gguf));
    const splat::Status started = app.start();
    REQUIRE_MESSAGE(started.ok(), started.message());

    REQUIRE(app.scene_count() == 2);
    REQUIRE(app.current_scene().has_value());
    CHECK(app.current_scene()->folder.filename().string() == "job-000002");
    REQUIRE(app.scene(1).has_value());
    CHECK(app.scene(1)->folder.filename().string() == "job-000001");

    app.next();
    CHECK(app.current_index() == 1);
    CHECK(app.current_scene()->folder.filename().string() == "job-000001");
    app.next();  // wraps
    CHECK(app.current_index() == 0);
    app.previous();
    CHECK(app.current_index() == 1);
    app.stop();
}

// stop() used to skip the guard shutdown whenever a conversion was running, so
// the session ended without "guard.shutdown" and stopped() stayed false.
TEST_CASE("app mode: stop during inference shuts the guard down") {
    const fs::path gguf = fallback_gguf();
    const fs::path photo = golden_photo();
    if (!fs::is_regular_file(gguf) || !fs::is_regular_file(photo)) {
        MESSAGE("skipping: missing " << gguf << " or " << photo);
        return;
    }

    splat_test::TempDir temp("app-stop");
    const fs::path data_dir = temp.path() / "data";
    splat::AppMode app(test_options(data_dir, gguf));
    const splat::Status started = app.start();
    REQUIRE_MESSAGE(started.ok(), started.message());

    httplib::Client client("127.0.0.1", app.port());
    login(client, app.pin());
    const splat::Result<std::string> stored = upload(client, photo);
    REQUIRE_MESSAGE(stored.ok(), stored.message());

    // Wait for the runner to hold the GPU with the weights resident, then stop
    // in the middle of the conversion.
    bool resident = false;
    for (int attempt = 0; attempt < 4000 && !resident; ++attempt) {
        resident = find_line(read_lines(app.transition_log_path()), " model.loaded ") >= 0;
        if (!resident) std::this_thread::sleep_for(2ms);
    }
    CHECK(resident);

    const auto before = std::chrono::steady_clock::now();
    app.stop();
    const double stop_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - before).count();
    // The job that already owns the GPU finishes before stop() returns.
    CHECK(stop_seconds < 120.0);
    CHECK(app.phase_guard_stopped());

    const std::vector<std::string> lines = read_lines(app.transition_log_path());
    const long shutdown_line = find_line(lines, " guard.shutdown ");
    REQUIRE(shutdown_line >= 0);
    CHECK(find_line(lines, " app.stop ") > shutdown_line);
    // Stopping with the weights resident is allowed, but the guard line says
    // so, and the running job still unloads before it ends.
    if (lines[static_cast<size_t>(shutdown_line)].find(" model=1 ") != std::string::npos) {
        CHECK(find_line(lines, " model.unloaded ", static_cast<size_t>(shutdown_line)) >
              shutdown_line);
    }
}

// The page address must never be a wildcard or a public address, and binding
// loopback has to be an explicit choice (--bind), not a silent fallback.
TEST_CASE("app mode: the page is never bound to 0.0.0.0 or a public address") {
    const fs::path gguf = fallback_gguf();
    if (!fs::is_regular_file(gguf)) {
        MESSAGE("skipping: missing " << gguf);
        return;
    }

    splat_test::TempDir temp("app-bind");

    for (const char* address : {"0.0.0.0", "8.8.8.8"}) {
        std::string folder(address);
        std::replace(folder.begin(), folder.end(), '.', '-');
        splat::AppModeOptions options = test_options(temp.path() / folder, gguf);
        options.bind_address = address;
        splat::AppMode app(options);
        const splat::Status refused = app.start();
        CHECK_FALSE(refused.ok());
        CHECK(refused.message().find("refusing to bind") != std::string::npos);
        app.stop();  // start() failed: nothing was started
        CHECK_FALSE(app.phase_guard_stopped());
    }

    // Default (no --bind): a private LAN address, or a clear error asking for
    // an explicit --bind 127.0.0.1. Never loopback by accident, never 0.0.0.0.
    splat::AppModeOptions automatic = test_options(temp.path() / "auto", gguf);
    automatic.bind_address.clear();
    splat::AppMode app(automatic);
    const splat::Status started = app.start();
    if (started.ok()) {
        CHECK(app.bind_address() != "0.0.0.0");
        CHECK(app.bind_address().rfind("127.", 0) != 0);
        app.stop();
    } else {
        MESSAGE("no private LAN address on this host: " << started.message());
        CHECK(started.message().find("--bind 127.0.0.1") != std::string::npos);
    }
}
