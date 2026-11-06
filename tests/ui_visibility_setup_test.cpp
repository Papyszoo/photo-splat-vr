// The boot path's visible-folders install (core/ui/visibility_setup.cpp), and
// in particular the error half: the 0.5.0 dev title's first new boot step with
// UI=1 was this call, and it read load_visibility(...).value() without an ok()
// check - a state file that ever answered an error would have aborted the title
// with no log line and no flush (CRASH-050.md section 2, candidate 1).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "gallery/visibility.h"
#include "test_util.h"
#include "ui/screen.h"
#include "ui/visibility_setup.h"

namespace {

splat::VisibilityContext console_context() {
    splat::VisibilityContext context;
    context.scenes_root = "/data/ps5-splat-vr/scenes";
    context.inbox = "/data/ps5-splat-vr/inbox";
    context.data = "/data";
    context.app_data = "/data/ps5-splat-vr";
    return context;
}

}  // namespace

TEST_CASE("visibility: a load error fails closed with a log line instead of aborting") {
    const splat::VisibilityContext context = console_context();
    // The process-wide slot is restored when the case ends, whatever it held.
    splat::VisibilityScope scope(splat::Visibility::all_visible(context));

    std::vector<std::string> lines;
    const auto log = [&lines](const std::string& text) { lines.push_back(text); };

    // The error a state folder can answer one day: the boot path must not call
    // value() on it. Nothing here may throw or abort.
    CHECK_NOTHROW(splat::apply_visibility_result(
        splat::Result<splat::Visibility>::failure("cannot read state/visible-folders.json"),
        context, log));

    REQUIRE(lines.size() == 1);
    // The reason is in the log, so the next line names the cause; and the
    // decision is fail closed, not "no passcode".
    CHECK(lines[0].find("cannot read state/visible-folders.json") != std::string::npos);
    CHECK(lines[0].find("fail closed") != std::string::npos);

    const std::shared_ptr<const splat::Visibility> installed = splat::current_visibility();
    CHECK(installed->enabled);
    CHECK(installed->damaged);
    CHECK(installed->allowed.empty());
    // Fail closed means every folder is refused, including the app's own roots.
    CHECK_FALSE(installed->allows(splat::VisibilityKey::console_folder("/data/ps5-splat-vr")));
    CHECK_FALSE(installed->allows(splat::VisibilityKey::console_folder("/data/Photos")));
    CHECK_FALSE(installed->allows_album("00_v02_console"));
    CHECK_FALSE(installed->allows_unsorted());
}

TEST_CASE("visibility: a loaded policy is installed as it is, and logged") {
    const splat::VisibilityContext context = console_context();
    splat::VisibilityScope scope(splat::Visibility::all_visible(context));

    std::vector<std::string> lines;
    const auto log = [&lines](const std::string& text) { lines.push_back(text); };

    // A value that carries a passcode and one ticked folder: installed unchanged.
    splat::Visibility state = splat::Visibility::fail_closed(context);
    state.damaged = false;
    state.allowed = {splat::VisibilityKey::console_folder("/data/Photos")};
    splat::apply_visibility_result(splat::Result<splat::Visibility>::success(state), context, log);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "ui: visible folders: passcode on, 1 ticked");
    CHECK(splat::current_visibility()->allows(
        splat::VisibilityKey::console_folder("/data/Photos")));

    // The all-visible policy (no file at all) says so and installs unchanged.
    lines.clear();
    splat::apply_visibility_result(
        splat::Result<splat::Visibility>::success(splat::Visibility::all_visible(context)),
        context, log);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "ui: visible folders: off");
    CHECK_FALSE(splat::current_visibility()->enabled);
}

TEST_CASE("visibility: install_visibility boots on a state folder that has no file") {
    splat_test::TempDir dir("visibility-setup");
    // install_visibility() sets the process-wide policy: restore whatever was
    // there before this case ends, or the next case runs under this one's.
    splat::VisibilityScope scope(splat::Visibility::all_visible(splat::VisibilityContext{}));
    splat::UiPaths paths;
    paths.scenes = dir.path() / "scenes";
    paths.inbox = dir.path() / "inbox";
    paths.state = dir.path() / "state";
    paths.mnt = dir.path() / "mnt";
    paths.data = dir.path() / "data";
    std::filesystem::create_directories(paths.state);

    std::vector<std::string> lines;
    CHECK_NOTHROW(splat::install_visibility(paths, [&lines](const std::string& text) {
        lines.push_back(text);
    }));
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "ui: visible folders: off");
    CHECK_FALSE(splat::current_visibility()->enabled);
}

TEST_CASE("visibility: a policy file past the byte cap fails closed instead of being read") {
    splat_test::TempDir dir("visibility-oversized");
    splat::VisibilityScope scope(splat::Visibility::all_visible(splat::VisibilityContext{}));
    splat::UiPaths paths;
    paths.scenes = dir.path() / "scenes";
    paths.inbox = dir.path() / "inbox";
    paths.state = dir.path() / "state";
    paths.mnt = dir.path() / "mnt";
    paths.data = dir.path() / "data";
    std::filesystem::create_directories(paths.state);

    // A file the loader must not read whole: the cap answers fail closed, the
    // same answer as an unreadable file, and the boot path keeps going.
    {
        std::ofstream out(paths.state / "visible-folders.json", std::ios::binary | std::ios::trunc);
        const std::string block(64 << 10, 'x');
        for (int i = 0; i < 5; ++i) {
            out.write(block.data(), static_cast<std::streamsize>(block.size()));
        }
    }

    std::vector<std::string> lines;
    CHECK_NOTHROW(splat::install_visibility(paths, [&lines](const std::string& text) {
        lines.push_back(text);
    }));
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "ui: visible folders: state damaged; fail closed");
    CHECK(splat::current_visibility()->enabled);
    CHECK(splat::current_visibility()->damaged);
    CHECK(splat::current_visibility()->allowed.empty());
}
