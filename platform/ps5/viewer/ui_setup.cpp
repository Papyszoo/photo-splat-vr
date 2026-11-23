// The console's models chain: the C1 ModelManager over the app's state
// directory, the streaming converter from core/model and W3's sceHttp fetcher,
// wrapped in the W13 ModelService adapter (W21) - assembled inside the panel
// runtime the TV and the VR path share (W16). See ui_setup.h.
//
// This is the same chain tools/model_cli/main.cpp builds on the desktop, with
// the console's own fetcher instead of libcurl. Two differences, both for the
// console: the converter is passed explicitly so the peak RSS of a conversion is
// logged, and the legacy /data/ps5-splat-vr/models GGUF folder is accepted as a
// ready model in place (the layout before the C1 manager owned state/).
//
// make_ui_runtime() also builds the W23 capture service: the same panel, on
// both modes, gets the Photos screen's Captures tab and the helper it starts
// from /app0/helper/splat-helper.elf (nothing to install on the console).
//
// Memory: converting SHARP (2.6 GiB .pt -> ~1.2 GiB GGUF) must not load the
// checkpoint. core/model/converter.cpp streams one tensor at a time (1 MiB
// chunks straight from the file in zip-offset order, fp16 serialised into the
// GGUF writer) and sha256_file()/the download hash in 1 MiB chunks, so the peak
// stays a little above the viewer's own footprint; the ru_maxrss lines around
// the conversion are the measurement that keeps it that way.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_setup.h"

#include <sys/resource.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "model/converter.h"
#include "model/da2_profile.h"
#include "net_init.h"
#include "title_common.h"
#include "ui/screens/registry.h"
#include "ui/visibility_setup.h"
#include "vfs/io.h"

namespace viewer {
namespace {

using splat::ConvertOptions;
using splat::ConvertStats;
using splat::ModelBackendSpec;
using splat::Path;
using splat::Progress;
using splat::Result;

// ru_maxrss is the process high-water mark in KiB on this platform. The
// difference across one conversion is the converter's own peak (the value never
// shrinks, so a later lower reading would just mean "the viewer was already
// bigger").
uint64_t peak_rss_kib() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
    return static_cast<uint64_t>(usage.ru_maxrss);
}

std::string mib_text(uint64_t kib) {
    char text[32];
    std::snprintf(text, sizeof(text), "%llu MiB",
                  static_cast<unsigned long long>(kib / 1024ull));
    return text;
}

// The manager's converter seam (ModelManagerOptions::Converter) with the C1
// profiles the CLI uses: convert_checkpoint for SHARP's torch zip, the
// safetensors profile for DA2-Small. Called on the ModelService worker thread
// once the checkpoint has been verified; `source_sha256` is that verification's
// digest, so the converter does not read the 2.8 GB file again to hash it.
Result<ConvertStats> convert_backend(const ModelBackendSpec& spec, const Path& source,
                                     const Path& out, const std::string& source_sha256,
                                     Progress& progress) {
    const uint64_t before = peak_rss_kib();
    const std::string before_text = before == 0 ? std::string("unknown") : mib_text(before);
    line("viewer: models: converting %s, %s -> %s, peak RSS %s before", spec.name.c_str(),
         source.string().c_str(), out.string().c_str(), before_text.c_str());

    ConvertOptions options;
    options.backend = spec.name;
    options.source_sha256 = source_sha256;
    // The converter's own ~5 s line with MB/s read and written: the half of the
    // "Preparing" phase the download's net lines could not show.
    options.log = [](const std::string& text) { line("%s", text.c_str()); };
    Result<ConvertStats> converted =
        spec.name == "da2" ? splat::convert_depth_anything_v2(source, out, options, progress)
                           : splat::convert_checkpoint(source, out, options, progress);

    const uint64_t after = peak_rss_kib();
    const std::string after_text = after == 0 ? std::string("unknown") : mib_text(after);
    if (converted.ok()) {
        const ConvertStats& stats = converted.value();
        const uint64_t grew = after > before ? after - before : 0;
        const std::string grew_text = after == 0 ? std::string("unknown") : mib_text(grew);
        line("viewer: models: converted %s in %.1f s, %llu tensor(s), %llu bytes, %.2f MB/s, peak "
             "RSS %s (+%s)",
             spec.name.c_str(), stats.seconds,
             static_cast<unsigned long long>(stats.tensor_count),
             static_cast<unsigned long long>(stats.gguf_bytes),
             stats.seconds > 0.0 ? static_cast<double>(stats.gguf_bytes) / stats.seconds / 1e6 : 0.0,
             after_text.c_str(), grew_text.c_str());
    } else {
        line("viewer: models: converting %s failed: %s (peak RSS %s)", spec.name.c_str(),
             converted.message().c_str(), after_text.c_str());
    }
    return converted;
}

}  // namespace

