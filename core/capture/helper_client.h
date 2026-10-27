#pragma once

// W5. Title-side client for the splat-helper file mailbox (helper_protocol.h).
// Non-blocking: request_*() writes req.json, poll() (once per UI frame) reads
// the answer, and a 10 s deadline turns a silent helper into Failed. No sce
// calls; the one PS5-only function (start_helper) is guarded by PS5_TITLE.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <string>
#include <vector>

#include "capture/helper_protocol.h"
#include "common/path.h"
#include "common/status.h"

namespace splat {

class HelperClient {
public:
    enum class State { Idle, Waiting, Done, Failed, NoHelper };

    struct Timing {
        double timeout_seconds = 10.0;
        double alive_limit_seconds = 6.0;
    };

    explicit HelperClient(Path helper_dir);
    HelperClient(Path helper_dir, Timing timing);

    // <dir>/alive is fresh (6 s by default).
    bool alive() const;

    // Both return an error Status without sending anything when the helper is
    // not running; state() is then NoHelper.
    Status request_list();
    Status request_copy(std::vector<Path> paths);

    State state() const { return state_; }
    // One frame step: picks up the answer for the pending request.
    void poll();
    const std::vector<CaptureInfo>& captures() const { return captures_; }
    const std::vector<Path>& copied() const { return copied_; }
    std::string error() const { return error_; }

private:
    Status send(const std::string& body);
    void handle_response(const std::string& text);
    bool response_matches(const std::string& text) const;

    Path helper_dir_;
    Timing timing_;
    State state_ = State::Idle;
    long long next_id_ = 0;
    long long pending_id_ = -1;
    std::chrono::steady_clock::time_point deadline_{};
    std::vector<CaptureInfo> captures_;
    std::vector<Path> copied_;
    std::string error_;
};

// Push a bundled payload ELF to elfldr on 127.0.0.1:9021 (raw ELF, ref/elfldr/
// socksrv.c). PS5_TITLE only; on the host there is no loader and this returns
// false, which the Captures screen reports as "needs the payload loader".
bool start_helper(const Path& elf);

}  // namespace splat
