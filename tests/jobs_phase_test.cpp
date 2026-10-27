#include <doctest/doctest.h>

#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "jobs/gpu_phase_guard.h"

using namespace std::chrono_literals;

TEST_CASE("the guard alternates inference and viewing") {
    splat::GpuPhaseGuard guard;
    CHECK(guard.phase() == splat::GpuPhase::Idle);
    CHECK_FALSE(guard.model_loaded());

    REQUIRE(guard.try_begin_inference());
    CHECK(guard.phase() == splat::GpuPhase::Inference);
    CHECK_FALSE(guard.try_begin_inference());
    CHECK_FALSE(guard.try_begin_viewing());

    guard.end_inference();
    CHECK(guard.phase() == splat::GpuPhase::Idle);

    REQUIRE(guard.try_begin_viewing());
    CHECK(guard.phase() == splat::GpuPhase::Viewing);
    CHECK_FALSE(guard.try_begin_inference());

    guard.end_viewing();
    CHECK(guard.phase() == splat::GpuPhase::Idle);
}

TEST_CASE("viewing stays blocked after inference until the model is unloaded") {
    splat::GpuPhaseGuard guard;
    guard.begin_inference();
    guard.set_model_loaded(true);

    guard.end_inference();
    CHECK(guard.phase() == splat::GpuPhase::Idle);
    CHECK(guard.model_loaded());
    CHECK_FALSE(guard.try_begin_viewing());

    guard.set_model_loaded(false);
    CHECK(guard.try_begin_viewing());
    guard.end_viewing();
}

TEST_CASE("a viewer thread waits for inference and for the unload") {
    splat::GpuPhaseGuard guard;
    guard.begin_inference();
    guard.set_model_loaded(true);

    std::promise<void> entered;
    std::future<void> entered_future = entered.get_future();
    std::thread viewer([&guard, &entered] {
        guard.begin_viewing();
        entered.set_value();
        guard.end_viewing();
    });

    CHECK(entered_future.wait_for(50ms) == std::future_status::timeout);

    // Inference ended but the weights are still resident: still no viewing.
    guard.end_inference();
    CHECK(entered_future.wait_for(50ms) == std::future_status::timeout);

    guard.set_model_loaded(false);
    CHECK(entered_future.wait_for(2s) == std::future_status::ready);
    viewer.join();
    CHECK(guard.phase() == splat::GpuPhase::Idle);
}

TEST_CASE("an inference thread waits for the viewer to leave") {
    splat::GpuPhaseGuard guard;
    REQUIRE(guard.try_begin_viewing());

    std::promise<void> entered;
    std::future<void> entered_future = entered.get_future();
    std::thread inference([&guard, &entered] {
        guard.begin_inference();
        entered.set_value();
        guard.end_inference();
    });

    CHECK(entered_future.wait_for(50ms) == std::future_status::timeout);

    guard.end_viewing();
    CHECK(entered_future.wait_for(2s) == std::future_status::ready);
    inference.join();
    CHECK(guard.phase() == splat::GpuPhase::Idle);
}

TEST_CASE("shutdown releases a blocked waiter instead of hanging the process") {
    splat::GpuPhaseGuard guard;
    REQUIRE(guard.try_begin_inference());
    CHECK_FALSE(guard.stopped());

    std::promise<bool> acquired;
    std::future<bool> acquired_future = acquired.get_future();
    std::thread viewer([&guard, &acquired] { acquired.set_value(guard.begin_viewing()); });

    // Inference is running, so the viewer blocks.
    CHECK(acquired_future.wait_for(50ms) == std::future_status::timeout);

    guard.shutdown();
    CHECK(guard.stopped());
    REQUIRE(acquired_future.wait_for(2s) == std::future_status::ready);
    CHECK_FALSE(acquired_future.get());  // told to give up, not admitted
    viewer.join();
    // The viewer did not take the phase; the inference that was already running
    // is still the owner until it ends.
    CHECK(guard.phase() == splat::GpuPhase::Inference);

    // After shutdown nothing blocks again.
    CHECK_FALSE(guard.begin_inference());
    CHECK_FALSE(guard.begin_viewing());
    CHECK_FALSE(guard.try_begin_inference());
    CHECK_FALSE(guard.try_begin_viewing());
    guard.end_inference();
}

// The app's transition log is filled from the observer, so the observer's order
// must be the guard's real order: no begin may be recorded before the matching
// end, however fast the two sides alternate on different cores. The observer
// stalls on the releasing transitions (the app writes and flushes a line there):
// with the waiter woken first, the other side would take the phase and log its
// begin during that stall, which this test would see as an inversion.
TEST_CASE("rapid alternation never logs a begin before the matching end") {
    splat::GpuPhaseGuard guard;
    std::mutex log_mutex;
    std::vector<splat::GpuTransition> log;
    guard.set_observer([&log, &log_mutex](splat::GpuTransition transition, splat::GpuPhase,
                                          bool) {
        // A log line takes a moment to format and flush; the point is that the
        // transition is already recorded as happening before the other side is
        // let in.
        if (transition == splat::GpuTransition::ViewerEnd ||
            transition == splat::GpuTransition::ModelUnloaded) {
            std::this_thread::sleep_for(1ms);
        }
        std::lock_guard<std::mutex> lock(log_mutex);
        log.push_back(transition);
    });

    constexpr int kRounds = 200;
    std::thread viewer([&guard] {
        for (int round = 0; round < kRounds; ++round) {
            if (!guard.begin_viewing()) return;
            guard.end_viewing();
        }
    });
    std::thread inference([&guard] {
        for (int round = 0; round < kRounds; ++round) {
            if (!guard.begin_inference()) return;
            guard.set_model_loaded(true);
            guard.set_model_loaded(false);
            guard.end_inference();
        }
    });
    viewer.join();
    inference.join();

    // 2 viewer events + 4 inference events per round.
    REQUIRE(log.size() == static_cast<size_t>(kRounds) * 6);

    // Replay the log as a state machine. An inverted pair shows up as a begin
    // that arrives while the phase is already taken (or as an end with nothing
    // to end), which is exactly what the app's ordering check rejects.
    int inversions = 0;
    splat::GpuPhase phase = splat::GpuPhase::Idle;
    bool loaded = false;
    for (splat::GpuTransition event : log) {
        switch (event) {
            case splat::GpuTransition::ViewerBegin:
                if (phase != splat::GpuPhase::Idle || loaded) ++inversions;
                phase = splat::GpuPhase::Viewing;
                break;
            case splat::GpuTransition::ViewerEnd:
                if (phase != splat::GpuPhase::Viewing) ++inversions;
                phase = splat::GpuPhase::Idle;
                break;
            case splat::GpuTransition::InferenceBegin:
                if (phase != splat::GpuPhase::Idle || loaded) ++inversions;
                phase = splat::GpuPhase::Inference;
                break;
            case splat::GpuTransition::ModelLoaded:
                if (phase != splat::GpuPhase::Inference || loaded) ++inversions;
                loaded = true;
                break;
            case splat::GpuTransition::ModelUnloaded:
                if (!loaded) ++inversions;
                loaded = false;
                break;
            case splat::GpuTransition::InferenceEnd:
                // The model is unloaded before inference is allowed to end.
                if (phase != splat::GpuPhase::Inference || loaded) ++inversions;
                phase = splat::GpuPhase::Idle;
                break;
            case splat::GpuTransition::Shutdown:
                ++inversions;  // never requested here
                break;
        }
    }
    CHECK(inversions == 0);
    CHECK(phase == splat::GpuPhase::Idle);
    CHECK_FALSE(loaded);
}
