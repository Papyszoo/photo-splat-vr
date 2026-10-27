#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace splat {

// Progress sink passed to IReconstructor::run. A null callback is valid and
// makes report() a no-op, which keeps tests and headless callers simple.
class Progress {
public:
    using Callback = std::function<void(float fraction, const std::string& stage)>;

    Progress() = default;
    explicit Progress(Callback callback) : callback_(std::move(callback)) {}

    // fraction is clamped to [0, 1]. stage is a short human-readable label.
    void report(float fraction, std::string_view stage = {}) {
        if (fraction < 0.0f) fraction = 0.0f;
        if (fraction > 1.0f) fraction = 1.0f;
        if (fraction < fraction_) fraction = fraction_;
        fraction_ = fraction;
        if (!callback_) return;
        callback_(fraction_, std::string(stage));
    }

    float fraction() const { return fraction_; }
    bool has_callback() const { return static_cast<bool>(callback_); }

private:
    Callback callback_;
    float fraction_ = 0.0f;
};

using ProgressCallback = Progress::Callback;

}  // namespace splat
