// Host test for the abort/teardown handshake of the PS5 HTTP fetcher.
//
// platform/ps5/net/ps5_http_fetcher.cpp is a title (PS5_TITLE) translation unit
// full of sceHttp calls and cannot run on the Mac; the handshake between a
// transfer and abort() lives in platform/ps5/net/live_requests.h and is tested
// here with callbacks standing in for sceHttpAbortRequest and
// sceHttpDeleteRequest. The two properties the console needs:
//  - abort() never waits for an entry whose delete (or an earlier abort) is
//    still inside an SDK call, so Cancel, the stall watchdog and Quit cannot
//    block behind it;
//  - an id is never aborted after its request was deleted, and a reused id is
//    never aborted through the old entry.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "../platform/ps5/net/live_requests.h"

using splat::ps5::AbortAttempt;
using splat::ps5::LiveRegistry;

namespace {

template <typename Predicate>
bool wait_until(Predicate predicate, double seconds = 5.0) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

double seconds_since(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

bool contains(const std::vector<int>& ids, int id) {
    for (const int value : ids) {
        if (value == id) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("live_requests aborts each live id and logs one line per transfer") {
    LiveRegistry registry;
    LiveRegistry::Scope first(registry);
    LiveRegistry::Scope second(registry);
    LiveRegistry::publish(first.request(), 41);
    LiveRegistry::publish(second.request(), 42);

    std::vector<int> aborted;
    const auto abort_call = [&aborted](int id) {
        aborted.push_back(id);
        return 0;
    };

    std::vector<AbortAttempt> attempts = registry.abort_all(1, abort_call);
    REQUIRE(attempts.size() == 2);
    CHECK(attempts[0].id == 41);
    CHECK(attempts[0].repeated == false);
    CHECK(attempts[1].id == 42);
    CHECK(attempts[1].repeated == false);
    CHECK(first.request().reason.load() == 1);

    // The same reason again: the SDK call still happens (it is what might unblock
    // the read), but the caller must not log a second line.
    attempts = registry.abort_all(1, abort_call);
    REQUIRE(attempts.size() == 2);
    CHECK(attempts[0].repeated);
    CHECK(attempts[1].repeated);

    // Even a new reason (Cancel after a stall) does not log a second line: the
    // line belongs to the transfer, not to the reason. The reason the transfer
    // reads - and the row's message - is still updated.
    attempts = registry.abort_all(2, abort_call);
    REQUIRE(attempts.size() == 2);
    CHECK(attempts[0].repeated);
    CHECK(first.request().reason.load() == 2);

    // Retiring one entry clears its id before the delete runs; abort() then only
    // sees the other one.
    int deletes = 0;
    LiveRegistry::retire(first.request(), [&deletes] { ++deletes; });
    CHECK(deletes == 1);
    aborted.clear();
    attempts = registry.abort_all(2, abort_call);
    REQUIRE(attempts.size() == 1);
    CHECK(attempts[0].id == 42);
    CHECK(aborted.size() == 1);
    CHECK(aborted[0] == 42);
}

TEST_CASE("live_requests a request that is not published yet is skipped, then found") {
    LiveRegistry registry;
    LiveRegistry::Scope request(registry);
    std::vector<int> aborted;

    // Between create and publish: nothing to abort, not an error.
    CHECK(registry.abort_all(1, [&aborted](int id) {
                                 aborted.push_back(id);
                                 return 0;
                             }).empty());

    LiveRegistry::publish(request.request(), 77);
    const std::vector<AbortAttempt> attempts = registry.abort_all(1, [&aborted](int id) {
        aborted.push_back(id);
        return 0;
    });
    REQUIRE(attempts.size() == 1);
    CHECK(attempts[0].id == 77);
    CHECK(aborted.size() == 1);
}

TEST_CASE("live_requests never waits for a delete that is stuck") {
    // The verifier's scenario: a transfer thread is inside sceHttpDeleteRequest
    // and that call does not return. abort() (Cancel, the watchdog, Quit) must
    // still return at once and still abort the other live request.
    LiveRegistry registry;
    LiveRegistry::Scope retiring(registry);
    LiveRegistry::Scope other(registry);
    LiveRegistry::publish(retiring.request(), 41);
    LiveRegistry::publish(other.request(), 42);

    std::atomic<bool> delete_inside{false};
    std::atomic<bool> delete_release{false};
    std::thread deleter([&] {
        LiveRegistry::retire(retiring.request(), [&] {
            delete_inside.store(true);
            while (!delete_release.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    });
    REQUIRE(wait_until([&] { return delete_inside.load(); }));

    std::vector<int> aborted;
    const auto started = std::chrono::steady_clock::now();
    const std::vector<AbortAttempt> attempts = registry.abort_all(1, [&aborted](int id) {
        aborted.push_back(id);
        return 0;
    });
    const double seconds = seconds_since(started);
    INFO("abort with a stuck delete returned in " << seconds << " s");
    CHECK_MESSAGE(seconds < 0.5, "abort waited " << seconds << " s for the delete");
    REQUIRE(aborted.size() == 1);
    CHECK(aborted[0] == 42);  // the retiring id was cleared before the delete
    CHECK(attempts.size() == 1);
    // The request being deleted must not have been aborted after its id was
    // cleared: the delete callback is the only holder of that entry.

    delete_release.store(true);
    deleter.join();
}

TEST_CASE("live_requests never waits for an abort that is stuck") {
    // The same failure one layer up: sceHttpAbortRequest itself does not return.
    // A second abort (the watchdog's next tick, or Quit) must not queue behind
    // the first; it skips that entry and still reaches the others.
    LiveRegistry registry;
    LiveRegistry::Scope blocked(registry);
    LiveRegistry::Scope other(registry);
    LiveRegistry::publish(blocked.request(), 51);
    LiveRegistry::publish(other.request(), 52);

    std::atomic<bool> abort_inside{false};
    std::atomic<bool> abort_release{false};
    std::vector<int> first_aborted;
    std::thread aborter([&] {
        registry.abort_all(1, [&](int id) {
            if (id == 51) {
                abort_inside.store(true);
                while (!abort_release.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            first_aborted.push_back(id);
            return 0;
        });
    });
    REQUIRE(wait_until([&] { return abort_inside.load(); }));

    std::vector<int> second_aborted;
    const auto started = std::chrono::steady_clock::now();
    const std::vector<AbortAttempt> attempts = registry.abort_all(2, [&second_aborted](int id) {
        second_aborted.push_back(id);
        return 0;
    });
    const double seconds = seconds_since(started);
    INFO("abort with a stuck abort in flight returned in " << seconds << " s");
    CHECK_MESSAGE(seconds < 0.5, "abort waited " << seconds << " s for the first abort");
    REQUIRE(second_aborted.size() == 1);
    CHECK(second_aborted[0] == 52);
    CHECK(attempts.size() == 1);

    abort_release.store(true);
    aborter.join();
    CHECK(contains(first_aborted, 51));
}

TEST_CASE("live_requests a reused id belongs to its new entry only") {
    // The delete of the old request finished before the new one was created, so
    // the SDK may hand out the same id. The old entry is cleared; only the new
    // entry may be aborted, exactly once.
    LiveRegistry registry;
    LiveRegistry::Scope old_request(registry);
    LiveRegistry::publish(old_request.request(), 7);
    int deletes = 0;
    LiveRegistry::retire(old_request.request(), [&deletes] { ++deletes; });
    CHECK(deletes == 1);
    CHECK(old_request.request().id == -1);

    LiveRegistry::Scope new_request(registry);
    LiveRegistry::publish(new_request.request(), 7);

    std::vector<int> aborted;
    const std::vector<AbortAttempt> attempts = registry.abort_all(1, [&aborted](int id) {
        aborted.push_back(id);
        return -7;
    });
    REQUIRE(attempts.size() == 1);
    CHECK(attempts[0].id == 7);
    CHECK(attempts[0].code == -7);  // the SDK's return value reaches the log
    CHECK(aborted.size() == 1);
    CHECK(new_request.request().reason.load() == 1);
    CHECK(old_request.request().reason.load() == 0);  // the old entry was never aborted
}
