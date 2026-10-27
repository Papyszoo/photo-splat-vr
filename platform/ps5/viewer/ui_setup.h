// The in-app panel's construction, shared by the TV path (main.cpp) and the VR
// path (vr_mode.cpp): viewer.env's Options, the gallery paths the screens read,
// one UiApp over core/ui and one UiHost over the renderer - plus, when UI_FAKE
// is off, the console's real models chain (W21), conversion chain (W19) and
// capture service (W23).
//
// W15 built this inline in main.cpp; W16 shows the same panel in the headset,
// W21 wires the real Models service into it and W23 the Captures tab with the
// helper it starts from inside the package. So both modes call
// make_ui_runtime() and UiRuntime::start(): one UiApp, one UiHost, one set of
// paths and services, whichever mode asks (docs/ui-v1-plan.md sections 2-3:
// "UiHost is the only thing main.cpp/vr_mode.cpp talk to"), and the models
// service is assembled exactly once by build_model_setup() for both of them.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>

#include "common/path.h"
#include "model/model_manager.h"
#include "platform/ps5/net/ps5_http_fetcher.h"
#include "render/camera.h"
#include "render/viewer_state.h"
#include "ui/app.h"
#include "ui/capture_service.h"
#include "ui/fakes.h"
#include "ui/model_service.h"
#include "ui/services.h"
#include "ui/ui_state.h"

#include "ps5_jobs.h"
#include "ui_host.h"

namespace splat {
class SplatRenderer;
}  // namespace splat

namespace viewer {

// The console data folders: the one data root of the identity this package was
// built as — the dataDir field of platform/ps5/title/identities.json, compiled
// in by build-title.mk as SPLAT_DATA_ROOT (dev: /data/ps5-splat-vr, release:
// the user's own folder). A relative path from viewer.env resolves under it,
// never under "/", and every folder the title writes (inbox, state/ui.json,
// models, scenes, shots, logs, samples marker, helper mailbox) is spelled from
// it. No viewer source names a console data path a second time.
#ifndef SPLAT_DATA_ROOT
#error "build-title.mk must define SPLAT_DATA_ROOT (identity.py: the identity's dataDir)"
#endif
inline constexpr const char* kDataDir = SPLAT_DATA_ROOT;

// A path below that root: the only spelling the viewer uses.
inline std::string data_path(const std::string& relative) {
    return std::string(kDataDir) + "/" + relative;
}

// W31: the sample scenes a release package carries (viewer/Makefile copies them
// from $SPLAT_DATA_DIR/assets/samples/out) and the marker that makes the seed
// into the gallery a one-time event. No samples in a package = no folder = no
// seed and no marker.
inline constexpr const char* kSamplesDir = "/app0/samples";

// viewer.env: bench.env style, `KEY=value`, every entry also exported before the
// first GPU call. These are the keys the viewer reads; main.cpp fills them once
// and both modes take them from there.
struct Options {
    std::string mode = "vr";                    // MODE: vr (default) or tv
    std::string scene;                          // SCENE=<folder name>, empty = first
    std::string scenes_dir = data_path("scenes");  // SCENES_DIR
    int seconds = 60;                           // SECONDS: TV run length / VR cap
    bool seconds_given = false;
    uint32_t width = 1920;                      // WIDTH (TV only)
    uint32_t height = 1080;                     // HEIGHT (TV only)
    int flip_rate = 1;                          // FLIP_RATE 0|1|2 (TV only)
    float comfort_radius = splat::kDefaultComfortRadius;  // COMFORT_RADIUS
    bool comfort_given = false;                 // COMFORT_RADIUS was in viewer.env
    float world_scale = 1.0f;                   // WORLD_SCALE (VR only)
    bool preload = true;                        // PRELOAD=0 (VR only)
    bool mirror = true;                         // MIRROR=0 (VR only)
    float sway_period = 12.0f;                  // SWAY_PERIOD seconds (TV only)
    // UI v1: the panel and the scripted input (docs/ui-v1-plan.md section 6).
    bool ui = false;                            // UI=1: boot into the panel (UiApp)
    bool ui_given = false;                      // UI was in viewer.env
    bool ui_fake = false;                       // UI_FAKE=1: deterministic fake services
    std::string script;                         // SCRIPT=<path>: replay this input script
    std::string shots_dir = data_path("shots");    // SHOTS_DIR: where `shot` lines write PNGs
    bool env_ok = false;
};

// The console's real models chain: the three objects that have to outlive the
// panel. The C1 ModelManager (licence gate, resumable transfer, pinned SHA-256,
// conversion), W3's sceHttp fetcher (null in an offline build) and the W13
// ModelService adapter the screens consume (core/ui/model_service.h). The
// shared UiState lives here too: the adapter writes selected_model through it
// and persists it to <state>/ui.json, which is what the Convert dialog reads.
//
// Not movable (or copyable): ModelService holds a reference to ui_state, so the
// object must stay where build_model_setup() allocated it.
struct ModelSetup {
    ModelSetup() = default;
    ModelSetup(const ModelSetup&) = delete;
    ModelSetup& operator=(const ModelSetup&) = delete;

