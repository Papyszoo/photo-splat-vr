// The console's conversion chain: core/ui's JobService (W12) over core/jobs'
// queue and runner, with the SHARP and DA2-Small backends and the panel's own
// model choice
// (W19).
//
// This is the piece the panel was missing: ui_setup.cpp built the real gallery,
// models (W21) and captures (W23) services, and left services.jobs null, so the
// Queue screen said "Jobs are not available in this build" and the Convert
// dialog refused to start. build_job_setup() assembles the same four objects the
// desktop and the tests use (JobQueue -> GpuPhaseGuard -> JobRunner ->
// JobService), with the console's paths and backend:
//
//   queue     /data/ps5-splat-vr/state/jobs.json (survives a restart; a job that
//             was Running when the title died is re-queued by JobQueue::open)
//   scenes    SCENES_DIR, so a converted scene lands where the gallery lists it
//   model     the model the Models screen marked "the converter uses"
//             (state/models/<backend>.gguf), else the pre-manager fixed-path
//             folder (/data/ps5-splat-vr/models/<backend>.gguf and W18's SHARP
//             file names)
//
// S1 (W18): the runner's worker thread calls ggml-vulkan, which creates its own
// VkDevice; that must happen after the renderer's device, so the panel runtime
// (and this chain) is built only once the renderer exists - see ui_setup.cpp.
// The worker is the single thread that touches the model; the UI thread only
// enqueues, cancels and reads snapshots.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <optional>
#include <string>

#include "common/path.h"
#include "jobs/gpu_phase_guard.h"
#include "jobs/job_queue.h"
#include "jobs/job_runner.h"
#include "ui/job_service.h"

namespace viewer {

struct ModelSetup;
class ConverterThread;

// Everything the conversion chain owns. `service` is what UiServices::jobs
// points at, so this object must outlive the panel. Not movable: the service
// holds the queue's mutex and the runner's hooks.
//
// The members are declared in construction order, so destruction is the reverse
// and load-bearing: the service joins its worker first, then the runner (which
// points at the queue) goes, then the guard, the queue and the converter thread.
struct JobSetup {
    JobSetup() = default;
    JobSetup(const JobSetup&) = delete;
    JobSetup& operator=(const JobSetup&) = delete;

    std::optional<splat::JobQueue> queue;
    splat::GpuPhaseGuard guard;
    // The one thread every ggml call runs on (W18's rule: one thread, 32 MiB
    // stack). Created by the first load(), never touched by the UI.
    std::shared_ptr<ConverterThread> converter;
    std::unique_ptr<splat::JobRunner> runner;
    std::unique_ptr<splat::JobService> service;
};

// Opens <state_dir>/jobs.json and builds the queue, runner and service. `models`
// is the console's real models chain (W21): its selected backend decides which
// GGUF the converter loads. `legacy_models_dir` is the fixed-path folder the
// shared legacy lookup reads (core/ui/model_service.h).
//
// Null with a "viewer: jobs: ..." log line only when the queue file cannot be
// read. A backend with no model file still gets the queue: its job fails on
// start with the paths that were looked in (each job resolves its own backend's
// file), instead of hiding the whole queue screen.
std::unique_ptr<JobSetup> build_job_setup(const splat::Path& state_dir,
                                          const splat::Path& scenes_dir,
                                          const splat::Path& legacy_models_dir,
                                          ModelSetup& models);

}  // namespace viewer
