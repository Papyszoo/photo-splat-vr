// ModelService over a real ModelManager with a fake IHttpFetcher and the fake
// converter from tests/model_manager_test.cpp: the licence gate, download
// progress, cancellation (the .part survives), the free-space refusal, selection
// persistence, the legacy GGUF and Delete.
//
// The fetcher here is in-memory (no HTTP server): the wire protocol is already
// covered by model_manager_test.cpp, and a blocking/stepped transfer is what the
// progress and cancel cases need.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "model/checkpoint.h"
#include "model/da2_profile.h"
#include "model/gguf_writer.h"
#include "model/license.h"
#include "model/model_manager.h"
#include "model/sha256.h"
#include "test_util.h"
#include "ui/download_eta.h"
#include "ui/model_service.h"
#include "ui/ui_state.h"

namespace {

using splat::ConvertStats;
using splat::FetchRequest;
using splat::HttpResource;
using splat::IHttpFetcher;
using splat::ModelBackendSpec;
using splat::ModelManager;
using splat::ModelManagerOptions;
using splat::ModelRecord;
using splat::ModelService;
using splat::ModelServiceOptions;
using splat::ModelState;
using splat::Path;
using splat::Progress;
using splat::Result;
using splat::Sha256;
using splat::Status;
using splat::UiState;

const char kPayloadUrl[] = "http://test/model.bin";
const char kLicenceUrl[] = "http://test/LICENSE_MODEL";

std::string make_payload(size_t size) {
    std::string payload(size, '\0');
    uint32_t state = 0x12345678u;
    for (size_t index = 0; index < size; ++index) {
        state = state * 1664525u + 1013904223u;
        payload[index] = static_cast<char>(state >> 24);
    }
    return payload;
}

std::string sha256_of(const std::string& text) { return Sha256::of_hex(text.data(), text.size()); }

void write_file(const Path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
}

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

// In-memory fetcher. block_after > 0 stops the transfer after that many bytes
// until release is set (progress test) or a cancel arrives (cancel test).
// abort() is the model service's stall watchdog reaching a blocked transfer, as
// the console's sceHttp fetch does it: the reason decides the message.
class MemoryFetcher final : public IHttpFetcher {
public:
    std::map<std::string, std::string> files;
    std::string licence = "MODEL LICENCE\nresearch use only\nthis text is not stored in the repo\n";
    std::string licence_url = kLicenceUrl;
    std::atomic<uint64_t> block_after{0};
    std::atomic<bool> release{false};
    std::atomic<bool> blocked{false};
    // A blocked transfer that keeps reporting the bytes it already has, like the
    // segmented download's 100 ms poll: the stall line is read from exactly
    // that (ui_model_service logs it).
    bool report_while_blocked = false;
    std::atomic<int> head_calls{0};
    std::atomic<int> abort_calls{0};
    std::atomic<int> abort_reason{0};  // int(FetchAbort) + 1, 0 before any abort
    // Who ran abort(): never the render thread. Atomic because the watchdog can
    // still be writing it when the test reads it (a delayed abort after the
    // transfer stopped), and a plain id there is a data race.
    std::atomic<std::thread::id> abort_thread{};
    std::atomic<bool> aborted{false};
    // False: abort() records the call but does not end a blocked transfer, like
    // a console sceHttp call that ignores sceHttpAbortRequest and whose reading
    // thread never returns to poll `cancelled`.
    bool honor_abort = true;
    bool honor_cancel = true;
    // The licence fetch: block on the worker until released, so the test can see
    // that the caller of licence_text() never waits for it.
    std::atomic<bool> block_text{false};
    std::atomic<bool> text_blocked{false};
    std::atomic<bool> release_text{false};

    void abort(splat::FetchAbort reason) override {
        abort_thread.store(std::this_thread::get_id());
        ++abort_calls;
        abort_reason.store(static_cast<int>(reason) + 1);
        if (!honor_abort) return;
        aborted.store(true);
        release.store(true);  // wake the blocked loop like sceHttpAbortRequest does
    }

    Result<HttpResource> head(const std::string& url) override {
        ++head_calls;
        const auto found = files.find(url);
        if (found == files.end()) return Result<HttpResource>::failure("no such url: " + url);
        HttpResource resource;
        resource.content_length = found->second.size();
        resource.range_supported = true;
        return Result<HttpResource>::success(resource);
    }

