// The console's conversion chain; see ps5_jobs.h.
//
// Same objects the desktop tool and the tests assemble (tools/reconstruct,
// tests/ui_jobservice_test.cpp): JobQueue::open -> GpuPhaseGuard -> JobRunner
// with a backend factory -> JobService with its worker thread. The console adds
// two things: the paths and the model lookup (below), and the one thread every
// ggml call has to happen on.
//
// That thread is why this file is longer than its header suggests. W18 (runs/
// ui_v1/reports/W18.md 4.1-4.3) ran the converter on a 32 MiB-stack thread
// because ACO recurses deeply during the first pipeline compile and a title's
// stack is small - the same rule title_common.h's run_with_big_stack() applies
// to the renderer. JobService's own worker is a plain std::thread with the
// default stack, and it is core/ui's code, so the console keeps that worker as
// the queue driver and sends every backend call to one dedicated 32 MiB-stack
// thread owned here. All ggml work therefore still happens on a single thread
// (W18's property), created lazily by the first load() - after the renderer's
// device, as measured.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ps5_jobs.h"

#include <pthread.h>

#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <system_error>
#include <utility>

#include "recon/fallback/fallback_reconstructor.h"
#include "recon/sharp/sharp_reconstructor.h"
#include "title_common.h"
#include "ui_setup.h"

namespace viewer {
namespace {

using splat::Path;

constexpr size_t kConverterStackBytes = 32u * 1024u * 1024u;

}  // namespace

// One thread for every ggml call, with W18's 32 MiB stack. The JobService
// worker posts a task and waits, so the runner's ordering (model loaded before
// the photo, unloaded before the next job) is unchanged and only one thread ever
// touches the ggml device.
class ConverterThread {
public:
    ConverterThread() = default;
    ~ConverterThread() { stop(); }

    ConverterThread(const ConverterThread&) = delete;
    ConverterThread& operator=(const ConverterThread&) = delete;

    void run(const std::function<void()>& task) {
        if (!start()) {
            // No thread (the console refused 32 MiB): run it where the caller
            // is, on the JobService worker's stack. The message was logged.
            task();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            task_ = &task;
            done_ = false;
        }
        wake_.notify_all();
        std::unique_lock<std::mutex> lock(mutex_);
        finished_.wait(lock, [this] { return done_; });
    }

private:
    static void* entry(void* argument) {
        static_cast<ConverterThread*>(argument)->loop();
        return nullptr;
    }

    bool start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) return thread_started_;
        started_ = true;

        // The same recipe as title_common.h's run_with_big_stack(): an explicit
        // 32 MiB buffer first (that is what W18 ran the converter on), a plain
        // stack size second, and no thread at all if the console refuses both.
        stack_ = static_cast<char*>(std::malloc(kConverterStackBytes));
        int created = -1;
        pthread_attr_t attributes;
        if (pthread_attr_init(&attributes) == 0) {
            if (stack_ != nullptr) {
                created = pthread_attr_setstack(&attributes, stack_, kConverterStackBytes);
            }
            if (created != 0) created = pthread_attr_setstacksize(&attributes, kConverterStackBytes);
            if (created == 0) created = pthread_create(&thread_, &attributes, &entry, this);
            pthread_attr_destroy(&attributes);
        }
        thread_started_ = created == 0;
        if (thread_started_) {
            line("viewer: jobs: converter thread up (%zu MiB stack); every ggml call runs there",
                 kConverterStackBytes / (1024u * 1024u));
        } else {
            std::free(stack_);
            stack_ = nullptr;
            line("viewer: jobs: a %zu MiB stack thread was refused (%d); the converter runs on "
                 "the JobService worker's stack",
                 kConverterStackBytes / (1024u * 1024u), created);
        }
        return thread_started_;
    }

    void loop() {
        for (;;) {
            const std::function<void()>* task = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stopping_ || task_ != nullptr; });
                if (stopping_ && task_ == nullptr) return;
                task = task_;
            }
            if (task != nullptr) (*task)();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                task_ = nullptr;
                done_ = true;
            }
            finished_.notify_all();
        }
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!started_ || !thread_started_) return;
            stopping_ = true;
        }
        wake_.notify_all();
        pthread_join(thread_, nullptr);
        std::lock_guard<std::mutex> lock(mutex_);
        thread_started_ = false;
        std::free(stack_);
        stack_ = nullptr;
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable finished_;
    const std::function<void()>* task_ = nullptr;
    char* stack_ = nullptr;  // the 32 MiB buffer given to pthread_attr_setstack
    bool started_ = false;
    bool thread_started_ = false;
    bool stopping_ = false;
    bool done_ = false;
    pthread_t thread_{};
};

