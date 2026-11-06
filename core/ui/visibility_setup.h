// Installs the process-wide "Visible folders" policy at boot.
//
// One call, once, right after UiServices::paths and UiServices::log exist and
// BEFORE the JobService worker starts: a job resumed at boot writes a scene's
// provenance immediately, and it must read the policy the owner saved, not the
// all-visible default. Any other host that constructs a JobService calls this
// too; a unit-test fixture installs a Visibility directly (VisibilityScope).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>

#include "common/status.h"
#include "gallery/visibility.h"
#include "ui/screen.h"

namespace splat {

// Builds the context from UiPaths, loads <state>/visible-folders.json, installs
// it and logs one line: "ui: visible folders: off", "... passcode on, <n>
// ticked" or "... state damaged; fail closed". A missing file is no passcode.
//
// A load that answers an error is not a reason to abort the title: the boot
// path installs the fail-closed policy and logs why (apply_visibility_result).
void install_visibility(const UiPaths& paths, const std::function<void(const std::string&)>& log);

// The half of install_visibility() that acts on a load result, exposed so the
// error path is testable without a state folder that can make the loader fail.
// An error result is logged ("ui: visible folders: <message>; fail closed") and
// installs Visibility::fail_closed(context); a value is installed as it is.
// Never throws, never aborts: this runs before the first frame of a title.
void apply_visibility_result(Result<Visibility> loaded, VisibilityContext context,
                             const std::function<void(const std::string&)>& log);

}  // namespace splat