std::unique_ptr<ModelSetup> build_model_setup(const std::string& state_dir,
                                              const std::string& legacy_models_dir, bool online) {
    auto setup = std::make_unique<ModelSetup>();
    setup->ui_state = splat::load_ui_state(state_dir);

    if (online) {
        setup->fetcher = std::make_unique<splat::Ps5HttpFetcher>();
        // The transfer's own phases (connect, send, status, body start, bytes
        // every ~5 s, end, error code or abort): the klog lines that date a
        // stall on the next console run (runs/issue-1-crash/sharp-harden-report.md).
        setup->fetcher->log = [](const std::string& text) { line("%s", text.c_str()); };
    }

    splat::ModelManagerOptions manager_options;
    manager_options.state_dir = state_dir;
    manager_options.fetcher = setup->fetcher.get();
    // One transfer at a time. Four ranged requests at once (0.6.0's speed-up)
    // were never proven on hardware; the first real console download wedged with
    // nothing transferred, and the frame loop was delayed at the same time. That
    // the four concurrent sceHttp threads caused either is the leading
    // hypothesis, not a measured fact: the klog has no HEAD-to-body marker and
    // says nothing of sceHttp's internals (runs/issue-1-crash/sharp-dl-verify.md
    // section 1.3). What is established is the discipline: sceHttp is only
    // proven with setup/teardown serialised (ProsperoMgr holds one lock across
    // each whole transfer), and with one at a time requests run front to back,
    // one after the other. Segmentation is off below, so on the console that is
    // the single plain GET. Flip this back to 0 once a gated create-then-read
    // split has downloaded the checkpoint on the console.
    manager_options.max_parallel_transfers = 1;
    // No segments either, so the console asks for the file the one way this
    // console has ever downloaded it: a plain GET (no Range header, status 200)
    // of the whole 2.8 GB body in 64 KiB reads at 2-3 MB/s
    // (runs/v0.2-library/REPORT-060-download.md). Every ranged request the
    // console has made since asked for a body of 702 MB (four in parallel, then
    // one) and stopped on its first body read without a byte
    // (runs/issue-1-crash/console-klog-061.txt, -062.txt and sharp-read-report.md).
    // A body of 60 MB or less was proven ranged in the bench
    // (REPORT-060-probe.md) and 2 MiB ranged requests are what ProsperoMgr
    // streams a game install with, so nothing says a range is wrong in itself;
    // what is missing is a console run that reads one back. One transfer at a
    // time also means segments buy no speed here: with `max_parallel_transfers`
    // 1 they run front to back, which is the same resume position a plain GET
    // leaves. Set this back to kSegmentedMinBytes when a gated ranged download
    // has completed on the console (and re-test with DA2-Small, whose four
    // segments are 25 MB, before SHARP's 702 MB).
    manager_options.segment_min_bytes = std::numeric_limits<uint64_t>::max();
    // Convert SHARP while it downloads: the checkpoint's bytes go straight into
    // the GGUF (core/model/stream_converter.h), so /data sees 1.31 GB instead of
    // the 2.81 GB .pt plus the GGUF. The console's write path is the measured
    // bottleneck (runs/issue-1-crash/sharp-slow-report.md). A streaming attempt
    // that fails does not resume: Retry starts at byte 0. DA2-Small is a
    // safetensors file, so it keeps the download-then-convert path.
    manager_options.convert_while_downloading = true;
    // Empty specs: the built-in pinned SHARP and DA2-Small entries.
    manager_options.converter = &convert_backend;
    // One line per phase with total time and MB/s: download, SHA-256, convert
    // and the whole fetch (runs/issue-1-crash/sharp-fast-report.md).
    manager_options.log = [](const std::string& text) { line("%s", text.c_str()); };
    setup->manager = std::make_unique<splat::ModelManager>(std::move(manager_options));

    splat::ModelServiceOptions service_options;
    service_options.state_dir = state_dir;
    service_options.legacy_models_dir = legacy_models_dir;
    // The download's own klog lines (started, about 10 s of progress, stalled,
    // ended). Without them a console log shows the row's 5% and nothing about
    // the transfer: runs/issue-1-crash/sharp-dl-report.md section 1.
    service_options.log = [](const std::string& text) { line("%s", text.c_str()); };
    setup->service = std::make_unique<splat::ModelService>(*setup->manager, setup->ui_state,
                                                           service_options);

    const std::string selected = setup->service->selected();
    line("viewer: models: state %s, legacy %s, fetcher %s, converter uses %s", state_dir.c_str(),
         legacy_models_dir.c_str(), online ? "sceHttp" : "none (offline)",
         selected.empty() ? "(none)" : selected.c_str());
    // One line per backend, so a console run's klog shows what the panel will
    // draw without guessing from a PNG.
    for (const std::string& name : setup->manager->backend_names()) {
        const Result<splat::ModelRecord> record = setup->manager->status(name);
        if (!record.ok()) {
            line("viewer: models: %s unreadable: %s", name.c_str(), record.message().c_str());
            continue;
        }
        line("viewer: models: %s %s%s%s", record.value().backend.c_str(),
             splat::model_state_name(record.value().state),
             record.value().message.empty() ? "" : " - ",
             record.value().message.c_str());
    }
    return setup;
}

