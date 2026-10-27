// W23. Host tests for CaptureService (the real ICaptureService over the helper
// mailbox, core/ui/capture_service.*) and for the two Photos-screen changes it
// needs: the Captures tab stays visible and says "Captures need the payload loader"
// when no loader answered, and every refresh logs the sources the title sees.
// The copy is asynchronous (W23 fix): copy_to_inbox() only writes the request,
// tick() reads the answer at 4 Hz, and the Photos screen finishes the copy from
// its own update() — none of the three may hold the frame loop.
//
// The fake helper is the real protocol code (splat::handle_request), the same
// code the payload runs, exactly like tests/helper_client_test.cpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "capture/helper_client.h"
#include "capture/helper_protocol.h"
#include "test_util.h"
#include "ui/capture_service.h"
#include "ui/screens/photos.h"
#include "ui/screen_stack.h"

namespace {

namespace fs = std::filesystem;
using splat::CaptureService;
using splat::Path;

void write_file(const Path& path, const std::string& data) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string read_file(const Path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

long long unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// The console layout (photo/<service>/<title>/<hash>/<capture>) with one JPG and
// one HDR .jxr, newest first as the helper lists them.
struct Fixture {
    splat_test::TempDir dir;
    Path helper_dir;
    Path root;
    Path inbox;

    explicit Fixture(const char* tag)
        : dir(tag),
          helper_dir(dir.path() / "helper"),
          root(dir.path() / "av"),
          inbox(dir.path() / "inbox") {
        fs::create_directories(helper_dir);
        write_file(photo() / "20261024_101112_aabbccdd.jpg", std::string(128, 'j'));
        write_file(photo() / "20261025_052805_00338445.jxr", std::string(64, 'x'));
        write_file(helper_dir / splat::kHelperRootName, root.string() + "\n");
    }

    Path photo() const { return root / "photo" / "NPXS40087" / "CUSA12392" / "2c4"; }
    Path jpg() const { return photo() / "20261024_101112_aabbccdd.jpg"; }
    Path jxr() const { return photo() / "20261025_052805_00338445.jxr"; }

    void beat(long long offset_seconds = 0) {
        write_file(helper_dir / splat::kHelperAliveName, std::to_string(unix_now() + offset_seconds));
    }

    bool answer() {
        const Path request = helper_dir / splat::kHelperRequestName;
        std::error_code error;
        if (!fs::exists(request, error)) return false;
        const std::string response =
            splat::handle_request(read_file(request), splat::helper_root(helper_dir), inbox);
        write_file(helper_dir / "resp.tmp", response);
        fs::rename(helper_dir / "resp.tmp", helper_dir / splat::kHelperResponseName, error);
        fs::remove(request, error);
        return true;
    }
};

CaptureService::Options base_options(const Fixture& fixture) {
    CaptureService::Options options;
    options.helper_dir = fixture.helper_dir;
    options.helper_elf = Path("/app0/helper/splat-helper.elf");
    options.start_interval_seconds = 60.0;
    options.starting_seconds = 5.0;
    options.list_interval_seconds = 10.0;
    return options;
}

// A capture service with no helper at all: the Photos screen only needs
// available() false and an empty list.
class OfflineCaptures : public splat::ICaptureService {
public:
    bool available() const override { return false; }
    splat::Status refresh() override { return splat::Status::error(CaptureService::kNeedsLoader); }
    std::vector<splat::CaptureView> list() const override { return {}; }
    splat::Status copy_to_inbox(const std::vector<Path>&) override {
        return splat::Status::error(CaptureService::kNeedsLoader);
    }
    std::string state() const override { return CaptureService::kStarting; }
};

}  // namespace

TEST_CASE("ui_captures pushes the packaged helper at most once a minute") {
    Fixture fixture("ui-captures-offline");
    int attempts = 0;
    Path pushed;
    CaptureService::Options options = base_options(fixture);
    options.launcher = [&attempts, &pushed](const Path& elf) {
        ++attempts;
        pushed = elf;
        return false;  // no loader on the host, exactly like the console without elfldr
    };
    CaptureService service(options);

    CHECK_FALSE(service.available());
    // The start attempt rides on the 4 Hz poll, not on every frame.
    service.tick(1.0 / 60.0);
    CHECK(attempts == 0);
    service.tick(0.25);
    CHECK(attempts == 1);
    CHECK(pushed == Path("/app0/helper/splat-helper.elf"));
    CHECK(service.state() == std::string(CaptureService::kStarting));
    CHECK(service.list().empty());

    // The screen's refresh re-lists, reports the missing loader, and does not
    // push a second time inside the one-minute window.
    const splat::Status refreshed = service.refresh();
    CHECK_FALSE(refreshed.ok());
    CHECK(refreshed.message() == std::string(CaptureService::kNeedsLoader));
    CHECK(attempts == 1);
    CHECK_FALSE(service.copy_to_inbox({fixture.jpg()}).ok());
    CHECK(attempts == 1);
}

TEST_CASE("ui_captures lists the helper's captures as CaptureViews") {
    Fixture fixture("ui-captures-list");
    CaptureService service(base_options(fixture));
    fixture.beat();
    REQUIRE(service.available());

    REQUIRE(service.refresh().ok());
    CHECK(service.state() == "listing");

    // No answer yet: still listing, the rows stay empty.
    service.tick(1.0 / 60.0);
    CHECK(service.list().empty());

    // The mailbox is not read every frame: one 60 Hz frame is below the 4 Hz
    // poll interval, so an answer that is already on disk waits for the poll.
    REQUIRE(fixture.answer());
    service.tick(1.0 / 60.0);
    CHECK(service.state() == "listing");
    service.tick(0.25);
    CHECK(service.state() == std::string(CaptureService::kIdle));
    REQUIRE(service.list().size() == 2);

    // Newest first, exactly the helper's order; the .jxr is listed but cannot
    // be selected.
    CHECK(service.list()[0].name == "20261025_052805_00338445.jxr");
    CHECK_FALSE(service.list()[0].supported);
    CHECK(service.list()[1].name == "20261024_101112_aabbccdd.jpg");
    CHECK(service.list()[1].supported);
    CHECK(service.list()[1].bytes == 128);
    CHECK(service.list()[1].path == fixture.jpg());

    // "YYYYMMDD_HHMMSS" becomes epoch seconds (local time, like mktime).
    std::tm stamp{};
    stamp.tm_year = 2026 - 1900;
    stamp.tm_mon = 10 - 1;
    stamp.tm_mday = 24;
    stamp.tm_hour = 10;
    stamp.tm_min = 11;
    stamp.tm_sec = 12;
    stamp.tm_isdst = -1;
    CHECK(service.list()[1].time == static_cast<std::int64_t>(std::mktime(&stamp)));

    // A helper that stops beating is no longer available.
    fixture.beat(-30);
    CHECK_FALSE(service.available());
}

TEST_CASE("ui_captures copies a capture without waiting for the helper") {
    Fixture fixture("ui-captures-copy");
    CaptureService::Options options = base_options(fixture);
    options.timing.timeout_seconds = 5.0;
    CaptureService service(options);
    fixture.beat();

    // No helper thread answers: copy_to_inbox() must return on its own.
    const auto start = std::chrono::steady_clock::now();
    const splat::Status started = service.copy_to_inbox({fixture.jpg()});
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(started.ok());
    CHECK(elapsed < 0.10);
    CHECK(service.state() == std::string(CaptureService::kCopying));
    // The request is on disk; the answer is the frame loop's job.
    CHECK(fs::exists(fixture.helper_dir / splat::kHelperRequestName));
    // A second request while the helper is silent is refused, not queued twice.
    CHECK_FALSE(service.copy_to_inbox({fixture.jpg()}).ok());
    CHECK(service.state() == std::string(CaptureService::kCopying));

    // Half a second of frames with no answer: still copying, never held.
    for (int frame = 0; frame < 30; ++frame) service.tick(1.0 / 60.0);
    CHECK(service.state() == std::string(CaptureService::kCopying));
    CHECK_FALSE(CaptureService::copy_finished(service.state()));

    // The helper answers: the next poll lands the copy.
    REQUIRE(fixture.answer());
    service.tick(0.25);
    CHECK(service.state() == "copied 1");
    CHECK(CaptureService::copy_finished(service.state()));
    const Path destination = fixture.inbox / "capture-20261024_101112-aabbccdd.jpg";
    CHECK(fs::exists(destination));
    CHECK(fs::file_size(destination) == 128);

    // The helper's refusal (HDR) is the error line, never a "copied" one.
    REQUIRE(service.copy_to_inbox({fixture.jxr()}).ok());
    REQUIRE(fixture.answer());
    service.tick(0.25);
    CHECK_FALSE(CaptureService::copy_finished(service.state()));
    CHECK(service.state().find("HDR") != std::string::npos);
}

TEST_CASE("ui_captures a silent helper ends the copy with a timeout") {
    Fixture fixture("ui-captures-timeout");
    CaptureService::Options options = base_options(fixture);
    // A zero deadline: the first poll after the request is already too late,
    // so the case does not sleep.
    options.timing.timeout_seconds = 0.0;
    CaptureService service(options);
    fixture.beat();

    REQUIRE(service.copy_to_inbox({fixture.jpg()}).ok());
    CHECK(service.state() == std::string(CaptureService::kCopying));
    // Nothing answers: the client's own deadline ends the copy on a poll.
    service.tick(0.25);
    CHECK_FALSE(CaptureService::copy_finished(service.state()));
    CHECK(service.state().find("timeout") != std::string::npos);
    // The service is free for the next attempt: no stuck "in flight" latch.
    CHECK(service.copy_to_inbox({fixture.jpg()}).ok());
    CHECK(service.state() == std::string(CaptureService::kCopying));
}

TEST_CASE("ui_captures the Photos screen copies in the background and finishes") {
    Fixture fixture("ui-captures-photos-copy");
    CaptureService::Options options = base_options(fixture);
    options.timing.timeout_seconds = 5.0;
    CaptureService service(options);
    fixture.beat();
    REQUIRE(service.available());

    splat::UiServices services;
    services.paths.inbox = fixture.inbox;
    services.paths.mnt = fixture.dir.path() / "mnt";
    services.paths.scenes = fixture.dir.path() / "scenes";
    services.paths.state = fixture.dir.path() / "state";
    services.captures = &service;
    std::vector<std::string> toasts;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    context.content = splat::Rect{16, 78, 1248, 586};
    context.toast_sink = [&toasts](std::string text, double) {
        toasts.push_back(std::move(text));
    };
    stack.set_context(&context);
    std::error_code error;
    fs::create_directories(services.paths.mnt, error);

    splat::PhotosScreen screen;
    screen.on_enter(context, splat::ScreenArgs{});
    // The service asks for its first list on the next poll; answer that one.
    service.tick(0.25);
    REQUIRE(fixture.answer());
    service.tick(0.25);
    REQUIRE(service.list().size() == 2);

    // L1 wraps to the captures (always the last tab); it does not ask the
    // service for a new list.
    splat::UiInput next;
    next.pressed = splat::PagePrev;
    screen.update(context, next);
    REQUIRE(screen.tab() == 3);
    REQUIRE(screen.tabs()[3].captures);
    REQUIRE(screen.tabs()[3].count == 2);

    // Select the supported capture (the HDR tile is first, newest first).
    splat::UiInput right;
    right.pressed = splat::Right;
    screen.update(context, right);
    splat::UiInput select;
    select.pressed = splat::Select;
    screen.update(context, select);
    REQUIRE(screen.selection().size() == 1);

    // A list answer is in flight (refresh() asked for one): the copy has to
    // wait for it, and Square still returns at once.
    REQUIRE(service.refresh().ok());
    splat::UiInput square;
    square.pressed = splat::Secondary;
    const auto start = std::chrono::steady_clock::now();
    screen.update(context, square);
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(elapsed < 0.10);
    CHECK(toasts.empty());
    CHECK(service.state() == std::string(CaptureService::kCopying));
    CHECK(screen.selection().empty());
    CHECK(screen.tab() == 3);

    // The helper answers the list; the same tick sends the held copy.
    REQUIRE(fixture.answer());
    service.tick(0.25);
    CHECK(service.state() == std::string(CaptureService::kCopying));
    REQUIRE(fixture.answer());
    service.tick(0.25);
    CHECK(service.state() == "copied 1");

    // The screen's next idle frame reports it and shows the inbox, which now
    // holds the copied photo.
    screen.update(context, splat::UiInput{});
    REQUIRE(!toasts.empty());
    CHECK(toasts.back() == "Copied 1 capture(s) to the inbox");
    CHECK(screen.tab() == 0);
    CHECK(screen.tabs()[0].id == "inbox");
    CHECK(screen.tabs()[0].count == 1);
    CHECK(fs::exists(fixture.inbox / "capture-20261024_101112-aabbccdd.jpg"));
}

TEST_CASE("ui_captures the Photos screen toasts a failed copy") {
    Fixture fixture("ui-captures-photos-fail");
    CaptureService::Options options = base_options(fixture);
    // A zero deadline: the poll after Square is already the timeout.
    options.timing.timeout_seconds = 0.0;
    CaptureService service(options);
    fixture.beat();
    REQUIRE(service.available());

    splat::UiServices services;
    services.paths.inbox = fixture.inbox;
    services.paths.mnt = fixture.dir.path() / "mnt";
    services.paths.scenes = fixture.dir.path() / "scenes";
    services.paths.state = fixture.dir.path() / "state";
    services.captures = &service;
    std::vector<std::string> toasts;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    context.content = splat::Rect{16, 78, 1248, 586};
    context.toast_sink = [&toasts](std::string text, double) {
        toasts.push_back(std::move(text));
    };
    stack.set_context(&context);
    std::error_code error;
    fs::create_directories(services.paths.mnt, error);

    splat::PhotosScreen screen;
    screen.on_enter(context, splat::ScreenArgs{});
    // The list answers, so the screen sees the two captures.
    service.tick(0.25);
    REQUIRE(fixture.answer());
    service.tick(0.25);

    splat::UiInput next;
    next.pressed = splat::PagePrev;  // the captures: the last tab
    screen.update(context, next);
    splat::UiInput right;
    right.pressed = splat::Right;
    screen.update(context, right);
    splat::UiInput select;
    select.pressed = splat::Select;
    screen.update(context, select);
    REQUIRE(screen.selection().size() == 1);

    splat::UiInput square;
    square.pressed = splat::Secondary;
    screen.update(context, square);
    CHECK(service.state() == std::string(CaptureService::kCopying));

    // The helper never answers: the timeout becomes the screen's toast and the
    // copy is no longer pending.
    service.tick(0.25);
    screen.update(context, splat::UiInput{});
    REQUIRE(!toasts.empty());
    CHECK(toasts.back().find("timeout") != std::string::npos);
    CHECK(toasts.back().find("Copied") == std::string::npos);
}

TEST_CASE("ui_captures the Photos screen shows the loader note and logs the sources") {
    splat_test::TempDir dir("ui-captures-photos");
    const Path inbox = dir.path() / "inbox";
    const Path mnt = dir.path() / "mnt";
    fs::create_directories(inbox);
    fs::create_directories(mnt / "usb0");

    OfflineCaptures offline;
    splat::UiServices services;
    services.paths.inbox = inbox;
    services.paths.mnt = mnt;
    services.paths.scenes = dir.path() / "scenes";
    services.paths.state = dir.path() / "state";
    services.captures = &offline;
    std::vector<std::string> log;
    services.log = [&log](const std::string& text) { log.push_back(text); };

    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    context.content = splat::Rect{16, 78, 1248, 586};
    stack.set_context(&context);

    splat::PhotosScreen screen;
    screen.on_enter(context, splat::ScreenArgs{});

    // Inbox, Console, USB (usb0) and Captures: the capture service is present
    // even though no helper answers, so the tab is there from the first frame.
    REQUIRE(screen.tabs().size() == 4);
    CHECK(screen.tabs()[0].id == "inbox");
    CHECK(screen.tabs()[2].id == "usb0");
    CHECK(screen.tabs()[3].id == "captures");
    CHECK(screen.tabs()[3].captures);
    REQUIRE(log.size() == 1);
    CHECK(log[0] == "ui: sources inbox usb0");

    // The next refreshes (L1/R1) log again; L1 wraps to the Captures tab,
    // which says what is missing instead of looking empty.
    splat::UiInput input;
    input.pressed = splat::PageNext;
    screen.update(context, input);
    input.pressed = splat::PagePrev;
    screen.update(context, input);
    screen.update(context, input);
    CHECK(log.size() == 4);
    CHECK(screen.tab() == 3);
    CHECK(screen.message() == "Captures need the payload loader");
}
