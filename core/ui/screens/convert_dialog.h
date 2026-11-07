// The model choice after the photo picker: the photos (ScreenArgs::list) go to
// the folder they were picked for (ScreenArgs::a, "" = the loose scenes), and
// this screen asks only which installed model converts them.
//
// The left column shows the first photo and how many there are; the right one
// lists the models the model service reports as Ready (the remembered one
// first). Cross on a model queues one job per photo into the folder, remembers
// the model (IModelService::select) and returns to the folder, where the new
// tiles show their progress.
//
// Missing pieces are said, not hidden: with no Ready model the list offers
// "Open Models" (Settings > Models), and a null job service makes the screen
// say "Conversion is not available on this build".
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "common/path.h"
#include "ui/screen.h"
#include "ui/thumb_cache.h"
#include "ui/widgets.h"

namespace splat {

class ConvertDialog : public Screen {
public:
    void on_enter(UiContext& ctx, const ScreenArgs& args) override;
    void update(UiContext& ctx, const UiInput& input) override;
    void draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) override;
    std::vector<Hint> hints() const override;
    std::string title() const override { return "Choose a model"; }

    // For the tests and the preview.
    const ListView& list() const { return list_; }
    const std::vector<Path>& photos() const { return photos_; }
    // The same photos with their quarter turns, which is what the job service
    // is given.
    const std::vector<PhotoPick>& picks() const { return picks_; }
    // The folder the jobs go to ("" = the loose-scene album).
    const std::string& album() const { return album_; }
    // The library root they go to when it is a network location (the shelf's own
    // or the Output folder); empty = this console's scene root.
    const Path& output_root() const { return output_root_; }
    // The Ready models in list order; empty when none is ready.
    const std::vector<ModelView>& models() const { return ready_; }
    // The model the focused row would queue with ("" when none is ready).
    std::string model_id() const;
    // "Conversion is not available on this build" without a job service, or
    // "That folder is not visible" when the visible-folders policy hides the
    // album, the destination or one of the picks (stage H).
    std::string message() const;
    // Re-read each frame: a passcode may be set while this dialog is open.
    bool blocked() const { return blocked_; }
    // The destination line the left column draws; empty while blocked, so a
    // hidden album's name is never drawn.
    std::string folder_line() const;
    // False while blocked: the first pick's thumbnail is not held (a hidden
    // photo must not stay on screen through a dialog left open).
    bool preview_ready() const { return !preview_.empty(); }

private:
    Rect list_area(const Rect& content) const;
    void refresh_rows(UiContext& ctx);
    void start(UiContext& ctx);
    // Re-evaluates the policy for this dialog's album, destination and picks.
    void refresh_blocked();

    std::vector<Path> photos_;
    std::vector<PhotoPick> picks_;
    std::string album_;
    // Set when the folder is on a network location (ScreenArgs::b): the runner
    // builds the scene locally and uploads it to this root.
    Path output_root_;
    // The destination in words when it is on a network location ("NAS > Splats"),
    // empty for the local library; and why the Output folder cannot be used.
    std::string destination_text_;
    std::string target_error_;
    std::vector<ModelView> ready_;
    bool jobs_available_ = false;
    bool blocked_ = false;
    ListView list_;
    ThumbCache thumbs_{4, 4};
    UiImage preview_;
};

}  // namespace splat