bool stop_models_for_exit(UiRuntime& runtime) {
    if (runtime.model_setup == nullptr || runtime.model_setup->service == nullptr) return true;
    splat::ModelService& service = *runtime.model_setup->service;
    if (service.stop()) return true;
    std::string what = service.running_text();
    if (what.empty()) what = "a model thread";
    line("viewer: models: %s is still running at exit; leaving the panel, the model and the "
         "network request to the process exit",
         what.c_str());
    return false;
}

bool UiRuntime::start(splat::SplatRenderer& renderer, UiHost::Mode mode,
                      const splat::CameraIntrinsics& eye_intrinsics) {
    if (app != nullptr) return true;
    // VR9 (review fix): the same 1280x720 design on a smaller canvas, at the
    // same physical size. The panel's 896x504 is 0.7 of 1280x720, so the theme
    // (margins, row heights, every font size, the chrome bands and the
    // keyboard's own sizes through UiTheme::scaled()) is scaled by 896/1280 in
    // VR. Only one theme struct is involved; the TV keeps it unscaled.
    splat::UiTheme theme = splat::default_theme();
    if (mode == UiHost::Mode::Vr) theme = splat::scaled_theme(theme, 896.0f / 1280.0f);
    app = std::make_unique<splat::UiApp>(services, theme);
    splat::register_default_screens(app->registry());
    app->start();
    host = std::make_unique<UiHost>(renderer, *app, mode);
    // VR9: the panel's canvas-to-headset ratio. PSVR2's fx (~711 px at
    // 1984x2048) projects the follower's 1.4 m panel at 1.6 m to about 622 px
    // per eye; 1280 canvas pixels were a 2:1 minification (the flicker's other
    // half, next to the missing mips). 896x504 keeps the theme's designed ratio
    // (~0.7 headset pixel per canvas pixel, theme.h), uploads 47 % fewer bytes
    // on every change and no longer needs a heavy minification. The TV keeps
    // its 1280x720 (one canvas pixel per output pixel).
    uint32_t width = UiHost::kDefaultCanvasWidth;
    uint32_t height = UiHost::kDefaultCanvasHeight;
    if (mode == UiHost::Mode::Vr) {
        width = 896;
        height = 504;
    }
    if (!host->init(width, height, eye_intrinsics)) {
        last_error_ = host->last_error();
        host.reset();
        app.reset();
        return false;
    }
    return true;
}

