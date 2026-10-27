// The deterministic fake services: job progress on tick, the model state walk
// and the capture list. These drive the tests, tools/ui_preview and the console
// fake mode, so their timing and ordering are part of the contract.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "ui/fakes.h"

namespace {

splat::Path photo(const char* name) { return splat::Path(name); }

}  // namespace

TEST_CASE("ui_fakes job progress is deterministic") {
    splat::FakeJobService jobs;
    REQUIRE(jobs.enqueue({photo("a.jpg"), photo("b.png")}, "sharp", "2026-10-26").ok());

    std::vector<splat::JobView> view = jobs.snapshot();
    REQUIRE(view.size() == 2);
    CHECK(view[0].name == "a.jpg");
    CHECK(view[0].backend == "sharp");
    CHECK(view[0].album == "2026-10-26");
    CHECK(view[0].state == "running");
    CHECK(view[0].fraction == 0.0f);
    CHECK(view[1].state == "queued");
    CHECK(jobs.busy());

    // 1 s of a 4 s job: exactly a quarter, in one step or four.
    jobs.tick(1.0);
    CHECK(jobs.snapshot()[0].fraction == doctest::Approx(0.25));
    jobs.tick(0.25);
    CHECK(jobs.snapshot()[0].fraction == doctest::Approx(0.3125));

    jobs.tick(10.0);
    view = jobs.snapshot();
    CHECK(view[0].state == "done");
    CHECK(view[0].fraction == doctest::Approx(1.0));
    CHECK(view[1].state == "running");
    CHECK(jobs.busy());

    jobs.tick(10.0);
    CHECK(jobs.snapshot()[1].state == "done");
    CHECK_FALSE(jobs.busy());
}

TEST_CASE("ui_fakes job cancel moves to the next job") {
    splat::FakeJobService jobs;
    REQUIRE(jobs.enqueue({photo("a.jpg"), photo("b.jpg")}, "da2", "").ok());
    const std::string first = jobs.snapshot()[0].id;

    CHECK(jobs.cancel(first).ok());
    CHECK(jobs.snapshot()[0].state == "cancelled");
    CHECK(jobs.snapshot()[1].state == "running");
    CHECK_FALSE(jobs.cancel("nope").ok());
    CHECK(jobs.cancel(jobs.snapshot()[1].id).ok());  // a running job can be cancelled
}

TEST_CASE("ui_fakes model walks licence -> download -> ready") {
    splat::FakeModelService models;
    std::vector<splat::ModelView> view = models.list();
    REQUIRE(view.size() == 2);
    CHECK(view[0].id == "sharp");
    CHECK(view[0].gated);
    CHECK(view[0].state == "license_required");
    CHECK(view[1].id == "da2");
    CHECK_FALSE(view[1].gated);
    CHECK(view[1].state == "ready");

    // A gated model refuses to download before the licence is accepted.
    CHECK_FALSE(models.download("sharp").ok());
    CHECK(models.accept("sharp").ok());
    CHECK(models.list()[0].state == "accepted");
    CHECK(models.download("sharp").ok());
    CHECK(models.list()[0].state == "downloading");

    models.tick(splat::FakeModelService::kDownloadSeconds / 2.0);
    CHECK(models.list()[0].fraction == doctest::Approx(0.5));
    models.tick(splat::FakeModelService::kDownloadSeconds / 2.0);
    CHECK(models.list()[0].state == "ready");
    CHECK(models.list()[0].fraction == doctest::Approx(1.0));

    // Only a ready model can be the converter's choice.
    CHECK(models.select("sharp").ok());
    CHECK(models.selected() == "sharp");
    CHECK(models.list()[0].selected);
    CHECK_FALSE(models.list()[1].selected);
}

TEST_CASE("ui_fakes model remove keeps the acceptance") {
    splat::FakeModelService models;
    REQUIRE(models.accept("sharp").ok());
    REQUIRE(models.download("sharp").ok());
    models.tick(splat::FakeModelService::kDownloadSeconds);
    REQUIRE(models.list()[0].state == "ready");
    REQUIRE(models.select("sharp").ok());

    CHECK(models.remove("sharp").ok());
    CHECK(models.list()[0].state == "accepted");
    CHECK(models.selected().empty());
    CHECK_FALSE(models.select("sharp").ok());  // no longer ready
}

TEST_CASE("ui_fakes captures list one unsupported capture") {
    splat::FakeCaptureService captures;
    CHECK(captures.available());
    const std::vector<splat::CaptureView> view = captures.list();
    REQUIRE(view.size() == 4);
    CHECK(view[0].name == "capture_01.jpg");
    CHECK(view[0].supported);
    CHECK_FALSE(view[3].supported);
    CHECK(view[3].name == "capture_04.jxr");
    CHECK(captures.state() == "idle");

    CHECK(captures.copy_to_inbox({view[0].path, view[1].path}).ok());
    CHECK(captures.state() == "copied 2");

    // An HDR capture cannot be copied.
    CHECK_FALSE(captures.copy_to_inbox({view[3].path}).ok());
    CHECK_FALSE(captures.copy_to_inbox({}).ok());
    CHECK(captures.state() == "copied 2");  // unchanged on failure
}