namespace {

// The backend for one job: the chosen reconstructor, with every call forwarded
// to the converter thread. The runner's factory creates one per job, as it does
// on the desktop; the thread (and the ggml device it creates) is shared and
// outlives the job. `create` names the class the factory registered (SHARP or
// the DA2-Small fallback), so this wrapper never has to know which backend is
// which.
class ConsoleBackend : public splat::IReconstructor {
public:
    // `model` is this backend's own GGUF, resolved again for the job's backend
    // (the runner carries one path for every job); empty means it was not
    // found, and load() fails the job with the paths it looked in.
    ConsoleBackend(std::shared_ptr<ConverterThread> thread, std::string backend, Path model,
                   Path legacy_models_dir, std::function<std::unique_ptr<splat::IReconstructor>()> create)
        : thread_(std::move(thread)),
          backend_(std::move(backend)),
          model_(std::move(model)),
          legacy_models_dir_(std::move(legacy_models_dir)),
          create_(std::move(create)) {}

    splat::BackendInfo info() const override {
        splat::BackendInfo info;
        thread_->run([&] {
            if (inner_ != nullptr) info = inner_->info();
        });
        return info;
    }

    splat::Status load(const Path&) override {
        if (model_.empty()) {
            const Path own = legacy_models_dir_ / (backend_ + ".gguf");
            return splat::Status::error(
                "no model file for backend '" + backend_ + "' (looked for " + own.string() +
                (backend_ == "sharp" ? " and W18's sharp_2572gikvuh-{f16,f32}-v1.gguf" : "") + ")");
        }
        splat::Status status = splat::Status::error("the converter thread did not run the load");
        thread_->run([&] {
            inner_ = create_();
            status = inner_->load(model_);
            if (!status.ok()) inner_.reset();
        });
        return status;
    }

    splat::Result<splat::SplatScene> run(const splat::ReconstructionInput& input,
                                        splat::Progress& progress) override {
        splat::Result<splat::SplatScene> scene =
            splat::Result<splat::SplatScene>::failure("the converter thread did not run the photo");
        thread_->run([&] {
            if (inner_ == nullptr) {
                scene = splat::Result<splat::SplatScene>::failure("the converter has no model open");
                return;
            }
            scene = inner_->run(input, progress);
        });
        return scene;
    }

    void unload() override {
        thread_->run([&] {
            if (inner_ != nullptr) inner_->unload();
            inner_.reset();
        });
    }

private:
    std::shared_ptr<ConverterThread> thread_;
    std::string backend_;
    Path model_;
    Path legacy_models_dir_;
    std::function<std::unique_ptr<splat::IReconstructor>()> create_;
    // Only touched on the converter thread.
    std::unique_ptr<splat::IReconstructor> inner_;
};

// The console's job log: one klog line per stage and per terminal state, so a
// console run's own log says what the queue did (the panel shows it on screen,
// which an unattended run cannot read). Called from the worker thread after every
// published change; only a new stage, a new 10 % step or a job's end prints.
struct JobLog {
    std::mutex mutex;
    const splat::IJobService* service = nullptr;
    std::string stage;
    int decile = -1;
    std::map<std::string, std::string> states;  // job id -> last logged state
};

void log_changes(const std::shared_ptr<JobLog>& state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->service == nullptr) return;
    for (const splat::JobView& view : state->service->snapshot()) {
        std::string& known = state->states[view.id];
        if (view.state == "running") {
            const int decile = static_cast<int>(view.fraction * 10.0f);
            if (view.stage == state->stage && decile == state->decile) continue;
            state->stage = view.stage;
            state->decile = decile;
            line("viewer: jobs: %s %s %d%% (%s)", view.name.c_str(), view.stage.c_str(),
                 decile * 10, view.backend.c_str());
            continue;
        }
        if (known == view.state) continue;
        known = view.state;
        if (view.state == "queued") continue;
        line("viewer: jobs: %s %s%s%s", view.name.c_str(), view.state.c_str(),
             view.message.empty() ? "" : ": ", view.message.c_str());
    }
}

}  // namespace