UiRuntime make_ui_runtime(const Options& options) {
    UiRuntime runtime;
    runtime.services.paths.scenes = splat::Path(options.scenes_dir);
    runtime.services.paths.inbox = splat::Path(data_path("inbox"));
    runtime.services.paths.state = splat::Path(data_path("state"));
    runtime.services.paths.mnt = splat::Path("/mnt");
    runtime.services.paths.data = splat::Path("/data");  // the photo picker's Console tab
    // A screen's log line is a klog line: the title has no stdout reader
    // (W23's Photos prints `ui: sources ...` per refresh through this sink).
    runtime.services.log = [](const std::string& text) { viewer::line("%s", text.c_str()); };
    // Stage H: the visible-folders policy is installed here, before anything
    // lists a folder and before the conversion chain below starts its worker -
    // a job resumed at boot writes its scene's provenance immediately, and it
    // must read the owner's saved list rather than the all-visible default.
    splat::install_visibility(runtime.services.paths, runtime.services.log);
    // UI_FAKE=1 uses the deterministic fakes a test and a fake-mode console run
    // need. Otherwise the real models chain (W21) is built here - the C1
    // manager over state/, the streaming converter and the sceHttp fetcher,
    // with the legacy fixed-path GGUF folder accepted in place - then the real
    // conversion chain (W19: the queue + runner + JobService the Queue screen
    // and the Convert dialog drive) and the real capture service (W23), which
    // starts the helper that ships in this package
    // (/app0/helper/splat-helper.elf).
    runtime.fake = options.ui_fake;
    // The IME hook (VR9): always the "not available" fake until the console
    // spike plugs sceImeDialog in. The keyboard screen asks it first and falls
    // back to its own grid.
    runtime.text_input = std::make_unique<splat::FakeTextInput>();
    runtime.services.text_input = runtime.text_input.get();
    if (options.ui_fake) {
        runtime.fake_jobs = std::make_unique<splat::FakeJobService>();
        runtime.fake_models = std::make_unique<splat::FakeModelService>();
        runtime.fake_captures =
            std::make_unique<splat::FakeCaptureService>(runtime.services.paths.inbox);
        runtime.fake_ui_state =
            std::make_unique<splat::UiState>(splat::load_ui_state(runtime.services.paths.state));
        runtime.fake_locations = std::make_unique<splat::FakeLocationService>();
        runtime.services.state = runtime.fake_ui_state.get();
        runtime.services.jobs = runtime.fake_jobs.get();
        runtime.services.models = runtime.fake_models.get();
        runtime.services.captures = runtime.fake_captures.get();
        runtime.services.locations = runtime.fake_locations.get();
    } else {
        runtime.model_setup = build_model_setup(data_path("state"),
                                                data_path("models"), true);
        runtime.services.models = runtime.model_setup->service.get();
        // The same ui.json the model service writes selected_model through:
        // Settings' comfort/display rows and the VR loop share this object.
        runtime.services.state = &runtime.model_setup->ui_state;

        // L3: the network locations. The service owns the store, registers one
        // adapter per location at construction (no connect) and proves them on
        // its worker, so a library path (smb0:/scenes) resolves as soon as the
        // panel opens. It is built *before* the conversion chain: the queue's
        // worker starts in JobService's constructor, and a job resumed from the
        // last session uploads into its destination immediately - with the
        // location adapters not yet registered, that job failed with "unknown
        // location \"smb0\"" in the console pass. sceNetInit() runs only when
        // there is something to mount.
        const Path locations_file = splat::Path(data_path("state")) / "locations.json";
        splat::Result<splat::vfs::LocationStore> opened =
            splat::vfs::LocationStore::open(locations_file);
        if (!opened.ok()) {
            line("viewer: locations: %s", opened.message().c_str());
        } else {
            if (!opened.value().locations().empty()) {
                // Always logged, success included: the line says whether
                // sceNetInit came up and whether a host name can be resolved,
                // which is the diagnosis six mounts in the console pass lacked.
                line("viewer: net: %s", ensure_net_init().c_str());
            }
            // One line per location per state change ("ready", "offline: ..."),
            // the same string the Locations screen shows: an unattended console
            // run asserts A3.1/A3.2's "Test = ready" here instead of reading a
            // screenshot. The service does not exist yet while its constructor
            // runs, so the callback reaches it through a slot filled right
            // after.
            auto service_slot = std::make_shared<const splat::ILocationService*>(nullptr);
            auto last_states = std::make_shared<std::map<std::string, std::string>>();
            runtime.location_service = std::make_unique<splat::LocationService>(
                opened.take(), splat::vfs::file_systems(), [service_slot, last_states] {
                    if (*service_slot == nullptr) return;
                    for (const splat::LocationView& location : (*service_slot)->list()) {
                        std::string& known = (*last_states)[location.id];
                        if (known == location.state) continue;
                        known = location.state;
                        line("viewer: location %s %s", location.id.c_str(),
                             location.state.c_str());
                    }
                });
            *service_slot = runtime.location_service.get();

            runtime.services.locations = runtime.location_service.get();
            for (const splat::LocationView& location : runtime.services.locations->list()) {
                line("viewer: location %s %s %s", location.id.c_str(),
                     location.protocol.c_str(), location.name.c_str());
            }
        }

        // W19: the conversion chain over the same state directory. It is built
        // after the models chain (its GGUF choice comes from there), after the
        // locations (see above) and, through this function, after the
        // renderer - the order W18 measured: the renderer's VkDevice first,
        // ggml-vulkan's own device later, on the runner's worker thread.
        runtime.jobs = build_job_setup(splat::Path(data_path("state")),
                                       splat::Path(options.scenes_dir),
                                       splat::Path(data_path("models")),
                                       *runtime.model_setup, runtime.services.locations);
        if (runtime.jobs != nullptr) runtime.services.jobs = runtime.jobs->service.get();
        splat::CaptureService::Options capture_options;
        capture_options.helper_dir = splat::Path(data_path("helper"));
        runtime.captures =
            std::make_unique<splat::CaptureService>(std::move(capture_options));
        runtime.services.captures = runtime.captures.get();
    }
    return runtime;
}

Options ui_options_from_env() {
    Options options;
    const char* value = std::getenv("SCENES_DIR");
    if (value != nullptr && value[0] != '\0') options.scenes_dir = value;
    value = std::getenv("UI");
    if (value != nullptr && value[0] != '\0') {
        options.ui = std::atoi(value) != 0;
        options.ui_given = true;
    }
    value = std::getenv("UI_FAKE");
    if (value != nullptr && value[0] != '\0') options.ui_fake = std::atoi(value) != 0;
    return options;
}

}  // namespace viewer
