// W23. CaptureService: HelperClient -> ICaptureService (capture_service.h).
//
// One frame step is poll() on the mailbox plus whatever that answer means:
// a list answer becomes CaptureView rows, a copy answer becomes the "copied N"
// status line. Nothing here waits. The mailbox is polled at
// poll_interval_seconds (4 Hz) instead of once per frame, so a frame costs an
// add; copy_to_inbox() sends req.json and returns, and tick() picks the answer
// up later, which keeps the frame loop (and the VR headset image) moving while
// the helper copies. The helper usually answers within 500 ms
// (platform/ps5/helper); the client's deadline bounds a silent one.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/capture_service.h"

#include <cstdio>
#include <ctime>
#include <utility>

namespace splat {
namespace {

// A list request that failed (mailbox not writable, ...) is retried no faster
// than this; a tick runs every frame and must not rewrite req.json each one.
constexpr double kListRetrySeconds = 2.0;

double steady_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// CaptureInfo::time is "YYYYMMDD_HHMMSS" (the helper's name stamp); a
// CaptureView carries epoch seconds. An unparseable stamp is 0, never "now".
std::int64_t capture_seconds(const std::string& stamp) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(stamp.c_str(), "%4d%2d%2d_%2d%2d%2d", &year, &month, &day, &hour, &minute,
                    &second) != 6) {
        return 0;
    }
    std::tm value{};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    value.tm_sec = second;
    value.tm_isdst = -1;
    const std::time_t seconds = std::mktime(&value);
    return seconds == static_cast<std::time_t>(-1) ? 0 : static_cast<std::int64_t>(seconds);
}

CaptureView to_view(const CaptureInfo& capture) {
    CaptureView view;
    view.path = capture.path;
    view.name = capture.name;
    view.time = capture_seconds(capture.time);
    view.bytes = capture.bytes;
    view.supported = capture.supported;
    return view;
}

}  // namespace

CaptureService::CaptureService() : CaptureService(Options{}) {}

CaptureService::CaptureService(Options options)
    : options_(std::move(options)), client_(options_.helper_dir, options_.timing) {}

bool CaptureService::available() const { return client_.alive(); }

double CaptureService::seconds_now() const { return steady_seconds(); }

void CaptureService::adopt(const std::vector<CaptureInfo>& captures) {
    captures_.clear();
    captures_.reserve(captures.size());
    for (const CaptureInfo& capture : captures) captures_.push_back(to_view(capture));
}

void CaptureService::request_list() {
    last_list_ = seconds_now();
    if (!client_.request_list().ok()) {
        pending_ = Pending::None;
        state_ = client_.error();
        return;
    }
    pending_ = Pending::List;
    state_ = "listing";
}

bool CaptureService::try_start() {
    const double now = seconds_now();
    if (start_attempted_ && now - last_start_ < options_.start_interval_seconds) return false;
    start_attempted_ = true;
    last_start_ = now;
    starting_at_ = now;
    if (options_.launcher) {
        options_.launcher(options_.helper_elf);
    } else {
        start_helper(options_.helper_elf);
    }
    state_ = kStarting;
    return true;
}

void CaptureService::send_copy() {
    if (!copy_active_ || copy_sent_) return;
    // A list answer is in flight: the helper removes req.json when it answers,
    // so a copy written now could swallow one of the two answers. The copy goes
    // out on the tick after the list lands (the client's deadline bounds the
    // wait), never in a blocking loop here.
    if (client_.state() == HelperClient::State::Waiting) return;
    if (!client_.request_copy(copy_paths_).ok()) {
        fail_copy(client_.error());
        return;
    }
    copy_sent_ = true;
    copy_paths_.clear();
    pending_ = Pending::Copy;
    state_ = kCopying;
}

void CaptureService::finish_copy(std::size_t copied) {
    copy_active_ = false;
    copy_sent_ = false;
    copy_paths_.clear();
    pending_ = Pending::None;
    state_ = copied == 0 ? std::string("the helper copied nothing")
                         : std::string(kCopiedPrefix) + std::to_string(copied);
}

