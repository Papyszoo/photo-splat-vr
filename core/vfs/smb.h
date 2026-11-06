// The SMB adapter: one IFileSystem per mounted share, over libsmb2 6.3.
//
// libsmb2 is single-threaded and blocking, so the context is created lazily on
// the first read/listing under the adapter's mutex and every call happens on a
// worker thread (AsyncListing, the thumbnail cache, the job runner). The UI
// thread never reaches this file: it only ever sees vfs:: free functions, which
// is the same layer a local folder goes through.
//
// Errors name the location ("smb0: ...") and carry libsmb2's own message; the
// password is never part of a message or of smb_url().
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>

#include "vfs/io.h"
#include "vfs/location.h"

namespace splat::vfs {

// `smb://[user@]host[:port]/share[/path][?seal]`, for a log line or the
// locations screen. The password is never included.
std::string smb_url(const Location& location);

// The server string `smb2_connect_share()` gets: the bare host for the
// protocol default (445, also what port 0 means), `host:port` otherwise.
// libsmb2 6.3 has no `smb2_set_port()`; it parses the `:port` suffix itself
// (lib/socket.c splits the server string before getaddrinfo), which is how a
// non-default port works on the desktop and on the console. The suffix is
// therefore the whole mechanism, and this function is it in one place so the
// two cannot drift.
std::string smb_connect_host(const Location& location);

// True when a libsmb2 return value means "that name is not there". Only the
// -errno return is read (nterror_to_errno() maps every name-not-found status
// to ENOENT): smb2_get_nterror() keeps the previous reply's status when a call
// fails before a reply, so reading it turns an access-denied answer into "no
// such file" (mac-share/PROBE.md). Public so the tests can pin the rule.
bool smb_missing_name(int status);

// A mounted share, or nullptr with `error` filled. The connection itself is
// made on the first operation (lazy), so a mount of a switched-off NAS still
// returns an adapter and its first listing says "offline". A location with no
// host or share fails here with a message naming the id.
std::shared_ptr<IFileSystem> make_smb_file_system(const Location& location, std::string* error);

// The one line a failed connect produces, in the words the Locations screen
// shows. Three shapes:
//   * `resolve_error` (from core/vfs/resolve.h) when the host is a name this
//     process cannot resolve: the share was never contacted, so blaming it
//     would be a lie (console pass 3, P2).
//   * "wrong user name or password" when libsmb2's message is a refused
//     session setup - servers differ in how much of the reply libsmb2 can
//     parse, so the test is on the message the library produced, and the
//     password itself is never in any of them (smb-e2e, BUG 3).
//   * otherwise "cannot connect to <server>/<share>: <libsmb2 message>".
// Public so tests/vfs_smb_test.cpp can pin the wording without a server.
std::string smb_connect_error(const Location& location, const std::string& server,
                              const std::string& resolve_error, const char* library_error);

}  // namespace splat::vfs
