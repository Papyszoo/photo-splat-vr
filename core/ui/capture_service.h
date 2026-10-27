// W23. The real ICaptureService: the Photos screen's Captures tab over the
// splat-helper file mailbox (core/capture/helper_client.h).
//
// The helper ships inside the title package (helper/splat-helper.elf); the
// title pushes it to the payload loader on 127.0.0.1:9021 itself, so the user
// installs nothing extra. When no loader answers the service stays in "Starting"
// for a few seconds and the Photos screen says "needs the payload loader".
//
// UiApp calls tick() once per UI frame (core/ui/app.cpp), so the service polls
// the mailbox on its own: no screen has to drive it. The poll runs at
// poll_interval_seconds (4 Hz), not once per frame, so a frame costs one add.
// refresh() is the screen's explicit "re-list now" entry point (the W7
// contract).
//
// copy_to_inbox() never waits for the helper: it writes req.json and returns,
// state() is kCopying until the answer lands, then "copied N" (copy_finished())
// or the helper's error. The Photos screen watches state() from its update() and
// re-lists the tabs when the copy lands, so neither the UI thread nor the VR
// frame loop is held for the length of a copy.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "capture/helper_client.h"
#include "common/path.h"
#include "common/status.h"
#include "ui/services.h"

namespace splat {

class CaptureService : public ICaptureService {
public:
    struct Options {
        // The mailbox directory: req.json / resp.json / alive. The title passes
        // <its identity's dataDir>/helper (ui_setup.cpp); the payload built as
        // the same identity uses the same folder, so two installs never share
        // a mailbox. No default: a console path belongs to the built identity.
        Path helper_dir;
        // The package root is /app0; the viewer Makefile puts the payload here.
        Path helper_elf = Path("/app0/helper/splat-helper.elf");
        // While the helper is absent, at most one start attempt per minute.
        double start_interval_seconds = 60.0;
        // How long the status line keeps saying "Starting" after an attempt.
        double starting_seconds = 5.0;
        // A running helper is re-listed this often, so a capture taken while
        // the panel is open appears without leaving the screen.
        double list_interval_seconds = 10.0;
        // How often tick() reads the mailbox. One stat/read of resp.json at
        // 4 Hz; the frame loop only adds its dt in between.
        double poll_interval_seconds = 0.25;
        HelperClient::Timing timing;
        // Test seam: push the ELF and say whether a loader took it. Empty means
        // start_helper() (a real socket on the console, false on the host).
        std::function<bool(const Path&)> launcher;
    };

    CaptureService();
    explicit CaptureService(Options options);

    bool available() const override;
    Status refresh() override;
    std::vector<CaptureView> list() const override { return captures_; }
    // Starts the copy and returns at once (the class note above): state() is
    // kCopying until the helper answers, then "copied N" (copy_finished()) or
    // the error line. Nothing on this path sleeps or polls.
    Status copy_to_inbox(const std::vector<Path>& paths) override;
    std::string state() const override { return state_; }
    void tick(double dt) override;

    // The state tokens, shared with the screen and the tests.
    static constexpr const char* kIdle = "idle";
    static constexpr const char* kStarting = "Starting";
    static constexpr const char* kNeedsLoader = "needs the payload loader";
    // A copy in flight: the Photos screen draws this line as-is.
    static constexpr const char* kCopying = "Copying...";
    // A landed copy's state() is kCopiedPrefix + the number of files.
    static constexpr const char* kCopiedPrefix = "copied ";

    // True when state() is a landed copy ("copied 3"), never for the error line
    // that a copy of nothing or a failed helper leaves behind.
    static bool copy_finished(const std::string& state) {
        return state.rfind(kCopiedPrefix, 0) == 0;
    }

private:
    enum class Pending { None, List, Copy };

    void pump();
    bool try_start();
    void request_list();
    void send_copy();
    void finish_copy(std::size_t copied);
    void fail_copy(const std::string& message);
    void adopt(const std::vector<CaptureInfo>& captures);
    double seconds_now() const;

    Options options_;
    HelperClient client_;
    std::vector<CaptureView> captures_;
    std::vector<Path> copy_paths_;
    std::string state_ = kIdle;
    Pending pending_ = Pending::None;
    bool start_attempted_ = false;
    bool listed_ = false;
    bool force_list_ = false;
    // A copy the frame loop is waiting on: accepted, maybe not sent yet (a list
    // answer was in flight), and resolved by an answer, an error or the
    // client's timeout. The screen only reads state().
    bool copy_active_ = false;
    bool copy_sent_ = false;
    double poll_elapsed_ = 0.0;
    double last_start_ = 0.0;
    double starting_at_ = 0.0;
    double last_list_ = 0.0;
};

}  // namespace splat