void CaptureService::fail_copy(const std::string& message) {
    copy_active_ = false;
    copy_sent_ = false;
    copy_paths_.clear();
    pending_ = Pending::None;
    state_ = message.empty() ? std::string("copy failed") : message;
}

void CaptureService::pump() {
    client_.poll();

    bool copy_ended = false;
    switch (client_.state()) {
        case HelperClient::State::Waiting:
            break;
        case HelperClient::State::Done:
            if (pending_ == Pending::List) {
                adopt(client_.captures());
                listed_ = true;
                state_ = kIdle;
            } else if (pending_ == Pending::Copy) {
                finish_copy(client_.copied().size());
                copy_ended = true;
            }
            pending_ = Pending::None;
            break;
        case HelperClient::State::Failed:
            if (pending_ == Pending::Copy) {
                fail_copy(client_.error().empty() ? std::string("copy failed")
                                                  : client_.error());
                copy_ended = true;
            } else if (!copy_active_) {
                state_ = client_.error().empty() ? std::string("failed") : client_.error();
            }
            // A failed list with a copy waiting behind it keeps "Copying...":
            // the copy still goes out on the next tick.
            pending_ = Pending::None;
            break;
        case HelperClient::State::NoHelper:
            listed_ = false;
            if (pending_ == Pending::Copy || copy_active_) {
                fail_copy(kNeedsLoader);
                copy_ended = true;
            }
            pending_ = Pending::None;
            break;
        case HelperClient::State::Idle:
            break;
    }
    // A copy's outcome is the frame's status line: the Photos screen reads
    // state() right after tick(), so no list/start chatter may overwrite it in
    // the same pump. The restart or re-list happens on the next frame.
    if (copy_ended) return;

    if (!client_.alive()) {
        if (copy_active_) {
            fail_copy(kNeedsLoader);
            return;  // report first, restart (if any) on the next frame
        }
        // "Starting" is a short note, not a state the panel sits in: after the
        // grace window the tab row is clean again and the Captures tab says
        // what is missing.
        if (state_ == kStarting && seconds_now() - starting_at_ > options_.starting_seconds) {
            state_ = kIdle;
        }
        try_start();
        return;
    }

    // A copy owns the mailbox until it is answered: no list request may race it.
    if (copy_active_) {
        send_copy();
        return;
    }

    const double now = seconds_now();
    const bool listed_interval = listed_ && now - last_list_ >= options_.list_interval_seconds;
    // A request that failed must not spin the mailbox: retry every kRetrySeconds
    // at most (a refresh() asks right away through force_list_).
    const bool retry = !listed_ && now - last_list_ >= kListRetrySeconds;
    if (force_list_ || listed_interval || retry) {
        force_list_ = false;
        request_list();
    }
}

void CaptureService::tick(double dt) {
    poll_elapsed_ += dt;
    if (poll_elapsed_ < options_.poll_interval_seconds) return;
    poll_elapsed_ = 0.0;
    pump();
}

Status CaptureService::refresh() {
    // The screen asked for fresh rows: re-list even when the helper already
    // answered once, and try the loader when it is absent.
    listed_ = false;
    force_list_ = true;
    poll_elapsed_ = 0.0;
    pump();
    if (client_.alive()) return Status::success();
    return Status::error(kNeedsLoader);
}

Status CaptureService::copy_to_inbox(const std::vector<Path>& paths) {
    if (paths.empty()) return Status::error("no captures selected");
    if (copy_active_) return Status::error("a copy is already running");
    if (!client_.alive()) {
        pending_ = Pending::None;
        state_ = kIdle;
        return Status::error(kNeedsLoader);
    }

    copy_paths_ = paths;
    copy_active_ = true;
    copy_sent_ = false;
    state_ = kCopying;
    // One write of req.json, or none when a list answer is still in flight; in
    // both cases the call returns now and tick() carries the copy to its end.
    send_copy();
    if (!copy_active_) return Status::error(state_);
    return Status::success();
}

}  // namespace splat
