// The build's screen registry. Every screen brief after W7 adds exactly one
// line here; an unregistered id stays a "not available yet" toast.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui/screen_stack.h"

namespace splat {

void register_default_screens(ScreenRegistry& registry);

}  // namespace splat
