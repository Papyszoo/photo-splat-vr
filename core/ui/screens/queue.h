// Conversions (Settings > Conversions): one row per job, newest last. A running row carries its
// stage and a progress bar, a done row "View" (Select opens the scene in the
// album it was written to) and a failed row its message. Square cancels the
// focused queued or running job; a running one stops at the next stage
// boundary, so the row says "cancels after this photo" until then.
//
// The screen rebuilds its rows in update() from IJobService::snapshot(), so it
// needs no notification plumbing, and asks for a repaint about four times a
// second while a job runs (UiApp also redraws a busy panel, this keeps the
// screen honest when it is driven by a bare ScreenStack).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "ui/screen.h"
#include "ui/services.h"
#include "ui/widgets.h"

namespace splat {

class QueueScreen : public Screen {
public:
    void on_enter(UiContext& ctx, const ScreenArgs& args) override;
    void update(UiContext& ctx, const UiInput& input) override;
    void draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) override;
    std::vector<Hint> hints() const override;
    std::string title() const override { return "Conversions"; }

    // For tests: the rows and the jobs they were built from, in the same order.
    const ListView& list() const { return list_; }
    const std::vector<JobView>& jobs() const { return jobs_; }

    // Seconds between two forced repaints while a job runs.
    static constexpr double kRedrawSeconds = 0.25;

private:
    void refresh(UiContext& ctx);
    void cancel_focused(UiContext& ctx);
    void view_focused(UiContext& ctx);
    int running_index() const;

    ListView list_;
    std::vector<JobView> jobs_;
    double redraw_ = 0.0;
};

}  // namespace splat
