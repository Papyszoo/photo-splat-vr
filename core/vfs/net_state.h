// The process's network stack, as the network adapters see it.
//
// A PS5 title must call sceNetInit() before any socket is opened. That call
// lives in platform/ps5/viewer/net_init.cpp (a console service cannot appear in
// the desktop build), which reports its result here; the SMB and NFS adapters
// read it before a connect and put the stack's own words on the location's
// offline line instead of libsmb2's "cannot connect" (console pass 3, P2: a
// dead stack must be named as such, not blamed on the share).
//
// The desktop build never sets it, so net_stack_error() is "" and every adapter
// behaves exactly as before. A test sets it to drive the console's failure.
//
// The retry hook is the same idea for L1: a sceNetInit() that failed at boot is
// not cached for the life of the process. The viewer registers a hook that asks
// the stack to come up again, and an adapter calls it before reporting offline,
// so a location that failed once can be retried in the same run (the mount and
// the screen's Test reach the adapter's reset_connection() and then this).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>

namespace splat::vfs {

// "" when the stack is up (or nothing has reported a stack at all), otherwise
// the sentence that names the stack failure.
std::string net_stack_error();

// Records the stack state. "" clears it. Called by the platform's once-per-
// process init (and by tests).
void set_net_stack_error(std::string error);

// The platform's "start the stack now" hook, or empty when the build has none.
void set_net_stack_retry(std::function<void()> retry);

// Asks the registered hook to bring the stack up again and returns true when it
// is up afterwards. False (and no side effect) when no hook is registered.
bool retry_net_stack();

}  // namespace splat::vfs