    splat::UiState ui_state;
    // Null when the setup was built offline: the manager then only imports a
    // checkpoint dropped in <state>/inbox (the C1 offline path).
    std::unique_ptr<splat::Ps5HttpFetcher> fetcher;
    std::unique_ptr<splat::ModelManager> manager;
    std::unique_ptr<splat::ModelService> service;
};

// state_dir is the manager's root (/data/ps5-splat-vr/state on the console):
// licenses.json, models/, downloads/ and inbox/ live under it.
// legacy_models_dir is the old fixed-path GGUF folder: a verified
// <dir>/<backend>.gguf is accepted as Ready in place, without a copy.
// online == false builds the manager with no fetcher (offline inbox only).
std::unique_ptr<ModelSetup> build_model_setup(const std::string& state_dir,
                                              const std::string& legacy_models_dir, bool online);

// Everything the panel owns. The services are the gallery paths plus the real
// models chain (W21), capture service (W23) and conversion chain (W19) or, with
// UI_FAKE=1, the deterministic fakes; a null service is what a screen shows as
// "not available in this build" (core/ui/services.h).
struct UiRuntime {
    splat::UiServices services;
    bool fake = false;                         // UI_FAKE=1 built the fake services
    std::unique_ptr<splat::FakeJobService> fake_jobs;
    std::unique_ptr<splat::FakeModelService> fake_models;
    std::unique_ptr<splat::FakeCaptureService> fake_captures;
    // The system keyboard hook (VR9): "not available" for now, so the panel
    // keyboard is the fallback. A console job swaps this for an sceImeDialog
    // adapter; nothing else in the runtime changes.
    std::unique_ptr<splat::FakeTextInput> text_input;
    // W21: the real models chain (non-null with UI_FAKE off); services.models
    // points into it, so this must outlive the panel.
    std::unique_ptr<ModelSetup> model_setup;
    // UI_FAKE=1 has no ModelSetup to own the settings state, so it lives here.
    // services.state points into whichever of the two is live.
    std::unique_ptr<splat::UiState> fake_ui_state;
    // W19: the real conversion chain (non-null with UI_FAKE off, unless the
    // queue file or the GGUF is missing); services.jobs points into it and it
    // owns the worker thread, so it must outlive the panel.
    std::unique_ptr<JobSetup> jobs;
    // W23: the real capture service (non-null with UI_FAKE off), whose
    // launcher pushes /app0/helper/splat-helper.elf to the payload loader;
    // services.captures points into it.
    std::unique_ptr<splat::CaptureService> captures;
    std::unique_ptr<splat::UiApp> app;
    std::unique_ptr<UiHost> host;

    // Creates the app (screens registered, started at the menu) and its host
    // over `renderer`, in `mode`: the renderer has to exist first, so this is
    // the step the TV and the VR path each take once theirs is up. False with
    // last_error() set when the host's textures could not be created.
    bool start(splat::SplatRenderer& renderer, UiHost::Mode mode,
               const splat::CameraIntrinsics& eye_intrinsics);
    const std::string& last_error() const { return last_error_; }

private:
    std::string last_error_;
};

// The paths and services of one panel, with the fakes when options.ui_fake says
// so or the real models chain and capture service otherwise. No GPU work: it
// may be called before or after the renderer exists.
UiRuntime make_ui_runtime(const Options& options);

// The UI keys of the Options, read from the environment main.cpp exported from
// viewer.env (load_env_file setenvs every entry). The VR session receives a
// VrOptions, not this struct, and must not parse viewer.env a second time.
Options ui_options_from_env();

}  // namespace viewer