std::unique_ptr<JobSetup> build_job_setup(const Path& state_dir, const Path& scenes_dir,
                                          const Path& legacy_models_dir, ModelSetup& models) {
    std::error_code error;
    std::filesystem::create_directories(state_dir, error);
    const Path queue_file = state_dir / "jobs.json";
    splat::Result<splat::JobQueue> opened = splat::JobQueue::open(queue_file);
    if (!opened.ok()) {
        line("viewer: jobs: %s; conversions stay unavailable", opened.message().c_str());
        return nullptr;
    }

    const std::string selected = models.service != nullptr ? models.service->selected() : "";
    // No model selected means no default model (docs/ui-v1-plan.md section 2):
    // the runner still needs a path, so fall back to the legacy SHARP GGUF the
    // console bench uses. A job for any other backend fails on the factory.
    const std::string backend = selected.empty() ? std::string("sharp") : selected;
    // The same lookup the Models screen's "verified in place" check uses
    // (core/ui/model_service.h), so the dialog's Ready row and the converter can
    // never disagree. Each job resolves its own backend's file again below; a
    // missing one is the job's failure with a clear message, not a silent load of
    // another backend's weights and not a panel with no queue at all.
    const Path startup_model =
        splat::resolved_model_path(*models.manager, backend, legacy_models_dir);
    if (startup_model.empty()) {
        line("viewer: jobs: no model file for backend %s (looked under %s and %s); a job for it "
             "will fail when it starts",
             backend.c_str(), (state_dir / "models").c_str(), legacy_models_dir.c_str());
    }
    if (backend != "sharp" && backend != "da2") {
        line("viewer: jobs: converter uses %s, which has no runtime backend in core/recon; "
             "its jobs will fail to start",
             backend.c_str());
    }

    auto setup = std::make_unique<JobSetup>();
    setup->queue = opened.take();

    // Declared before the runner so the factory's shared_ptr keeps the thread
    // alive until the last backend object (and the runner's copy of the lambda)
    // is gone: the service joins its worker first, then the runner goes.
    setup->converter = std::make_shared<ConverterThread>();
    const std::shared_ptr<ConverterThread> converter = setup->converter;
    splat::ModelManager* const manager = models.manager.get();
    auto factory = [converter, manager, legacy_models_dir](
                       const std::string& name) -> std::unique_ptr<splat::IReconstructor> {
        if (manager == nullptr) return nullptr;
        std::function<std::unique_ptr<splat::IReconstructor>()> create;
        if (name == "sharp") {
            create = [] { return std::make_unique<splat::SharpReconstructor>(); };
        } else if (name == "da2") {
            // DA2-Small is C4's fallback backend; it is built into the title
            // already (core/recon/fallback is in libsplat_core_ps5.a, and
            // da2_bench runs the same class on the console).
            create = [] { return std::make_unique<splat::FallbackReconstructor>(); };
        } else {
            return nullptr;
        }
        const Path model = splat::resolved_model_path(*manager, name, legacy_models_dir);
        return std::make_unique<ConsoleBackend>(converter, name, model, legacy_models_dir,
                                                std::move(create));
    };

    splat::JobRunnerConfig config;
    config.output_root = scenes_dir;
    // The runner loads a model per job and unloads it before the guard admits a
    // viewer. This path is only what the runner hands to load(); ConsoleSharp
    // ignores it and loads the file resolved for the job's own backend, so a
    // model installed or selected after the panel was built is used from the
    // next job, not the next launch.
    config.model_path = startup_model;
    setup->runner =
        std::make_unique<splat::JobRunner>(*setup->queue, setup->guard, std::move(factory), config);
    auto job_log = std::make_shared<JobLog>();
    setup->service = std::make_unique<splat::JobService>(
        *setup->queue, *setup->runner, [job_log] { log_changes(job_log); });
    job_log->service = setup->service.get();

    line("viewer: jobs: queue %s (%zu row(s)), scenes %s, backend %s, model %s",
         queue_file.c_str(), setup->queue->jobs().size(), scenes_dir.c_str(), backend.c_str(),
         startup_model.empty() ? "(none)" : startup_model.c_str());
    return setup;
}

}  // namespace viewer