    Status download(const FetchRequest& request) override {
        aborted.store(false);
        abort_reason.store(0);
        const auto found = files.find(request.url);
        if (found == files.end()) return Status::error("no such url: " + request.url);
        const std::string& payload = found->second;
        if (request.offset > payload.size()) return Status::error("offset past the payload");

        std::error_code error;
        std::filesystem::create_directories(request.destination.parent_path(), error);
        std::FILE* file = std::fopen(request.destination.string().c_str(),
                                     request.offset > 0 ? "ab" : "wb");
        if (file == nullptr) return Status::error("cannot open the test destination");

        constexpr size_t kChunk = 256;
        uint64_t written = 0;
        for (size_t at = static_cast<size_t>(request.offset); at < payload.size(); at += kChunk) {
            if (aborted.load()) {
                std::fclose(file);
                return abort_status();
            }
            if (honor_cancel && request.cancelled && request.cancelled()) {
                std::fclose(file);
                return Status::error("download cancelled");
            }
            const size_t take = std::min(kChunk, payload.size() - at);
            if (std::fwrite(payload.data() + at, 1, take, file) != take) {
                std::fclose(file);
                return Status::error("write failed");
            }
            if (request.hash != nullptr) request.hash->update(payload.data() + at, take);
            written += take;
            if (request.written != nullptr) request.written->store(written);
            if (request.progress != nullptr && request.total_bytes > 0) {
                request.progress->report(static_cast<float>(
                                             static_cast<double>(request.offset + written) /
                                             static_cast<double>(request.total_bytes)),
                                         "downloading");
            }
            const uint64_t limit = block_after.load();
            if (limit > 0 && request.offset + written >= limit) {
                blocked.store(true);
                while (!release.load()) {
                    if (aborted.load()) {
                        std::fclose(file);
                        return abort_status();
                    }
                    if (honor_cancel && request.cancelled && request.cancelled()) {
                        std::fclose(file);
                        return Status::error("download cancelled");
                    }
                    // The bytes did not move; the progress sink still hears from
                    // the transfer, as the segmented download's poll does.
                    if (report_while_blocked && request.progress != nullptr &&
                        request.total_bytes > 0) {
                        request.progress->report(
                            static_cast<float>(static_cast<double>(request.offset + written) /
                                               static_cast<double>(request.total_bytes)),
                            "downloading");
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                if (aborted.load()) {
                    std::fclose(file);
                    return abort_status();
                }
                block_after.store(0);
            }
        }
        std::fclose(file);
        return Status::success();
    }

    Result<std::string> get_text(const std::string& url, size_t max_bytes) override {
        if (url != licence_url) return Result<std::string>::failure("no such url: " + url);
        if (block_text.load()) {
            text_blocked.store(true);
            while (block_text.load() && !release_text.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            text_blocked.store(false);
        }
        if (licence.size() > max_bytes) return Result<std::string>::failure("too large");
        return Result<std::string>::success(licence);
    }

private:
    Status abort_status() const {
        if (abort_reason.load() == static_cast<int>(splat::FetchAbort::stalled) + 1) {
            return Status::error("the transfer stopped making progress and was aborted");
        }
        return Status::error("download cancelled");
    }
};

// Same fake GGUF writer as model_manager_test.cpp: the tiny runtime file carries
// exactly the metadata ModelManager verifies.
Result<ConvertStats> write_fake_gguf(const ModelBackendSpec& spec, const Path& out,
                                     Progress& progress) {
    splat::GgufWriter writer;
    std::vector<splat::GgufTensorDesc> tensors;
    tensors.push_back({"weight", {4}, false});
    Status status = writer.begin(out, std::move(tensors));
    if (!status.ok()) return Result<ConvertStats>::failure(status.message());
    writer.set_string("general.architecture", spec.gguf_architecture);
    writer.set_string(spec.name + ".backend", spec.name);
    writer.set_string(spec.name + ".source_sha256", spec.source_sha256);
    writer.set_u32(spec.name + ".converter_version", spec.converter_version);
    status = writer.write_header();
    if (!status.ok()) return Result<ConvertStats>::failure(status.message());
    const float values[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    status = writer.write_tensor_bytes(values, sizeof(values));
    if (!status.ok()) return Result<ConvertStats>::failure(status.message());
    status = writer.finish();
    if (!status.ok()) return Result<ConvertStats>::failure(status.message());
    progress.report(1.0f, "converted");
    ConvertStats stats;
    stats.tensor_count = 1;
    return Result<ConvertStats>::success(stats);
}

ModelManagerOptions::Converter fake_converter() {
    return [](const ModelBackendSpec& spec, const Path&, const Path& out, const std::string&,
              Progress& progress) { return write_fake_gguf(spec, out, progress); };
}

ModelBackendSpec test_spec(const std::string& name, const std::string& payload) {
    ModelBackendSpec spec = name == "da2" ? splat::da2_model_spec() : splat::sharp_model_spec();
    spec.source_filename = "model.bin";
    spec.source_url = kPayloadUrl;
    spec.source_sha256 = sha256_of(payload);
    spec.source_bytes = payload.size();
    if (name == "sharp") spec.license_url = kLicenceUrl;
    return spec;
}

struct Fixture {
    splat_test::TempDir temp{"ui-model-service"};
    std::string payload = make_payload(4096);
    MemoryFetcher fetcher;
    ModelManager manager;
    UiState state;
    ModelServiceOptions options;

    Fixture() : manager(make_options()) {
        fetcher.files[kPayloadUrl] = payload;
        options.state_dir = temp.path();
    }

    ModelManagerOptions make_options() {
        fetcher.files[kPayloadUrl] = payload;
        ModelManagerOptions options;
        options.state_dir = temp.path();
        options.fetcher = &fetcher;
        options.specs = {test_spec("sharp", payload), test_spec("da2", payload)};
        options.converter = fake_converter();
        return options;
    }
};

// Polls a predicate for up to `seconds`; returns its final value.
template <typename Predicate>
bool wait_until(Predicate predicate, double seconds = 10.0) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

const splat::ModelView* find_view(const std::vector<splat::ModelView>& views,
                                  const std::string& id) {
    for (const splat::ModelView& view : views) {
        if (view.id == id) return &view;
    }
    return nullptr;
}

// licence_text() starts the fetch on ModelService's worker and reports loading
// until it arrives; waits for the final text (or the failure message).
std::string wait_for_licence(ModelService& service, const std::string& id, double seconds = 10.0) {
    std::string text;
    const bool done = wait_until([&] {
        text = service.licence_text(id);
        return text.rfind(splat::kLicenceLoadingPrefix, 0) != 0;
    }, seconds);
    REQUIRE_MESSAGE(done, "the licence fetch did not finish: " << text);
    return text;
}

double seconds_since(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

}  // namespace

TEST_CASE("ui_model_service ui_state round trips, tolerates garbage and missing files") {
    splat_test::TempDir temp("ui-state");
    CHECK(splat::load_ui_state(temp.path()).selected_model.empty());  // no file
    CHECK(splat::load_ui_state(temp.path()).tooltips);

    UiState state;
    state.selected_model = "sharp";
    state.last_album = "2026-10-26";
    state.tooltips = false;
    REQUIRE_MESSAGE(splat::save_ui_state(temp.path(), state).ok(), "save");
    const UiState loaded = splat::load_ui_state(temp.path());
    CHECK(loaded.selected_model == "sharp");
    CHECK(loaded.last_album == "2026-10-26");
    CHECK_FALSE(loaded.tooltips);
    CHECK_FALSE(std::filesystem::exists(splat::ui_state_path(temp.path()).string() + ".tmp"));

    // Garbage, a JSON array and wrong field types all fall back to the defaults.
    write_file(splat::ui_state_path(temp.path()), "not json at all {{{");
    CHECK(splat::load_ui_state(temp.path()).selected_model.empty());
    write_file(splat::ui_state_path(temp.path()), "[1, 2, 3]");
    CHECK(splat::load_ui_state(temp.path()).selected_model.empty());
    write_file(splat::ui_state_path(temp.path()),
               "{\"selected_model\": 7, \"last_album\": \"x\", \"tooltips\": \"no\"}");
    const UiState partial = splat::load_ui_state(temp.path());
    CHECK(partial.selected_model.empty());
    CHECK(partial.last_album == "x");
    CHECK(partial.tooltips);

    // A directory where the file should be is not an error either.
    splat_test::TempDir other("ui-state-dir");
    std::filesystem::create_directories(splat::ui_state_path(other.path()));
    CHECK(splat::load_ui_state(other.path()).selected_model.empty());
}

TEST_CASE("ui_model_service lists the pinned models in order") {
    Fixture fixture;
    ModelService service(fixture.manager, fixture.state, fixture.options);
    const std::vector<splat::ModelView> views = service.list();
    REQUIRE(views.size() == 2);

    CHECK(views[0].id == "sharp");
    CHECK(views[0].name == "SHARP");
    CHECK(views[0].blurb == "Recommended, best quality");
    CHECK(views[0].licence_name == "Apple research");
    CHECK(views[0].gated);
    CHECK(views[0].offered);
    CHECK(views[0].state == "license_required");
    CHECK(views[0].size_bytes == fixture.payload.size());
    CHECK_FALSE(views[0].selected);

    CHECK(views[1].id == "da2");
    CHECK(views[1].name == "DA2-Small");
    CHECK(views[1].blurb == "Fast, approximate scene size");
    CHECK(views[1].licence_name == "Apache-2.0");
    CHECK_FALSE(views[1].gated);
    CHECK(views[1].state == "accepted");  // ungated, nothing downloaded yet
    CHECK(service.selected().empty());
}

TEST_CASE("ui_model_service list() is a cached snapshot, not a state-directory read") {
    // The render thread's list() must not open, stat or parse anything: the
    // SHARP slow report's §3.3 measured ~40 /data calls per frame from this
    // path, and each cost tens of ms at the page-cache wall.
    Fixture fixture;
    const Path legacy_dir = fixture.temp.path() / "legacy-models";
    std::filesystem::create_directories(legacy_dir);
    const Path legacy = legacy_dir / "da2.gguf";
    {
        Progress progress;
        const Result<ConvertStats> written =
            write_fake_gguf(test_spec("da2", fixture.payload), legacy, progress);
        REQUIRE_MESSAGE(written.ok(), written.message());
    }
    ModelServiceOptions options = fixture.options;
    options.legacy_models_dir = legacy_dir;
    options.cache_refresh_seconds = 3600.0;  // no worker refresh behind the test's back
    ModelService service(fixture.manager, fixture.state, options);

    std::vector<splat::ModelView> views = service.list();
    const splat::ModelView* da2 = find_view(views, "da2");
    REQUIRE(da2 != nullptr);
    REQUIRE(da2->state == "ready");

    // Remove every file that snapshot came from. list() still serves the
    // snapshot (it lags until the worker refreshes), and it does not fail or
    // block on the missing disk. A rebuild here would drop the legacy row.
    std::filesystem::remove_all(fixture.temp.path());
    service.tick(1.0);
    views = service.list();
    da2 = find_view(views, "da2");
    REQUIRE(da2 != nullptr);
    CHECK(da2->state == "ready");
    CHECK(contains(da2->message, "models/da2.gguf"));
}

TEST_CASE("ui_model_service refreshes the row cache on the worker side") {
    // The other half of the contract: the flag tick() sets is drained by the
    // stall-watch thread, so an external change does become visible - without
    // the render thread ever reading the state directory.
    Fixture fixture;
    const Path legacy_dir = fixture.temp.path() / "legacy-models";
    std::filesystem::create_directories(legacy_dir);
    const Path legacy = legacy_dir / "da2.gguf";
    {
        Progress progress;
        REQUIRE_MESSAGE(write_fake_gguf(test_spec("da2", fixture.payload), legacy, progress).ok(),
                        "gguf");
    }
    ModelServiceOptions options = fixture.options;
    options.legacy_models_dir = legacy_dir;
    options.cache_refresh_seconds = 0.0;
    ModelService service(fixture.manager, fixture.state, options);
    REQUIRE(find_view(service.list(), "da2")->state == "ready");

    write_file(legacy, "not a gguf");
    service.tick(0.3);
    REQUIRE(wait_until([&] {
        const std::vector<splat::ModelView> views = service.list();
        const splat::ModelView* da2 = find_view(views, "da2");
        return da2 != nullptr && da2->state != "ready";
    }));
}

TEST_CASE("ui_model_service gates SHARP on the licence text shown") {
    Fixture fixture;
    ModelService service(fixture.manager, fixture.state, fixture.options);

    const Status blocked = service.download("sharp");
    REQUIRE_FALSE(blocked.ok());
    CHECK(contains(blocked.message(), "licence"));
    CHECK_FALSE(service.downloading());
    CHECK(fixture.fetcher.head_calls == 0);  // nothing touched the network

    // Nothing can be accepted before its text was shown.
    CHECK_FALSE(service.accept("sharp").ok());
    CHECK(wait_for_licence(service, "sharp") == fixture.fetcher.licence);
    CHECK(service.selected().empty());

    // Accepting an ungated model is refused.
    CHECK_FALSE(service.accept("da2").ok());
    REQUIRE(service.accept("sharp").ok());
    CHECK(fixture.manager.license_accepted("sharp"));

    const Status started = service.download("sharp");
    REQUIRE_MESSAGE(started.ok(), started.message());
    service.wait_for_idle();

    const std::vector<splat::ModelView> ready_views = service.list();
    const splat::ModelView* sharp = find_view(ready_views, "sharp");
    REQUIRE(sharp != nullptr);
    CHECK(sharp->state == "ready");
    CHECK(sharp->fraction == doctest::Approx(1.0f));
    CHECK(std::filesystem::exists(fixture.temp.path() / "models" / "sharp.gguf"));
    CHECK(fixture.manager.license_accepted("sharp"));

    // The licence fetch failure carries a clear message, not an empty string.
    MemoryFetcher offline;
    ModelManagerOptions options;
    options.state_dir = fixture.temp.path() / "offline";
    options.fetcher = nullptr;
    options.specs = {test_spec("sharp", fixture.payload)};
    options.converter = fake_converter();
    ModelManager manager(std::move(options));
    UiState state;
    ModelServiceOptions offline_options;
    offline_options.state_dir = fixture.temp.path() / "offline";
    ModelService offline_service(manager, state, offline_options);
    const std::string text = wait_for_licence(offline_service, "sharp");
    CHECK(text.rfind(splat::kLicenceUnavailablePrefix, 0) == 0);
    CHECK(contains(text, "no HTTP fetcher"));
    CHECK_FALSE(offline_service.accept("sharp").ok());
}

TEST_CASE("ui_model_service publishes download progress from the worker") {
    Fixture fixture;
    ModelService service(fixture.manager, fixture.state, fixture.options);
    fixture.fetcher.block_after.store(fixture.payload.size() / 4);

    REQUIRE_MESSAGE(service.download("da2").ok(), "start");
    REQUIRE(wait_until([&] { return fixture.fetcher.blocked.load(); }));
    REQUIRE(wait_until([&] {
        const std::vector<splat::ModelView> views = service.list();
        const splat::ModelView* view = find_view(views, "da2");
        return view != nullptr && view->state == "downloading";
    }));

    const std::vector<splat::ModelView> progress_views = service.list();
    const splat::ModelView* downloading = find_view(progress_views, "da2");
    REQUIRE(downloading != nullptr);
    CHECK(downloading->state == "downloading");
    // The manager maps the transfer to 0.05 .. 0.70, so a quarter of the bytes
    // is about 0.21 on the shared bar.
    CHECK(downloading->fraction > 0.15f);
    CHECK(downloading->fraction < 0.30f);
    CHECK(service.downloading());
    CHECK(service.downloading_id() == "da2");

    fixture.fetcher.release.store(true);
    service.wait_for_idle();
    const std::vector<splat::ModelView> ready_views = service.list();
    const splat::ModelView* ready = find_view(ready_views, "da2");
    REQUIRE(ready != nullptr);
    CHECK(ready->state == "ready");
    CHECK(ready->fraction == doctest::Approx(1.0f));
    CHECK_FALSE(service.downloading());
}

TEST_CASE("ui_model_service logs a download's start, stall and end") {
    // The klog lines are the only way an unattended console run can tell "no
    // bytes arrived" from "the panel did not repaint" (the 0.6.1 stall).
    Fixture fixture;
    std::mutex lines_mutex;
    std::vector<std::string> lines;
    fixture.options.log = [&](const std::string& text) {
        std::lock_guard<std::mutex> lock(lines_mutex);
        lines.push_back(text);
    };
    // Ten seconds per progress report: one line per report.
    double now = 0.0;
    fixture.options.clock = [&now] { return now += 10.0; };
    ModelService service(fixture.manager, fixture.state, fixture.options);
    fixture.fetcher.block_after.store(fixture.payload.size() / 4);
    fixture.fetcher.report_while_blocked = true;

    const auto copy_lines = [&] {
        std::lock_guard<std::mutex> lock(lines_mutex);
        return lines;
    };

    REQUIRE_MESSAGE(service.download("da2").ok(), "start");
    REQUIRE(wait_until([&] { return fixture.fetcher.blocked.load(); }));
    REQUIRE(wait_until([&] {
        for (const std::string& line : copy_lines()) {
            if (line.find("no progress") != std::string::npos) return true;
        }
        return false;
    }));
    const std::vector<std::string> logged = copy_lines();
    REQUIRE(logged.size() >= 2);
    CHECK(logged[0].find("model download: da2 started") != std::string::npos);
    CHECK(logged[1].find("model download: da2 ") != std::string::npos);
    CHECK(logged[1].find("%") != std::string::npos);
    // The blocked transfer keeps reporting the bytes it already has, and the
    // line says the bytes did not move.
    CHECK(logged.back().find("no progress") != std::string::npos);

    fixture.fetcher.release.store(true);
    service.wait_for_idle();
    const std::vector<std::string> after = copy_lines();
    REQUIRE_FALSE(after.empty());
    CHECK(after.back().find("model download: da2 finished") != std::string::npos);
}

TEST_CASE("ui_model_service never says no progress while verifying or converting") {
    // D3: only the transfer has bytes. Verify and convert report a fraction with
    // the file already complete, and used to append ", no progress" - which a
    // klog reads as a stalled transfer while the converter is working.
    splat_test::TempDir temp("ui-model-convert-log");
    const std::string payload = make_payload(4096);
    MemoryFetcher fetcher;
    fetcher.files[kPayloadUrl] = payload;

    ModelManagerOptions manager_options;
    manager_options.state_dir = temp.path();
    manager_options.fetcher = &fetcher;
    manager_options.specs = {test_spec("da2", payload)};
    // A converter that reports like the real streaming one does while it works.
    manager_options.converter = [](const ModelBackendSpec& spec, const Path&, const Path& out,
                                   const std::string&, Progress& progress) {
        for (int step = 1; step <= 4; ++step) {
            progress.report(0.25f * static_cast<float>(step), "converting");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return write_fake_gguf(spec, out, progress);
    };
    ModelManager manager(std::move(manager_options));

    UiState state;
    std::mutex lines_mutex;
    std::vector<std::string> lines;
    ModelServiceOptions options;
    options.state_dir = temp.path();
    options.log = [&](const std::string& text) {
        std::lock_guard<std::mutex> lock(lines_mutex);
        lines.push_back(text);
    };
    // Every progress report is ten seconds after the last, so each verify and
    // convert report takes a log line.
    double now = 0.0;
    options.clock = [&now] { return now += 10.0; };
    ModelService service(manager, state, options);

    REQUIRE_MESSAGE(service.download("da2").ok(), "download");
    service.wait_for_idle();

    std::vector<std::string> copy;
    {
        std::lock_guard<std::mutex> lock(lines_mutex);
        copy = lines;
    }
    REQUIRE_FALSE(copy.empty());
    CHECK(copy.back().find("model download: da2 finished") != std::string::npos);
    // The conversion's reports are in the log (a complete file), and none of
    // them claims a stalled transfer.
    bool converted_line = false;
    for (const std::string& line : copy) {
        CHECK_MESSAGE(line.find("no progress") == std::string::npos, line);
        if (line.find("(4096 of 4096 bytes)") != std::string::npos) converted_line = true;
    }
    CHECK(converted_line);
}

TEST_CASE("ui_model_service aborts a transfer that stops moving") {
    // D1: a transfer whose bytes stop moving is aborted from another thread (the
    // transfer itself is blocked and cannot notice), and the row reaches Failed
    // through the existing manager failure path, with the .part kept for retry.
    Fixture fixture;
    fixture.options.stall_timeout_seconds = 0.5;
    ModelService service(fixture.manager, fixture.state, fixture.options);
    fixture.fetcher.block_after.store(fixture.payload.size() / 4);

    REQUIRE_MESSAGE(service.download("da2").ok(), "start");
    REQUIRE(wait_until([&] { return fixture.fetcher.blocked.load(); }));
    REQUIRE(wait_until(
        [&] {
            const std::vector<splat::ModelView> views = service.list();
            const splat::ModelView* view = find_view(views, "da2");
            return view != nullptr && view->state == "failed";
        },
        10.0));

    CHECK(fixture.fetcher.abort_calls.load() >= 1);
    CHECK(fixture.fetcher.abort_reason.load() ==
          static_cast<int>(splat::FetchAbort::stalled) + 1);
    CHECK_FALSE(service.downloading());
    const Result<ModelRecord> record = fixture.manager.status("da2");
    REQUIRE(record.ok());
    CHECK(record.value().state == ModelState::Failed);
    CHECK(contains(record.value().message, "progress"));
    CHECK(std::filesystem::exists(fixture.temp.path() / "downloads" / "model.bin.part"));

    // The existing retry path starts again from the .part.
    fixture.fetcher.block_after.store(0);
    fixture.fetcher.aborted.store(false);
    REQUIRE_MESSAGE(service.download("da2").ok(), "retry");
    service.wait_for_idle();
    const std::vector<splat::ModelView> views = service.list();
    const splat::ModelView* view = find_view(views, "da2");
    REQUIRE(view != nullptr);
    CHECK(view->state == "ready");
}

TEST_CASE("ui_model_service bounded stop gives up on a read that ignores abort") {
    // The console failure this pass bounds: the transfer sits inside a sceHttp
    // read that sceHttpAbortRequest does not unblock. ModelService::stop() must
    // return false in about 2 s, and the caller then leaves the service (and
    // the manager and fetcher it uses) alone - the process is ending.
    Fixture fixture;
    fixture.fetcher.honor_abort = false;
    fixture.fetcher.honor_cancel = false;
    fixture.options.stall_timeout_seconds = 0.0;  // the watchdog is not what ends it
    std::mutex lines_mutex;
    std::vector<std::string> lines;
    fixture.options.log = [&](const std::string& text) {
        std::lock_guard<std::mutex> lock(lines_mutex);
        lines.push_back(text);
    };
    fixture.fetcher.block_after.store(fixture.payload.size() / 4);

    // Intentionally never destroyed: stop() said a thread still uses it.
    auto* service = new ModelService(fixture.manager, fixture.state, fixture.options);
    REQUIRE_MESSAGE(service->download("da2").ok(), "start");
    REQUIRE(wait_until([&] { return fixture.fetcher.blocked.load(); }));

    const auto started = std::chrono::steady_clock::now();
    const bool stopped = service->stop();
    const double seconds = seconds_since(started);
    INFO("the bounded stop took " << seconds << " s");
    CHECK_FALSE(stopped);
    CHECK_MESSAGE(seconds < 3.0, "the bounded stop took " << seconds << " s");
    CHECK_MESSAGE(seconds >= 1.5, "the stop did not use its bound: " << seconds);
    // The abort was attempted but the transfer ignored it; the host's one line
    // says what is still running (the service does not free anything).
    CHECK(fixture.fetcher.abort_calls.load() >= 1);
    CHECK(service->running_text().find("download") != std::string::npos);
    CHECK(service->downloading());

    // Let the worker finish; its last log line is written after the manager is
    // done with it, so the fixture can go (the service itself stays leaked,
    // like the panel on the console's quit path).
    fixture.fetcher.release.store(true);
    const bool finished = wait_until([&] {
        std::lock_guard<std::mutex> lock(lines_mutex);
        for (const std::string& line : lines) {
            if (line.find("model download: da2 finished") != std::string::npos) return true;
        }
        return false;
    }, 15.0);
    CHECK(finished);
}

TEST_CASE("ui_model_service bounded stop joins a worker that honours it") {
    // The other half of the contract: a transfer that ends on the flag/abort is
    // stopped and joined inside the bound, so the host may destroy the runtime.
    Fixture fixture;
    fixture.fetcher.block_after.store(fixture.payload.size() / 4);
    ModelService service(fixture.manager, fixture.state, fixture.options);
    REQUIRE_MESSAGE(service.download("da2").ok(), "start");
    REQUIRE(wait_until([&] { return fixture.fetcher.blocked.load(); }));

    const auto started = std::chrono::steady_clock::now();
    const bool stopped = service.stop();
    const double seconds = seconds_since(started);
    INFO("the bounded stop took " << seconds << " s");
    CHECK(stopped);
    CHECK_MESSAGE(seconds < 2.0, "the bounded stop took " << seconds << " s");
    CHECK_FALSE(service.downloading());

    // A second stop is the destructor's: nothing is left to join.
    CHECK(service.stop());
    CHECK(service.running_text().empty());
}

TEST_CASE("ui_model_service bounded stop covers a blocked licence fetch") {
    // D1's licence half: the licence worker uses the same manager and fetcher,
    // so a fetch that is still inside them after the bound must make stop()
    // return false - the host then leaks the runtime instead of freeing them.
    Fixture fixture;
    fixture.fetcher.block_text.store(true);
    ModelServiceOptions options = fixture.options;

    auto* service = new ModelService(fixture.manager, fixture.state, options);
    REQUIRE(service->licence_text("sharp").rfind(splat::kLicenceLoadingPrefix, 0) == 0);
    REQUIRE(wait_until([&] { return fixture.fetcher.text_blocked.load(); }));

    const auto started = std::chrono::steady_clock::now();
    const bool stopped = service->stop();
    const double seconds = seconds_since(started);
    INFO("the bounded stop took " << seconds << " s");
    CHECK_FALSE(stopped);
    CHECK_MESSAGE(seconds < 3.0, "the bounded stop took " << seconds << " s");
    CHECK(service->running_text().find("licence") != std::string::npos);

    // Let the fetch finish; the service stays leaked, as the console's quit
    // path leaves the panel.
    fixture.fetcher.block_text.store(false);
    fixture.fetcher.release_text.store(true);
    CHECK(wait_until([&] { return service->licence_text("sharp") == fixture.fetcher.licence; }));
}

TEST_CASE("ui_model_service fetches a licence without blocking the caller") {
    // Item 3: the render thread must never wait on the fetcher's setup lock. The
    // licence text is fetched on a worker, so a download that is stuck (and a
    // licence server that does not answer) cannot freeze a frame.
    Fixture fixture;
    fixture.fetcher.block_after.store(fixture.payload.size() / 4);
    fixture.fetcher.block_text.store(true);
    ModelService service(fixture.manager, fixture.state, fixture.options);

    REQUIRE_MESSAGE(service.download("da2").ok(), "download");
    REQUIRE(wait_until([&] { return fixture.fetcher.blocked.load(); }));

    const auto first_started = std::chrono::steady_clock::now();
    const std::string loading = service.licence_text("sharp");
    const double first_seconds = seconds_since(first_started);
    INFO("the first licence_text() call returned in " << first_seconds << " s");
    CHECK(loading.rfind(splat::kLicenceLoadingPrefix, 0) == 0);
    CHECK_MESSAGE(first_seconds < 0.5, "licence_text blocked for " << first_seconds << " s");
    REQUIRE(wait_until([&] { return fixture.fetcher.text_blocked.load(); }));

    // Still non-blocking while the fetch is in flight, and nothing is accepted
    // before the exact text is shown.
    const auto second_started = std::chrono::steady_clock::now();
    CHECK(service.licence_text("sharp").rfind(splat::kLicenceLoadingPrefix, 0) == 0);
    const double second_seconds = seconds_since(second_started);
    CHECK_MESSAGE(second_seconds < 0.5, "licence_text blocked for " << second_seconds << " s");
    CHECK_FALSE(service.accept("sharp").ok());

    fixture.fetcher.block_text.store(false);
    fixture.fetcher.release_text.store(true);
    REQUIRE(wait_until(
        [&] { return service.licence_text("sharp") == fixture.fetcher.licence; }, 10.0));
    CHECK(service.accept("sharp").ok());

    fixture.fetcher.release.store(true);
    service.wait_for_idle();
}

TEST_CASE("ui_model_service shows the time left from the byte rate while downloading") {
    Fixture fixture;
    // A synthetic clock: every progress report is one minute after the last, so
    // the 4096-byte payload arrives at 256 bytes per minute.
    double now = 0.0;
    fixture.options.clock = [&now] { return now += 60.0; };
    ModelService service(fixture.manager, fixture.state, fixture.options);
    fixture.fetcher.block_after.store(fixture.payload.size() / 2);

    REQUIRE_MESSAGE(service.download("da2").ok(), "start");
    REQUIRE(wait_until([&] { return fixture.fetcher.blocked.load(); }));

    const std::vector<splat::ModelView> views = service.list();
    const splat::ModelView* downloading = find_view(views, "da2");
    REQUIRE(downloading != nullptr);
    CHECK(downloading->state == "downloading");
    // Half of the bytes are in and the other half takes as long again: 8 minutes.
    CHECK(downloading->eta_seconds == doctest::Approx(480.0).epsilon(0.05));
    CHECK(splat::eta_text(downloading->eta_seconds) == "about 8 min left");

    fixture.fetcher.release.store(true);
    service.wait_for_idle();
    const std::vector<splat::ModelView> after = service.list();
    const splat::ModelView* ready = find_view(after, "da2");
    REQUIRE(ready != nullptr);
    CHECK(ready->state == "ready");
    CHECK(ready->eta_seconds < 0.0);  // no estimate outside a download
}

TEST_CASE("ui_model_service cancel stops the fetcher and keeps the .part") {
    Fixture fixture;
    ModelService service(fixture.manager, fixture.state, fixture.options);
    fixture.fetcher.block_after.store(1536);  // six whole chunks
    // The transfer ignores the polled cancel flag, so only the watchdog's abort
    // can end it: cancel() itself must never enter the fetcher on this thread.
    fixture.fetcher.honor_cancel = false;

    REQUIRE_MESSAGE(service.download("da2").ok(), "start");
    REQUIRE(wait_until([&] { return fixture.fetcher.blocked.load(); }));

    const Status cancelled = service.cancel("da2");
    REQUIRE_MESSAGE(cancelled.ok(), cancelled.message());

    REQUIRE_MESSAGE(wait_until([&] { return !service.downloading(); }, 5.0),
                    "the cancel did not stop the transfer");
    service.wait_for_idle();
    CHECK(fixture.fetcher.abort_calls.load() >= 1);
    CHECK(fixture.fetcher.abort_reason.load() ==
          static_cast<int>(splat::FetchAbort::cancelled) + 1);
    // The abort ran on the watchdog thread, not on the caller of cancel().
    CHECK(fixture.fetcher.abort_thread.load() != std::this_thread::get_id());
    const Path part = fixture.temp.path() / "downloads" / "model.bin.part";
    REQUIRE(std::filesystem::exists(part));
    CHECK(std::filesystem::file_size(part) == 1536);
    CHECK_FALSE(std::filesystem::exists(fixture.temp.path() / "downloads" / "model.bin"));

    const Result<ModelRecord> record = fixture.manager.status("da2");
    REQUIRE(record.ok());
    CHECK(record.value().state == ModelState::Failed);
    CHECK(contains(record.value().message, "cancel"));

    const std::vector<splat::ModelView> views = service.list();
    const splat::ModelView* view = find_view(views, "da2");
    REQUIRE(view != nullptr);
    CHECK(view->state == "failed");
    CHECK(view->fraction > 0.35f);  // the paused part is still shown
    CHECK(contains(view->message, "cancel"));
    CHECK_FALSE(service.cancel("da2").ok());

    // The next download resumes from that part and finishes.
    fixture.fetcher.block_after.store(0);
    REQUIRE(service.download("da2").ok());
    service.wait_for_idle();
    const std::vector<splat::ModelView> resumed_views = service.list();
    const splat::ModelView* resumed = find_view(resumed_views, "da2");
    REQUIRE(resumed != nullptr);
    CHECK(resumed->state == "ready");
    CHECK(fixture.fetcher.head_calls >= 2);
}

TEST_CASE("ui_model_service refuses a download that does not fit") {
    splat_test::TempDir temp("ui-model-space");
    const std::string payload = make_payload(4096);
    MemoryFetcher fetcher;
    fetcher.files[kPayloadUrl] = payload;

    // The pinned SHARP size (the payload is a stand-in, no transfer happens):
    // 2.6 GiB source + 1.22 GiB GGUF + 256 MiB margin is about 4 GB.
    ModelBackendSpec sharp = test_spec("sharp", payload);
    sharp.source_bytes = splat::kSharpCheckpointBytes;
    ModelBackendSpec da2 = test_spec("da2", payload);

    ModelManagerOptions manager_options;
    manager_options.state_dir = temp.path();
    manager_options.fetcher = &fetcher;
    manager_options.specs = {sharp, da2};
    manager_options.converter = fake_converter();
    ModelManager manager(std::move(manager_options));

    UiState state;
    ModelServiceOptions options;
    options.state_dir = temp.path();
    options.free_bytes = [](const Path&) { return uint64_t{100} * 1024 * 1024; };
    ModelService service(manager, state, options);

    const Status da2_refused = service.download("da2");  // needs ~0.4 GB
    REQUIRE_FALSE(da2_refused.ok());
    CHECK(contains(da2_refused.message(), "needs ~"));
    CHECK(contains(da2_refused.message(), "free"));
    CHECK(contains(da2_refused.message(), "GB"));
    CHECK_FALSE(service.downloading());
    CHECK_FALSE(std::filesystem::exists(temp.path() / "downloads" / "model.bin.part"));
    CHECK(fetcher.head_calls == 0);

    // SHARP waits for the licence first, then quotes the ~4 GB it needs.
    REQUIRE_FALSE(service.download("sharp").ok());
    REQUIRE(wait_for_licence(service, "sharp") == fetcher.licence);
    REQUIRE(service.accept("sharp").ok());
    const Status sharp_refused = service.download("sharp");
    REQUIRE_FALSE(sharp_refused.ok());
    CHECK(contains(sharp_refused.message(), "needs ~4 GB free"));
    CHECK_FALSE(service.downloading());
    CHECK(fetcher.head_calls == 0);
}

TEST_CASE("ui_model_service persists the converter selection") {
    Fixture fixture;
    {
        ModelService service(fixture.manager, fixture.state, fixture.options);
        REQUIRE_MESSAGE(service.download("da2").ok(), "download");
        service.wait_for_idle();

        const Status refused = service.select("sharp");  // not ready
        REQUIRE_FALSE(refused.ok());
        CHECK(contains(refused.message(), "not ready"));
        CHECK(service.selected().empty());

        REQUIRE_MESSAGE(service.select("da2").ok(), "select");
        CHECK(service.selected() == "da2");
        const std::vector<splat::ModelView> first_views = service.list();
        const splat::ModelView* da2 = find_view(first_views, "da2");
        REQUIRE(da2 != nullptr);
        CHECK(da2->selected);
    }

    // A restart reads ui.json: the selection is not kept in the old instance.
    CHECK(splat::load_ui_state(fixture.temp.path()).selected_model == "da2");
    UiState restored = splat::load_ui_state(fixture.temp.path());
    ModelManagerOptions options;
    options.state_dir = fixture.temp.path();
    options.fetcher = &fixture.fetcher;
    options.specs = {test_spec("sharp", fixture.payload), test_spec("da2", fixture.payload)};
    options.converter = fake_converter();
    ModelManager manager(std::move(options));
    ModelService second(manager, restored, fixture.options);
    CHECK(second.selected() == "da2");
    const std::vector<splat::ModelView> second_views = second.list();
    const splat::ModelView* da2 = find_view(second_views, "da2");
    REQUIRE(da2 != nullptr);
    CHECK(da2->selected);

    // Delete clears the selection in the file too.
    REQUIRE_MESSAGE(second.remove("da2").ok(), "remove");
    CHECK(second.selected().empty());
    CHECK(splat::load_ui_state(fixture.temp.path()).selected_model.empty());
}

TEST_CASE("ui_model_service accepts a verified legacy GGUF and deletes it") {
    splat_test::TempDir temp("ui-model-legacy");
    std::string payload = make_payload(2048);
    MemoryFetcher fetcher;
    fetcher.files[kPayloadUrl] = payload;
    const ModelBackendSpec da2 = test_spec("da2", payload);

    ModelManagerOptions manager_options;
    manager_options.state_dir = temp.path();
    manager_options.fetcher = &fetcher;
    manager_options.specs = {da2};
    manager_options.converter = fake_converter();
    ModelManager manager(std::move(manager_options));

    const Path legacy_dir = temp.path() / "legacy-models";
    std::filesystem::create_directories(legacy_dir);
    const Path legacy = legacy_dir / "da2.gguf";
    Progress progress;
    const Result<ConvertStats> written_gguf = write_fake_gguf(da2, legacy, progress);
    REQUIRE_MESSAGE(written_gguf.ok(), written_gguf.message());

    UiState state;
    ModelServiceOptions options;
    options.state_dir = temp.path();
    options.legacy_models_dir = legacy_dir;
    ModelService service(manager, state, options);

    const std::vector<splat::ModelView> legacy_views = service.list();
    const splat::ModelView* view = find_view(legacy_views, "da2");
    REQUIRE(view != nullptr);
    CHECK(view->state == "ready");
    CHECK(contains(view->message, "models/da2.gguf"));
    CHECK(manager.status("da2").value().state != ModelState::Ready);  // the manager has no record

    REQUIRE_MESSAGE(service.select("da2").ok(), "select the legacy file");
    CHECK(service.selected() == "da2");

    // A corrupt legacy file does not count as ready any more. The refresh runs
    // on the worker side (tick only flags it), so wait for the published rows.
    write_file(legacy, "not a gguf");
    service.tick(0.3);  // ask the worker side to re-read the rows
    REQUIRE(wait_until([&] {
        const std::vector<splat::ModelView> views = service.list();
        const splat::ModelView* current = find_view(views, "da2");
        return current != nullptr && current->state != "ready";
    }));
    const std::vector<splat::ModelView> corrupt_views = service.list();
    view = find_view(corrupt_views, "da2");
    REQUIRE(view != nullptr);
    CHECK(view->state != "ready");

    // Delete removes the legacy file as well and keeps the (absent) licence in
    // place: DA2 needs no acceptance.
    REQUIRE_MESSAGE(service.remove("da2").ok(), "remove");
    CHECK_FALSE(std::filesystem::exists(legacy));
    CHECK(service.selected().empty());
}

TEST_CASE("ui_model_service legacy lookup only ever returns the backend's own file") {
    // The shared lookup (core/ui/model_service.h) that ModelService's Ready check
    // and the console converter (platform/ps5/viewer/ps5_jobs.cpp) both use. The
    // W19 review's bug: with da2.gguf absent, the converter used to fall through
    // to the resident SHARP weights.
    splat_test::TempDir temp("ui-model-legacy-lookup");
    const Path dir = temp.path() / "models";
    std::filesystem::create_directories(dir);

    CHECK(splat::legacy_model_path(dir, "sharp").empty());
    CHECK(splat::legacy_model_path(dir, "da2").empty());

    const Path f16 = dir / "sharp_2572gikvuh-f16-v1.gguf";
    write_file(f16, "not a real gguf, only the name matters here");
    CHECK(splat::legacy_model_path(dir, "sharp") == f16);
    // The same folder must never hand a da2 job SHARP's file.
    CHECK(splat::legacy_model_path(dir, "da2").empty());

    const Path da2 = dir / "da2.gguf";
    write_file(da2, "x");
    CHECK(splat::legacy_model_path(dir, "da2") == da2);
    CHECK(splat::legacy_model_path(dir, "sharp") == f16);  // sharp.gguf is preferred below

    const Path sharp = dir / "sharp.gguf";
    write_file(sharp, "x");
    CHECK(splat::legacy_model_path(dir, "sharp") == sharp);
    CHECK(splat::legacy_model_path(dir, "other").empty());
    CHECK(splat::legacy_model_path(Path{}, "sharp").empty());  // disabled
}

TEST_CASE("ui_model_service sees W18's SHARP names and never gives them to da2") {
    Fixture fixture;
    const Path legacy_dir = fixture.temp.path() / "legacy-models";
    std::filesystem::create_directories(legacy_dir);
    const Path legacy = legacy_dir / "sharp_2572gikvuh-f16-v1.gguf";
    Progress progress;
    const Result<ConvertStats> written =
        write_fake_gguf(test_spec("sharp", fixture.payload), legacy, progress);
    REQUIRE_MESSAGE(written.ok(), written.message());

    ModelServiceOptions options = fixture.options;
    options.legacy_models_dir = legacy_dir;
    ModelService service(fixture.manager, fixture.state, options);
    const std::vector<splat::ModelView> views = service.list();

    const splat::ModelView* sharp = find_view(views, "sharp");
    REQUIRE(sharp != nullptr);
    CHECK(sharp->state == "ready");
    CHECK(contains(sharp->message, "sharp_2572gikvuh-f16-v1.gguf"));

    // The Models screen used to show nothing for this file while the converter
    // resolved it; now both agree. DA2-Small is not ready just because SHARP's
    // weights sit in the folder.
    const splat::ModelView* da2 = find_view(views, "da2");
    REQUIRE(da2 != nullptr);
    CHECK(da2->state != "ready");
    CHECK(splat::resolved_model_path(fixture.manager, "sharp", legacy_dir) == legacy);
    CHECK(splat::resolved_model_path(fixture.manager, "da2", legacy_dir).empty());
}

TEST_CASE("ui_model_service resolved model path prefers the manager's Ready file") {
    Fixture fixture;
    ModelService service(fixture.manager, fixture.state, fixture.options);
    REQUIRE(wait_for_licence(service, "sharp") == fixture.fetcher.licence);
    REQUIRE(service.accept("sharp").ok());
    REQUIRE_MESSAGE(service.download("sharp").ok(), "download");
    service.wait_for_idle();
    REQUIRE(service.list()[0].state == "ready");

    const Path legacy_dir = fixture.temp.path() / "legacy-models";
    std::filesystem::create_directories(legacy_dir);
    write_file(legacy_dir / "sharp.gguf", "a legacy file that must not win");
    CHECK(splat::resolved_model_path(fixture.manager, "sharp", legacy_dir) ==
          fixture.manager.gguf_path("sharp"));
}

TEST_CASE("ui_model_service delete keeps the licence acceptance and clears the files") {
    Fixture fixture;
    ModelService service(fixture.manager, fixture.state, fixture.options);
    REQUIRE(wait_for_licence(service, "sharp") == fixture.fetcher.licence);
    REQUIRE(service.accept("sharp").ok());
    REQUIRE_MESSAGE(service.download("sharp").ok(), "download");
    service.wait_for_idle();
    REQUIRE_MESSAGE(service.select("sharp").ok(), "select");
    REQUIRE(std::filesystem::exists(fixture.temp.path() / "models" / "sharp.gguf"));

    // A paused part left around is removed too, with its segments file.
    write_file(fixture.temp.path() / "downloads" / "model.bin.part", "stale");
    write_file(fixture.temp.path() / "downloads" / "model.bin.part.segments", "{}");

    REQUIRE_MESSAGE(service.remove("sharp").ok(), "remove");
    CHECK_FALSE(std::filesystem::exists(fixture.temp.path() / "models" / "sharp.gguf"));
    CHECK_FALSE(std::filesystem::exists(fixture.temp.path() / "downloads" / "model.bin"));
    CHECK_FALSE(std::filesystem::exists(fixture.temp.path() / "downloads" / "model.bin.part"));
    CHECK_FALSE(std::filesystem::exists(fixture.temp.path() / "downloads" / "model.bin.part.segments"));
    CHECK_FALSE(std::filesystem::exists(fixture.temp.path() / "models" / "sharp.json"));
    CHECK(fixture.manager.license_accepted("sharp"));  // the acceptance survives
    CHECK(service.selected().empty());

    const std::vector<splat::ModelView> after_delete = service.list();
    const splat::ModelView* sharp = find_view(after_delete, "sharp");
    REQUIRE(sharp != nullptr);
    CHECK(sharp->state == "accepted");  // licence accepted, nothing downloaded

    // A running download is not deleted underneath the worker.
    fixture.fetcher.block_after.store(1024);
    REQUIRE(service.download("da2").ok());
    REQUIRE(wait_until([&] { return fixture.fetcher.blocked.load(); }));
    const Status busy = service.remove("da2");
    CHECK_FALSE(busy.ok());
    CHECK(contains(busy.message(), "cancel"));
    CHECK(contains(busy.message(), "DA2-Small"));  // the busy model

    // Deleting a different model while that one downloads names both: the
    // download to cancel first and the model the user tried to delete.
    const Status other = service.remove("sharp");
    CHECK_FALSE(other.ok());
    CHECK(contains(other.message(), "DA2-Small"));
    CHECK(contains(other.message(), "SHARP"));
    CHECK(other.message().find("cancel the SHARP download") == std::string::npos);
    CHECK(service.downloading());  // the refused delete left the transfer running

    REQUIRE(service.cancel("da2").ok());
    service.wait_for_idle();
}
