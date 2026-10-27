// One-line text truncation for the panel: a label that does not fit its box
// ends in an ellipsis instead of running into the next one. Measured with the
// panel font, cut on UTF-8 code point boundaries. Used by the widgets and the
// app chrome; the drawing itself stays in UiCanvas::text().
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "ui/canvas.h"
#include "ui/font.h"

namespace splat {

// Returns `text` when it fits in max_width pixels at `size`, else the longest
// prefix that fits with a trailing "…" ("" when not even that fits).
inline std::string fit_text(std::string_view text, int size, int max_width) {
    if (max_width <= 0) return std::string();
    if (static_cast<int>(UiCanvas::text_width(text, size)) <= max_width) return std::string(text);

    static constexpr const char* kEllipsis = "\xE2\x80\xA6";
    std::vector<std::size_t> cuts;  // code point boundaries
    for (std::size_t index = 0; index < text.size();) {
        ui_font::next_codepoint(text, index);
        cuts.push_back(index);
    }
    std::size_t low = 0;
    std::size_t high = cuts.size();  // number of code points kept, search in [low, high)
    while (low + 1 < high) {
        const std::size_t middle = (low + high) / 2;
        std::string candidate(text.substr(0, cuts[middle - 1]));
        while (!candidate.empty() && candidate.back() == ' ') candidate.pop_back();
        candidate += kEllipsis;
        if (static_cast<int>(UiCanvas::text_width(candidate, size)) <= max_width) {
            low = middle;
        } else {
            high = middle;
        }
    }
    if (low == 0) {
        return static_cast<int>(UiCanvas::text_width(kEllipsis, size)) <= max_width
                   ? std::string(kEllipsis)
                   : std::string();
    }
    std::string out(text.substr(0, cuts[low - 1]));
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out + kEllipsis;
}

// Greedy wrap of `text` into at most `max_lines` lines of `max_width` px:
// breaks after a space, '-' or '_' where possible, else inside a word; the
// last line ends in "…" when text is left over.
inline std::vector<std::string> wrap_lines(std::string_view text, int size, int max_width,
                                           int max_lines) {
    std::vector<std::string> lines;
    std::string_view rest = text;
    while (!rest.empty() && static_cast<int>(lines.size()) < max_lines) {
        if (static_cast<int>(lines.size()) == max_lines - 1 ||
            static_cast<int>(UiCanvas::text_width(rest, size)) <= max_width) {
            lines.push_back(fit_text(rest, size, max_width));
            break;
        }
        // The longest prefix that fits, preferring a break character.
        std::size_t fit = 0;
        std::size_t last_break = 0;
        for (std::size_t index = 0; index < rest.size();) {
            std::size_t next = index;
            ui_font::next_codepoint(rest, next);
            if (static_cast<int>(UiCanvas::text_width(rest.substr(0, next), size)) > max_width) {
                break;
            }
            fit = next;
            const char c = rest[index];
            if (c == ' ' || c == '-' || c == '_') last_break = next;
            index = next;
        }
        if (fit == 0) fit = 1;
        const std::size_t cut = last_break > 0 ? last_break : fit;
        std::string line(rest.substr(0, cut));
        while (!line.empty() && line.back() == ' ') line.pop_back();
        lines.push_back(line);
        rest.remove_prefix(cut);
        while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    }
    return lines;
}

}  // namespace splat
