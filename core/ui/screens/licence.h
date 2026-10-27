// The licence screen: the model's licence text, fetched at runtime, wrapped to
// the panel width, scrolled with Up/Down and paged with L1/R1. Select accepts
// the exact text shown and starts the download; Circle (the app's Back) leaves
// without accepting. The text itself is never stored in the repository; the
// screen keeps the SHA-256 prefix of the text the manager will record (for the
// tests and the log, not drawn). The page marker and the L1/R1 prompt only show
// when there is more than one page.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ui/screen.h"

namespace splat {

// Greedy word wrap at `width_px` for the panel font at `size` px, measured
// with UiCanvas::text_width. Words longer than a line are split on code point
// boundaries; '\n' starts a new line, runs of spaces/tabs collapse. Never
// returns an empty vector.
std::vector<std::string> wrap_licence_text(const std::string& text, int width_px, int size);

class LicenceScreen : public Screen {
public:
    void on_enter(UiContext& ctx, const ScreenArgs& args) override;
    void update(UiContext& ctx, const UiInput& input) override;
    void draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) override;
    std::vector<Hint> hints() const override;
    std::string title() const override { return "Licence"; }

    // Read-only accessors for the tests.
    const std::string& model_id() const { return model_id_; }
    const std::string& model_name() const { return model_name_; }
    const std::string& licence_name() const { return licence_name_; }
    const std::vector<std::string>& lines() const { return lines_; }
    const std::string& digest_prefix() const { return digest_prefix_; }
    bool text_available() const { return text_available_; }
    int scroll() const { return scroll_; }
    int visible_lines() const { return visible_; }
    std::size_t page() const { return page_; }
    std::size_t pages() const { return pages_; }

private:
    void layout(const Rect& content, const UiTheme& theme);
    void update_pages();
    void accept_and_download(UiContext& ctx);

    std::string model_id_;
    std::string model_name_;
    std::string licence_name_;
    std::string digest_prefix_;
    std::vector<std::string> lines_;
    bool text_available_ = false;
    int scroll_ = 0;
    int visible_ = 1;
    int line_height_ = 24;
    std::size_t page_ = 1;
    std::size_t pages_ = 1;
};

}  // namespace splat
