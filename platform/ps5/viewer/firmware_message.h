// The TV card shown when VR cannot start because this firmware has no
// libSceHmd2 / libSceVrTracker2 export table (issue #2, firmware 10.01): the
// text and the drawing of the card on the TV path's own panel canvas
// (ui/canvas.h, ui/theme.h, ui/text_fit.h) - no renderer, no Hmd2 and no second
// font.
//
// The text is the user's decision of 2026-11-21: English, short, neutral, no
// developer name and no mention of AI. The "Needed files" line always names
// both system libraries (the dump tool writes both, and when libSceHmd2 fails
// libSceVrTracker2 is never checked). They are Sony's: the card asks for them
// by file name and says not to attach them.
//
// Header-only over core/ui, so tests/firmware_message_test.cpp draws the exact
// card the title shows on the console.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "firmware_version.h"
#include "ui/canvas.h"
#include "ui/text_fit.h"
#include "ui/theme.h"

namespace viewer {

struct FirmwareMessage {
    std::string title;
    std::vector<std::string> paragraphs;  // one entry per paragraph
    std::string exit_hint;
};

// Both library files, whichever table was missing: when libSceHmd2 has no
// table libSceVrTracker2 is never checked, and the dump writes both anyway.
// The parameter stays because hmd2 reports the failed library
// (Error::missing_table_library) and the caller already passes it.
inline std::string firmware_message_filenames(const char* /*missing_library*/) {
    return "libSceHmd2.sprx and libSceVrTracker2.sprx";
}

inline FirmwareMessage firmware_message(uint32_t sdk_version, const char* missing_library) {
    const std::string version = format_firmware_version(sdk_version);
    FirmwareMessage message;
    message.title = version.empty() ? "PSVR2 is not supported on this firmware yet"
                                    : "PSVR2 is not supported on firmware " + version + " yet";
    message.paragraphs.push_back(
        "To add support, dump the system libraries with ps5-self-pager "
        "(ps5-self-pager-system-common-lib.elf, github.com/idlesauce/ps5-self-pager), then open "
        "an issue at github.com/Papyszoo/photo-splat-vr with your firmware version.");
    message.paragraphs.push_back("Needed files: " + firmware_message_filenames(missing_library) +
                                 " from dump/system/common/lib/ (on USB or in /data/dump).");
    message.paragraphs.push_back(
        "Do not attach them to the issue. They are Sony system files: share a download link "
        "privately when asked.");
    message.exit_hint = "Press X or O to exit.";
    return message;
}

// Draws the card on a 1920x1080 canvas: the panel's theme at 1.5x (the TV image
// is 1.5 times the 1280x720 design) with the panel font, wrapping and metrics.
// The body is clipped to the canvas by every draw; the layout leaves the
// bottom band for the exit hint.
inline void draw_firmware_message(splat::UiCanvas& canvas, const FirmwareMessage& message) {
    const splat::UiTheme theme = splat::scaled_theme(splat::default_theme(), 1.5f);
    canvas.clear(theme.background);
    const int left = theme.margin;
    const int width = static_cast<int>(canvas.width()) - 2 * theme.margin;
    int y = theme.margin;
    canvas.text(left, y, message.title, theme.title_size, theme.text);
    y += splat::UiCanvas::line_height(theme.title_size) + theme.padding * 2;
    for (const std::string& paragraph : message.paragraphs) {
        for (const std::string& line_text :
             splat::wrap_lines(paragraph, theme.body_size, width, 6)) {
            canvas.text(left, y, line_text, theme.body_size, theme.dim_text);
            y += splat::UiCanvas::line_height(theme.body_size) + theme.padding / 2;
        }
        y += theme.padding;
    }
    const int hint_y = static_cast<int>(canvas.height()) - theme.margin -
                       splat::UiCanvas::line_height(theme.body_size);
    canvas.text(left, hint_y, message.exit_hint, theme.body_size, theme.text);
}

}  // namespace viewer
