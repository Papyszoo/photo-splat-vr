// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/visibility_setup.h"

#include <string>
#include <utility>

#include "gallery/visibility.h"

namespace splat {

void apply_visibility_result(Result<Visibility> loaded, VisibilityContext context,
                             const std::function<void(const std::string&)>& log) {
    if (!loaded.ok()) {
        // A boot path must not abort the title because a state file answered an
        // error: fail closed (nothing ticked, no passcode usable) and say so, so
        // the next line in the log names the cause instead of a SIGABRT.
        if (log) log("ui: visible folders: " + loaded.message() + "; fail closed");
        set_visibility(Visibility::fail_closed(std::move(context)));
        return;
    }
    Visibility state = loaded.take();
    if (log) {
        if (state.damaged) {
            log("ui: visible folders: state damaged; fail closed");
        } else if (!state.enabled) {
            log("ui: visible folders: off");
        } else {
            log("ui: visible folders: passcode on, " + std::to_string(state.allowed.size()) +
                " ticked");
        }
    }
    set_visibility(std::move(state));
}

void install_visibility(const UiPaths& paths, const std::function<void(const std::string&)>& log) {
    VisibilityContext context;
    context.scenes_root = paths.scenes;
    context.inbox = paths.inbox;
    context.mnt = paths.mnt;
    context.data = paths.data;
    // paths.inbox.parent_path() is the app's own folder: never a tickable
    // console row, and the reason a file under it maps to "other".
    context.app_data = paths.inbox.parent_path();

    // load_visibility() answers a value for every file it can classify (a
    // missing file is all_visible, an unusable one fail_closed), but its result
    // is still checked: the day it grows a real error, the boot path must log it
    // and fail closed instead of calling value() on it.
    Result<Visibility> loaded = load_visibility(paths.state, context);
    apply_visibility_result(std::move(loaded), std::move(context), log);
}

}  // namespace splat
