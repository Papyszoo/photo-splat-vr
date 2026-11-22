// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/screens/licence.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "model/sha256.h"
#include "ui/font.h"
#include "ui/model_service.h"
#include "ui/screen_stack.h"
#include "ui/text_fit.h"

namespace splat {
namespace {

constexpr int kHeaderGap = 18;
constexpr int kFooterGap = 12;
constexpr int kCardPadding = 28;

int text_width(const UiContext& ctx) { return ctx.content.w - 2 * kCardPadding; }

int text_size(const UiTheme& theme) { return theme.body_size - 2; }

int fits(const std::string& text, int size, int width) {
    return static_cast<int>(UiCanvas::text_width(text, size)) <= width;
}

bool starts_with(const std::string& text, const char* prefix) {
    const std::size_t length = std::strlen(prefix);
    return text.size() >= length && text.compare(0, length, prefix) == 0;
}

// The line shown while ModelService fetches the text on its worker.
constexpr char kLicenceLoadingText[] = "Loading the licence text\xE2\x80\xA6";

// The one place the three licence_text() results become screen state: a real
// text (available, hashed), a message starting with kLicenceUnavailablePrefix,
// or the fetch still running (kLicenceLoadingPrefix). The loading flag is set
// here too, so the screen never has to recognise the loading line again.
void apply_licence_text(const std::string& text, int width, int size,
                        std::vector<std::string>* lines, std::string* digest, bool* available,
                        bool* loading) {
    *loading = starts_with(text, kLicenceLoadingPrefix);
    if (*loading) {
        *available = false;
        digest->clear();
        *lines = wrap_licence_text(kLicenceLoadingText, width, size);
        return;
    }
    if (starts_with(text, kLicenceUnavailablePrefix)) {
        *available = false;
        digest->clear();
        *lines = wrap_licence_text(text.substr(std::strlen(kLicenceUnavailablePrefix)), width, size);
        return;
    }
    *available = true;
    *digest = Sha256::of_hex(text.data(), text.size()).substr(0, 12);
    *lines = wrap_licence_text(text, width, size);
}

}  // namespace

std::vector<std::string> wrap_licence_text(const std::string& text, int width_px, int size) {
    const int width = std::max(1, width_px);
    std::vector<std::string> lines;
    std::string line;
    std::string word;

    auto push_line = [&lines, &line] {
        lines.push_back(line);
        line.clear();
    };
    auto flush_word = [&] {
        if (word.empty()) return;
        // A word longer than the line is split, filling whole lines.
        while (!fits(word, size, width)) {
            if (!line.empty()) push_line();
            std::size_t cut = 0;
            std::size_t index = 0;
            while (index < word.size()) {
                std::size_t next = index;
                ui_font::next_codepoint(word, next);
                if (!fits(word.substr(0, next), size, width) && cut > 0) break;
                cut = next;
                index = next;
            }
            if (cut == 0 || cut >= word.size()) break;
            lines.push_back(word.substr(0, cut));
            word.erase(0, cut);
        }
        const std::string joined = line.empty() ? word : line + " " + word;
        if (!line.empty() && !fits(joined, size, width)) {
            push_line();
            line = word;
        } else {
            line = joined;
        }
        word.clear();
    };

    // Licence files are hard-wrapped at ~80 columns: a single line break is a
    // space, so the text reflows to the full box; a blank line still separates
    // paragraphs.
    for (std::size_t at = 0; at < text.size(); ++at) {
        const char raw = text[at];
        const unsigned char character = static_cast<unsigned char>(raw);
        if (character == '\n') {
            std::size_t next = at + 1;
            while (next < text.size() && (text[next] == '\r' || text[next] == ' ' ||
                                          text[next] == '\t')) {
                ++next;
            }
            if (next < text.size() && text[next] == '\n') {
                // A paragraph break: end the line and keep one blank line.
                flush_word();
                push_line();
                push_line();
                while (next < text.size() && (std::isspace(static_cast<unsigned char>(text[next])) != 0)) {
                    ++next;
                }
                at = next - 1;
            } else {
                flush_word();
            }
        } else if (character < 0x80 && std::isspace(character) != 0) {
            flush_word();  // '\r' of a CRLF is whitespace too
        } else {
            word += raw;
        }
    }
    flush_word();
    if (!line.empty()) push_line();
    if (lines.empty()) lines.push_back(std::string());

    // Trailing blank lines (a final newline) would become an empty page.
    while (lines.size() > 1 && lines.back().empty()) lines.pop_back();
    return lines;
}

void LicenceScreen::layout(const Rect& content, const UiTheme& theme) {
    line_height_ = UiCanvas::line_height(text_size(theme)) + 4;
    const int header = UiCanvas::line_height(theme.heading_size) + kHeaderGap;
    const int footer = UiCanvas::line_height(theme.small_size) + kFooterGap;
    const int body_height =
        std::max(line_height_, content.h - header - footer - 2 * kCardPadding + 12);
    visible_ = std::max(1, body_height / line_height_);
    update_pages();
}

void LicenceScreen::update_pages() {
    const std::size_t count = lines_.size();
    pages_ = count == 0 ? 1 : (count + static_cast<std::size_t>(visible_) - 1) /
                                       static_cast<std::size_t>(visible_);
    // The scroll stops on the first line of the last page, so every page is
    // reachable and "Page n/m" always names the text at the top of the panel.
    // A partial last page leaves the rows below its text blank instead of
    // pulling the page marker back one page.
    const int max_scroll = static_cast<int>(pages_ - 1) * visible_;
    scroll_ = std::max(0, std::min(scroll_, max_scroll));
    page_ = static_cast<std::size_t>(scroll_ / visible_) + 1;
}

void LicenceScreen::on_enter(UiContext& ctx, const ScreenArgs& args) {
    model_id_ = args.a;
    model_name_.clear();
    licence_name_.clear();
    digest_prefix_.clear();
    lines_.clear();
    text_available_ = false;
    loading_ = false;
    scroll_ = 0;

    if (ctx.svc.models == nullptr) {
        lines_ = wrap_licence_text("Models are not available in this build.", text_width(ctx),
                                   text_size(ctx.theme));
        layout(ctx.content, ctx.theme);
        return;
    }

    for (const ModelView& view : ctx.svc.models->list()) {
        if (view.id != model_id_) continue;
        model_name_ = view.name;
        licence_name_ = view.licence_name;
        break;
    }

    const std::string text = ctx.svc.models->licence_text(model_id_);
    apply_licence_text(text, text_width(ctx), text_size(ctx.theme), &lines_, &digest_prefix_,
                       &text_available_, &loading_);
    layout(ctx.content, ctx.theme);
}

void LicenceScreen::accept_and_download(UiContext& ctx) {
    if (!text_available_) {
        ctx.toast(loading_ ? "The licence text is still loading"
                           : "The licence text is not available; nothing was accepted");
        return;
    }
    const Status accepted = ctx.svc.models->accept(model_id_);
    if (!accepted.ok()) {
        ctx.toast(accepted.message());
        return;
    }
    const Status started = ctx.svc.models->download(model_id_);
    ctx.toast(started.ok() ? "Accepted " + model_name_ + "; downloading" : started.message());
    ctx.nav.pop();
}

void LicenceScreen::update(UiContext& ctx, const UiInput& input) {
    layout(ctx.content, ctx.theme);

    // The text is fetched on ModelService's worker, so this screen never waits
    // for the network or for a wedged download's body read. Ask again each
    // frame while the loading flag is set; a real text or an error then
    // replaces it. An error stays until the screen is entered again.
    if (loading_ && ctx.svc.models != nullptr) {
        const std::string text = ctx.svc.models->licence_text(model_id_);
        if (!starts_with(text, kLicenceLoadingPrefix)) {
            apply_licence_text(text, text_width(ctx), text_size(ctx.theme), &lines_, &digest_prefix_,
                               &text_available_, &loading_);
            layout(ctx.content, ctx.theme);
            ctx.invalidate();
        }
    }

    if ((input.pressed & Back) != 0) {
        // UiApp pops on Circle before the screen sees it; this keeps the screen
        // correct when a host feeds it directly.
        ctx.nav.pop();
        return;
    }
    if ((input.pressed & Up) != 0) --scroll_;
    if ((input.pressed & Down) != 0) ++scroll_;
    if ((input.pressed & PagePrev) != 0) scroll_ -= visible_;
    if ((input.pressed & PageNext) != 0) scroll_ += visible_;
    update_pages();

    if ((input.pressed & Select) != 0) accept_and_download(ctx);
}

void LicenceScreen::draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) {
    layout(content, ctx.theme);
    const UiTheme& theme = ctx.theme;

    // The text sits on one card, the model and licence name above it.
    int y = content.y;
    std::string header = model_name_;
    if (!licence_name_.empty()) {
        if (!header.empty()) header += "  \xC2\xB7  ";
        header += licence_name_;
    }
    canvas.text(content.x, y, fit_text(header, theme.heading_size, content.w - 160), theme.heading_size,
                theme.text);
    if (pages_ > 1) {
        const std::string page = "Page " + std::to_string(page_) + " of " + std::to_string(pages_);
        const int page_width = static_cast<int>(UiCanvas::text_width(page, theme.small_size));
        canvas.text(content.right() - page_width,
                    y + UiCanvas::line_height(theme.heading_size) -
                        UiCanvas::line_height(theme.small_size),
                    page, theme.small_size, theme.dim_text);
    }

    y += UiCanvas::line_height(theme.heading_size) + kHeaderGap;
    const int card_bottom = content.bottom() - UiCanvas::line_height(theme.small_size) - kFooterGap;
    canvas.fill_round_rect(content.x, y, content.w, card_bottom - y, theme.radius, theme.panel);
    y += kCardPadding - 6;
    const int last = std::min(static_cast<int>(lines_.size()), scroll_ + visible_);
    for (int index = scroll_; index < last; ++index) {
        canvas.text(content.x + kCardPadding, y, lines_[static_cast<std::size_t>(index)],
                    text_size(theme), Rgba{226, 232, 242, 255});
        y += line_height_;
    }
}

std::vector<Hint> LicenceScreen::hints() const {
    if (pages_ > 1) {
        return {Hint{"Cross", "Accept and download"}, Hint{"L1/R1", "Page"},
                Hint{"Circle", "Decline"}};
    }
    return {Hint{"Cross", "Accept and download"}, Hint{"Circle", "Decline"}};
}

}  // namespace splat
