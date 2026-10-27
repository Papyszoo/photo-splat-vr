// The renderer/conversion ordering rule the TV and VR loops follow (W19).
//
// W18 measured S1 on the console (runs/ui_v1/reports/W18.md 4.1-4.3): SHARP runs
// in the title process while the renderer, VideoOut and the panel stay alive, on
// ggml-vulkan's own VkDevice over the same 11.44 GiB heap. The viewed scene stays
// loaded (run B: a live 1.18 M-splat scene, 8.65 GiB still free at model load,
// peak RSS 7.53 GiB), so a conversion does not own the screen: the user may keep
// browsing and switch scenes while the photo infers.
//
// The one scene load that has to wait is the model-load phase itself: a queued
// job (the worker is about to load) or a running job whose stage is still
// "starting" (JobRunner loads the GGUF before it reports "inferring"). That is
// the tight window; after it the model is resident and a scene upload
// (0.28 GiB for 1.2 M splats) next to inference fits in the measured headroom.
// The gate's policy below (job_loading_model) is what ViewScene and L1/R1 ask;
// the toast is "Converting... scene switch in a moment".
//
// The gate itself follows the job service one frame at a time, edge by edge, and
// runs four hooks in the order W18 measured:
//
//   Idle --(a job is enqueued)--> Paused
//       pause_loading()   a conversion began
//       release_scene()   empty in S1 (the viewed scene stays); the unload
//                         variant of the same machine
//   Paused --(the service reports a running job)--> Converting
//   Paused/Converting --(no queued or running job left)--> Restoring
//       restore_scene()   after the last job, on success and on failure
//       resume_loading()  the conversion is over
//   Restoring --> Idle
//
// Nothing here touches a GPU, a file or a thread: the hooks are the host's, so
// the order is reproducible on the desktop (tests/ps5_jobs_order_test.cpp drives
// the same machine over the real JobService and a fake reconstructor).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <utility>

#include "ui/services.h"

namespace viewer {

enum class ConvertGatePhase {
    Idle,        // no conversion: scene loading allowed
    Paused,      // a job is queued but not running yet: loading closed
    Converting,  // the service reports a running job
    Restoring,   // transient, inside poll(): restore_scene() then resume_loading()
};

inline const char* convert_gate_phase_name(ConvertGatePhase phase) {
    switch (phase) {
        case ConvertGatePhase::Idle: return "idle";
        case ConvertGatePhase::Paused: return "paused";
        case ConvertGatePhase::Converting: return "converting";
        case ConvertGatePhase::Restoring: return "restoring";
    }
    return "idle";
}

// True while the service has a job in flight, queued or running. A job that was
// cancelled or has finished is not active, so a queue whose remaining rows are
// done/failed/cancelled counts as idle. This is the condition the gate follows;
// busy() alone is not enough, because an enqueue is visible before the worker
// takes the job.
inline bool jobs_active(const splat::IJobService& jobs) {
    for (const splat::JobView& view : jobs.snapshot()) {
        if (view.state == "queued" || view.state == "running") return true;
    }
    return false;
}

// True only in the model-load phase: a queued job (the worker is about to load)
// or a running job whose stage is still "starting". JobRunner reports "starting"
// before the GGUF load and "inferring" after it, so this is the window the scene
// switch waits for. Everything after it - inference, a second queued photo - is
// open: W18 run B left 8.65 GiB free with a live 1.18 M-splat scene, and one
// scene upload is 0.28 GiB.
inline bool job_loading_model(const splat::IJobService& jobs) {
    for (const splat::JobView& view : jobs.snapshot()) {
        if (view.state == "queued") return true;
        if (view.state == "running" && (view.stage.empty() || view.stage == "starting")) {
            return true;
        }
    }
    return false;
}

// The scene-switch permission the TV loop, the VR loop and the panel's ViewScene
// ask before reading/uploading a scene. Only the model-load phase holds it.
inline bool scene_loading_allowed(const splat::IJobService& jobs) {
    return !job_loading_model(jobs);
}

class ConvertGate {
public:
    using Hook = std::function<void()>;

    // The host's four hooks. Empty hooks are valid and are what the S1 TV path
    // passes for release_scene (W18 4.2: nothing has to be unloaded).
    struct Hooks {
        Hook pause_loading;
        Hook release_scene;
        Hook restore_scene;
        Hook resume_loading;
    };

    ConvertGate() = default;
    explicit ConvertGate(Hooks hooks) : hooks_(std::move(hooks)) {}

    // Advances the machine from the service's current state. Call once per frame
    // while the panel is up. Returns true when the phase changed (the host may
    // log convert_gate_phase_name()).
    bool poll(const splat::IJobService& jobs) {
        const bool active = jobs_active(jobs);
        switch (phase_) {
            case ConvertGatePhase::Idle:
                if (!active) return false;
                run(hooks_.pause_loading);
                run(hooks_.release_scene);
                phase_ = ConvertGatePhase::Paused;
                return true;

            case ConvertGatePhase::Paused:
                // The worker has to take the queued job before it can run; a
                // job that never runs (cancelled, or refused by the runner) is
                // seen as "not active" here and restores on the spot.
                if (jobs.busy()) {
                    phase_ = ConvertGatePhase::Converting;
                    return true;
                }
                if (!active) {
                    restore();
                    return true;
                }
                return false;

            case ConvertGatePhase::Converting:
                // A second queued job starts right after the first: the model is
                // resident again, so stay closed instead of flapping the hooks.
                if (active) return false;
                restore();
                return true;

            case ConvertGatePhase::Restoring:
                return false;  // never left observable between two poll() calls
        }
        return false;
    }

    // The host is ending the run (quit, a failed frame) and a job may still be
    // running: only scene loading is reopened. Nothing is restored and no scene
    // is put back under a GPU that may still be writing (W18's rule for a
    // converter that outlived its window).
    void abort() {
        if (phase_ == ConvertGatePhase::Idle) return;
        run(hooks_.resume_loading);
        phase_ = ConvertGatePhase::Idle;
    }

    ConvertGatePhase phase() const { return phase_; }

    // Jobs the gate followed from enqueue to idle (one per batch, not per photo:
    // a queue of two photos restores once).
    int completed() const { return completed_; }

private:
    static void run(const Hook& hook) {
        if (hook) hook();
    }

    void restore() {
        phase_ = ConvertGatePhase::Restoring;
        run(hooks_.restore_scene);
        run(hooks_.resume_loading);
        phase_ = ConvertGatePhase::Idle;
        ++completed_;
    }

    Hooks hooks_;
    ConvertGatePhase phase_ = ConvertGatePhase::Idle;
    int completed_ = 0;
};

}  // namespace viewer
