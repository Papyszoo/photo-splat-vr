// The TV's own output during a VR session (0.6.0, "one menu, two views").
//
// The Hmd2 mirror can only show the eye buffers, which carry the VR window, the
// scene label and the HUD, and the TV person has to be able to use the menu.
// So the title opens the MAIN VideoOut port next to the headset's and puts its
// own picture there: the renderer's TV image (render/tv_view.h: the left eye's
// centre 16:9 crop of the scene, plus the same menu canvas flat when the menu is
// open) at 30 fps. This class is the output half of that:
//
//   - start() opens the port, flips one black frame (so the TV is ours from the
//     first second of the session, before the headset is worn or the first frame
//     is rendered) and starts a worker thread. A port that will not open is the
//     caller's cue to keep the Hmd2 mirror (today's behaviour), nothing else.
//   - the render loop never waits for it: finished TV frames are handed over in
//     a few slots (render/tv_slots.h, bounded, newest wins), and the CPU tile
//     write and the flip wait run on the worker.
//   - a port that stops working while the session runs (flips refused, or the
//     system never showing them) is noticed here, once: failed() turns true, the
//     worker stops presenting and the caller falls back to the mirror.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "render/tv_slots.h"
#include "render/tv_view.h"
#include "title_common.h"
#include "videoout.h"

namespace viewer {

class TvOutput {
public:
    // Frame slots shared with the renderer (SplatRenderer::enable_tv_capture).
    static constexpr int kSlots = 3;
    // A port is given up after this many failed presents in a row ...
    static constexpr int kMaxErrors = 30;
    // ... or this many presents in a row whose earlier flip the system never
    // showed (VideoOutDisplay::unconfirmed_flips): it is not scanning us out.
    static constexpr int kMaxUnconfirmed = 3;

    struct Stats {
        uint64_t presents = 0;
        uint64_t errors = 0;
        double tile_ms_sum = 0.0;
        double tile_ms_max = 0.0;
    };

    ~TvOutput() { stop(); }

    // Opens the port and starts the worker. `error` says why when it does not.
    bool start(std::string& error) {
        if (worker_.joinable()) return true;
        splat::ps5::VideoOutConfig config;
        config.width = splat::kTvWidth;
        config.height = splat::kTvHeight;
        config.flip_rate = 1;   // 30 fps on a 60 Hz output, the TV path's pacing
        config.buffers = 2;
        if (!display_.open(config, error)) return false;
        stop_ = false;
        failed_ = false;
        running_ = true;
        worker_ = std::thread([this] { run(); });
        return true;
    }

    // Where the worker reads slot `i`'s pixels (R, G, B, A, 1920x1080); set
    // once the renderer exists and before the first publish().
    void attach(std::function<const uint8_t*(int)> pixels) { pixels_ = std::move(pixels); }

    bool running() const { return running_ && !failed_; }
    bool failed() const { return failed_; }
    // Why it failed, for the one line the caller logs.
    std::string failure() const { return failure_; }

    // Render loop side (render/tv_slots.h).
    int acquire() { return running() ? queue_.acquire() : -1; }
    void publish(int slot) { queue_.publish(slot); }
    void cancel(int slot) { queue_.cancel(slot); }
    uint64_t dropped() const { return queue_.refused() + queue_.replaced(); }

    Stats stats() const {
        Stats stats;
        stats.presents = presents_;
        stats.errors = errors_;
        stats.tile_ms_sum = static_cast<double>(tile_us_sum_) / 1000.0;
        stats.tile_ms_max = static_cast<double>(tile_us_max_) / 1000.0;
        return stats;
    }

    // Joins the worker and gives the port back; safe to call twice and on any path.
    void stop() {
        stop_ = true;
        queue_.close();
        if (worker_.joinable()) worker_.join();
        running_ = false;
        std::string problems;
        display_.close(&problems);
        if (!problems.empty()) line("viewer: vr tv output: closing the port: %s", problems.c_str());
    }

private:
    void run() {
        std::string error;
        double tile_ms = 0.0;
        {
            // The TV is ours from now: black until the first real picture.
            std::vector<uint32_t> black(static_cast<size_t>(splat::kTvWidth) * splat::kTvHeight,
                                        0xff000000u);
            if (!display_.present(black.data(), &tile_ms, error, false)) {
                fail("the first flip was refused: " + error);
                return;
            }
        }
        int errors_in_a_row = 0;
        int unconfirmed_in_a_row = 0;
        uint64_t unconfirmed_seen = display_.unconfirmed_flips();
        while (!stop_) {
            const int slot = queue_.take(std::chrono::milliseconds(100));
            if (slot < 0) continue;
            const uint8_t* pixels = pixels_ ? pixels_(slot) : nullptr;
            bool ok = false;
            error.clear();
            if (pixels != nullptr) {
                ok = display_.present(pixels, &tile_ms, error, true);
            } else {
                error = "no pixels for the slot";
            }
            queue_.release(slot);
            if (ok) {
                errors_in_a_row = 0;
                ++presents_;
                const uint64_t micros = static_cast<uint64_t>(tile_ms * 1000.0);
                tile_us_sum_ += micros;
                if (micros > tile_us_max_) tile_us_max_ = micros;
                const uint64_t unconfirmed = display_.unconfirmed_flips();
                unconfirmed_in_a_row = unconfirmed > unconfirmed_seen ? unconfirmed_in_a_row + 1 : 0;
                unconfirmed_seen = unconfirmed;
                if (unconfirmed_in_a_row >= kMaxUnconfirmed) {
                    fail("the system did not show " + std::to_string(kMaxUnconfirmed) +
                         " flips in a row (it is not scanning the port out)");
                    return;
                }
            } else {
                ++errors_;
                if (++errors_in_a_row >= kMaxErrors) {
                    fail(std::to_string(kMaxErrors) + " presents in a row failed, last: " + error);
                    return;
                }
            }
        }
    }

    // The port is closed before the flag is raised: the caller turns the Hmd2
    // mirror on when it sees failed(), and the TV must not still be ours then.
    void fail(const std::string& why) {
        failure_ = why;
        display_.close(nullptr);
        failed_ = true;
    }

    splat::ps5::VideoOutDisplay display_;
    splat::TvSlotQueue queue_{kSlots};
    std::function<const uint8_t*(int)> pixels_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    std::string failure_;   // written before failed_ is set, read after it is seen
    std::atomic<uint64_t> presents_{0};
    std::atomic<uint64_t> errors_{0};
    std::atomic<uint64_t> tile_us_sum_{0};
    std::atomic<uint64_t> tile_us_max_{0};
};

}  // namespace viewer
