#include "jobs/gpu_phase_guard.h"

#include <utility>

namespace splat {

const char* gpu_transition_name(GpuTransition transition) {
    switch (transition) {
        case GpuTransition::InferenceBegin: return "inference.begin";
        case GpuTransition::InferenceEnd: return "inference.end";
        case GpuTransition::ViewerBegin: return "viewer.begin";
        case GpuTransition::ViewerEnd: return "viewer.end";
        case GpuTransition::ModelLoaded: return "model.loaded";
        case GpuTransition::ModelUnloaded: return "model.unloaded";
        case GpuTransition::Shutdown: return "guard.shutdown";
    }
    return "unknown";
}

void GpuPhaseGuard::set_observer(Observer observer) {
    std::lock_guard<std::mutex> lock(mutex_);
    observer_ = std::move(observer);
}

void GpuPhaseGuard::notify_locked(GpuTransition transition) {
    if (!observer_) return;
    observer_(transition, phase_locked(), model_loaded_);
}

bool GpuPhaseGuard::begin_inference() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stopped_ || inference_ready_locked(); });
        if (stopped_) return false;
        inference_ = true;
        notify_locked(GpuTransition::InferenceBegin);
    }
    return true;
}

void GpuPhaseGuard::end_inference() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        inference_ = false;
        notify_locked(GpuTransition::InferenceEnd);
    }
    cv_.notify_all();
}

bool GpuPhaseGuard::begin_viewing() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stopped_ || viewing_ready_locked(); });
        if (stopped_) return false;
        ++viewers_;
        notify_locked(GpuTransition::ViewerBegin);
    }
    return true;
}

void GpuPhaseGuard::end_viewing() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (viewers_ > 0) --viewers_;
        notify_locked(GpuTransition::ViewerEnd);
    }
    cv_.notify_all();
}

bool GpuPhaseGuard::try_begin_inference() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ || !inference_ready_locked()) return false;
        inference_ = true;
        notify_locked(GpuTransition::InferenceBegin);
    }
    return true;
}

bool GpuPhaseGuard::try_begin_viewing() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ || !viewing_ready_locked()) return false;
        ++viewers_;
        notify_locked(GpuTransition::ViewerBegin);
    }
    return true;
}

void GpuPhaseGuard::set_model_loaded(bool loaded) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        model_loaded_ = loaded;
        notify_locked(loaded ? GpuTransition::ModelLoaded : GpuTransition::ModelUnloaded);
    }
    cv_.notify_all();
}

bool GpuPhaseGuard::model_loaded() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return model_loaded_;
}

void GpuPhaseGuard::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        notify_locked(GpuTransition::Shutdown);
    }
    cv_.notify_all();
}

bool GpuPhaseGuard::stopped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_;
}

GpuPhase GpuPhaseGuard::phase() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return phase_locked();
}

}  // namespace splat
