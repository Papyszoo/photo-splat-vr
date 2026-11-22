// Issue #2: the TV card shown when a firmware has no libSceHmd2 /
// libSceVrTracker2 export table. The text, the firmware formatting and the
// library file list are pure (platform/ps5/viewer/firmware_message.h), so the
// exact words the user asked for are checked here without a console; the same
// header draws the card with the TV path's own canvas text, and the test writes
// that 1920x1080 image for review.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#include "../platform/ps5/viewer/firmware_message.h"
#include "data_dir.h"

TEST_CASE("the firmware card words the version like the klog") {
    // kern.sdk_version is BCD per byte, as the console reports it and main.cpp
    // prints it: 10.01 is 0x10010000, 13.60 is 0x13600007, 7.61 is 0x07610001.
    // Two minor digits, like the log.
    CHECK(viewer::format_firmware_version(0x10010000u) == "10.01");
    CHECK(viewer::format_firmware_version(0x13600007u) == "13.60");
    CHECK(viewer::format_firmware_version(0x07610001u) == "7.61");
    CHECK(viewer::format_firmware_version(0x05500000u) == "5.50");
    // Unknown: no number is invented.
    CHECK(viewer::format_firmware_version(0u).empty());

    CHECK(viewer::firmware_message(0x10010000u, "libSceHmd2").title ==
          "PSVR2 is not supported on firmware 10.01 yet");
    // No number is invented when the version is unknown.
    CHECK(viewer::firmware_message(0u, "libSceHmd2").title ==
          "PSVR2 is not supported on this firmware yet");
}

TEST_CASE("the firmware card is the user's text") {
    const viewer::FirmwareMessage message =
        viewer::firmware_message(0x10010000u, "libSceHmd2");
    REQUIRE(message.paragraphs.size() == 3);
    CHECK(message.paragraphs[0] ==
          "To add support, dump the system libraries with ps5-self-pager "
          "(ps5-self-pager-system-common-lib.elf, github.com/idlesauce/ps5-self-pager), then open "
          "an issue at github.com/Papyszoo/photo-splat-vr with your firmware version.");
    CHECK(message.paragraphs[1] ==
          "Needed files: libSceHmd2.sprx and libSceVrTracker2.sprx from "
          "dump/system/common/lib/ (on USB or in /data/dump).");
    CHECK(message.paragraphs[2] ==
          "Do not attach them to the issue. They are Sony system files: share a download link "
          "privately when asked.");
    CHECK(message.exit_hint == "Press X or O to exit.");
}

TEST_CASE("the firmware card always lists both library files") {
    // The dump tool writes both, and when libSceHmd2 fails libSceVrTracker2 is
    // never checked: whichever table was missing, the card asks for both.
    const char* const missing[] = {"libSceHmd2", "libSceVrTracker2", "something else"};
    for (const char* library : missing) {
        CAPTURE(library);
        CHECK(viewer::firmware_message_filenames(library) ==
              "libSceHmd2.sprx and libSceVrTracker2.sprx");
    }
    CHECK(viewer::firmware_message_filenames(nullptr) ==
          "libSceHmd2.sprx and libSceVrTracker2.sprx");
}

TEST_CASE("the firmware card fits and draws on the 1920x1080 TV canvas") {
    const viewer::FirmwareMessage message =
        viewer::firmware_message(0x10010000u, "libSceHmd2");
    const splat::UiTheme theme = splat::scaled_theme(splat::default_theme(), 1.5f);
    const int width = 1920 - 2 * theme.margin;
    // The layout draws at most six body lines per paragraph; a paragraph that
    // needs more would lose its end to the wrapper's ellipsis. Five is the
    // measured maximum, so the text is complete above the exit hint.
    for (const std::string& paragraph : message.paragraphs) {
        CHECK(splat::wrap_lines(paragraph, theme.body_size, width, 100).size() <= 5);
    }

    splat::UiCanvas canvas(1920, 1080);
    viewer::draw_firmware_message(canvas, message);
    REQUIRE(canvas.pixels().size() == 1920u * 1080u * 4u);
    bool opaque = true;
    size_t text_pixels = 0;
    const splat::Rgba background = theme.background;
    for (size_t i = 0; i < canvas.pixels().size(); i += 4) {
        if (canvas.pixels()[i + 3] != 255) {
            opaque = false;
            break;
        }
        if (canvas.pixels()[i] != background.r || canvas.pixels()[i + 1] != background.g ||
            canvas.pixels()[i + 2] != background.b) {
            ++text_pixels;
        }
    }
    CHECK(opaque);
    CHECK(text_pixels > 2000);                            // real glyphs ...
    CHECK(text_pixels < canvas.pixels().size() / 4 / 2);  // ... not a filled screen

    // Review artefact for the issue (the data dir's scratch space, never the
    // repo): the exact 1920x1080 image the title presents.
    const std::filesystem::path out = splat_test::data_path("runs/issue-1-crash/fwmsg.png");
    std::error_code error;
    std::filesystem::create_directories(out.parent_path(), error);
    const splat::Status written = canvas.write_png(out);
    CHECK(written.ok());
    MESSAGE("wrote " << out);
}
