#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>

namespace splat {

enum class GpuPhase { Idle, Inference, Viewing };

// One edge of the guard's state machine, in the order the app's transition log
// records it. ModelLoaded/ModelUnloaded bracket the weights being resident,
// which is what makes "unload before the renderer allocates" checkable.
enum class GpuTransition {
    InferenceBegin,
    InferenceEnd,
    ViewerBegin,
    ViewerEnd,
    ModelLoaded,
    ModelUnloaded,
    Shutdown,
};

// "inference.begin", "model.unloaded", ... (the app's log tokens).
const char* gpu_transition_name(GpuTransition transition);

// C9/C6. In v1 inference and viewing never share the GPU: the backend is
// unloaded before the renderer allocates. This object arbitrates between the
// two sides.
//
//   - Inference waits until no viewer holds the GPU.
//   - Viewing waits until inference has finished *and* the runner reported the
//     model unloaded. Ending inference while the model is still resident is
//     allowed but keeps viewers out, so forgetting unload() cannot expose a
//     loaded model to the renderer.
//
// The guard only decides ordering; it allocates nothing and touches no GPU.
// Safe to call from the runner thread and the UI/render thread.
//
// Both blocking calls return false instead of blocking once shutdown() has been
// called, so a thread parked in one of them cannot keep the process alive when
// the app is closing.
class GpuPhaseGuard {
public:
    // Optional observer for the app's transition log. It is called from the
    // thread that caused the transition with the post-transition state, while
    // the guard's mutex is held: the log line for a release is therefore
    // written before any thread can wake up and take the phase, so the log
    // order is the real order. Set it once at start-up; keep it short, use the
    // snapshot arguments instead of querying the guard, and do not call back
    // into it (no begin_*/end_*/set_model_loaded) or it will deadlock.
    using Observer = std::function<void(GpuTransition, GpuPhase, bool model_loaded)>;
    void set_observer(Observer observer);

    // Blocks while a viewer holds the GPU, then marks inference active.
    // Returns false only when the guard has been shut down.
    bool begin_inference();
    // Leaves inference. Call after set_model_loaded(false); viewers stay
    // blocked until the model is reported unloaded.
    void end_inference();

    // Blocks while inference runs or the model is loaded.
    bool begin_viewing();
    void end_viewing();

    // Non-blocking variants, for tests and for callers that would rather retry.
    // Return false when the matching begin_*() would have to wait.
    bool try_begin_inference();
    bool try_begin_viewing();

    // Lifecycle of the backend weights, reported by the job runner.
    void set_model_loaded(bool loaded);
    bool model_loaded() const;

    // Wakes every blocked waiter and makes later begin_*() calls return false
    // without taking the phase. Call once, when the app is closing down.
    void shutdown();
    bool stopped() const;

    GpuPhase phase() const;

private:
    bool inference_ready_locked() const { return !inference_ && viewers_ == 0; }
    bool viewing_ready_locked() const { return !inference_ && !model_loaded_; }
    GpuPhase phase_locked() const {
        if (inference_) return GpuPhase::Inference;
        if (viewers_ > 0) return GpuPhase::Viewing;
        return GpuPhase::Idle;
    }
    // Calls the observer with the state after the transition. The caller must
    // hold mutex_: the transition's log line has to be written before another
    // thread can change the state and write its own, which is what keeps the
    // log in the real order (a begin never overtakes the matching end).
    void notify_locked(GpuTransition transition);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    Observer observer_;
    bool inference_ = false;
    int viewers_ = 0;
    bool model_loaded_ = false;
    bool stopped_ = false;
};

}  // namespace splat
