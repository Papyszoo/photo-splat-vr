// splat-helper: resident payload that answers the viewer title's file mailbox.
// A title cannot see /user/av_contents (docs/research/browse-ui.md 1.3); this
// payload can, so it lists captures and copies the picked JPG/PNG ones into the
// normal inbox. No sockets: req.json in, resp.json out, alive rewritten every
// 2 s, stop file to exit (protocol: core/capture/helper_protocol.h).
//
// The mailbox directory, the pid lock and the inbox are all under the data
// folder of the identity this ELF was built as (SPLAT_DATA_ROOT, from
// platform/ps5/title/identities.json through this directory's Makefile). The
// title that pushes this ELF was built as that same identity, so a dev helper
// and a release helper on one console each have their own lock and mailbox and
// cannot answer or copy for the other install.
//
//   helper: start ...
//   helper: request answered ...
//   helper: stop file, exiting
//
// Lines go to stdout (elfldr streams them back over 9021) and to the kernel log
// via klog_puts, like platform/ps5/hello.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <ps5/klog.h>
#include <signal.h>
#include <unistd.h>

#include "capture/helper_protocol.h"

#ifndef SPLAT_DATA_ROOT
#error "build with make -C platform/ps5/helper: it reads the identity's dataDir (identities.json)"
#endif
#ifndef SPLAT_TITLE_ID
#error "build with make -C platform/ps5/helper: it reads the identity's titleId (identities.json)"
#endif

extern "C" int sceKernelUsleep(unsigned int usec);

namespace {

namespace fs = std::filesystem;
using splat::Path;

// The identity's one data folder (the dev build's test folder, or the release
// build's own private one), never both, never a second hard-coded copy.
const Path kDataRoot = Path(SPLAT_DATA_ROOT);
const Path kHelperDir = kDataRoot / "helper";
const Path kInboxDir = kDataRoot / "inbox";
const Path kRequestPath = kHelperDir / splat::kHelperRequestName;
const Path kResponsePath = kHelperDir / splat::kHelperResponseName;
const Path kAlivePath = kHelperDir / splat::kHelperAliveName;
const Path kLockPath = kHelperDir / "helper.pid";
const Path kStopPath = kHelperDir / "stop";

void
line(const char *fmt, ...) {
    char    msg[512];
    va_list args;

    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    std::printf("%s\n", msg);
    std::fflush(stdout);
    klog_puts(msg);
}

// resp.json and alive are read by another process: write a temporary and rename
// so a reader never sees half a file.
bool
write_atomic(const Path &path, const std::string &text) {
    std::error_code  error;
    const Path       temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out) return false;
    }
    fs::rename(temporary, path, error);
    return !error;
}

std::string
read_file(const Path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// One instance at a time: helper.pid holds the owner's pid, and the payload
// exits after logging when that process is still alive. A stale pid (crash,
// reboot) is taken over.
bool
acquire_lock() {
    std::error_code error;
    fs::create_directories(kHelperDir, error);
    if (error) {
        line("helper: cannot create %s: %s", kHelperDir.string().c_str(), error.message().c_str());
        return false;
    }

    long long owner = 0;
    {
        std::ifstream in(kLockPath);
        in >> owner;
    }
    if (owner > 0 && owner != static_cast<long long>(::getpid()) &&
        ::kill(static_cast<pid_t>(owner), 0) == 0) {
        line("helper: already running (pid %lld), exiting", owner);
        return false;
    }

    std::ofstream out(kLockPath, std::ios::trunc);
    if (!out) {
        line("helper: cannot write %s", kLockPath.string().c_str());
        return false;
    }
    out << static_cast<long long>(::getpid());
    return true;
}

void
beat() {
    if (!write_atomic(kAlivePath, std::to_string(static_cast<long long>(std::time(nullptr)))))
        line("helper: cannot write the heartbeat");
}

// Answer one req.json, if there is one. The helper never deletes the user's
// files; it removes only its own request file.
void
answer_request() {
    std::error_code error;
    if (!fs::exists(kRequestPath, error)) return;

    const std::string request = read_file(kRequestPath);
    if (request.empty()) {
        // The title writes req.json atomically (rename), so an empty file is
        // never a request in progress: drop our own mailbox file instead of
        // logging it again every tick.
        fs::remove(kRequestPath, error);
        line("helper: empty request removed");
        return;
    }

    const Path        root = splat::helper_root(kHelperDir);
    const std::string response = splat::handle_request(request, root, kInboxDir);
    if (!write_atomic(kResponsePath, response)) {
        // Keep req.json so the next 500 ms tick retries instead of losing it.
        line("helper: cannot write the response, will retry");
        return;
    }
    fs::remove(kRequestPath, error);
    line("helper: request answered (%zu bytes, root %s)", response.size(), root.string().c_str());
}

}  // namespace

int
main() {
    line("helper: start (%s, capture mailbox in %s)", SPLAT_TITLE_ID, kHelperDir.string().c_str());

    if (!acquire_lock()) return 0;

    // A stop file from an earlier run must not end this one immediately.
    std::error_code error;
    fs::remove(kStopPath, error);
    fs::remove(kResponsePath, error);
    beat();
    std::time_t last_beat = std::time(nullptr);

    bool stopping = false;
    while (!stopping) {
        if (fs::exists(kStopPath, error)) {
            line("helper: stop file, exiting");
            stopping = true;
            break;
        }
        const std::time_t now = std::time(nullptr);
        if (now - last_beat >= 2) {
            beat();
            last_beat = now;
        }
        answer_request();
        sceKernelUsleep(500 * 1000);
    }

    // Leave the mailbox clean: a fresh start has no stale answer, and the
    // client sees "helper not running" instead of an old heartbeat.
    fs::remove(kAlivePath, error);
    fs::remove(kLockPath, error);
    fs::remove(kStopPath, error);
    line("helper: done, exit 0");
    return 0;
}
