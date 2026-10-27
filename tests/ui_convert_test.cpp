// ConvertDialog, the model choice after the photo picker: the ready models
// only (the remembered one first), the Models screen when none is ready, the
// enqueue arguments (the folder the photos were picked for), the return to that
// folder, and the null service messages.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/path.h"
#include "common/status.h"
#include "gallery/gallery.h"
#include "test_util.h"
#include "ui/fakes.h"
#include "ui/screens/convert_dialog.h"
#include "ui/screen_stack.h"

using splat::Path;

namespace {

class StubScreen : public splat::Screen {
public:
    explicit StubScreen(std::string title) : title_(std::move(title)) {}
    void update(splat::UiContext&, const splat::UiInput&) override {}
    void draw(splat::UiContext&, splat::UiCanvas&, const splat::Rect&) override {}
    std::string title() const override { return title_; }

private:
    std::string title_;
};

// One model, never ready: the dialog must offer the Models screen instead of a
// conversion.
class NoReadyModelService : public splat::IModelService {
public:
    std::vector<splat::ModelView> list() const override {
        splat::ModelView model;
        model.id = "sharp";
        model.name = "SHARP";
        model.state = "license_required";
        return {model};
    }
    std::string licence_text(const std::string&) override { return {}; }
    splat::Status accept(const std::string&) override { return splat::Status::error("nope"); }
    splat::Status download(const std::string&) override { return splat::Status::error("nope"); }
    splat::Status cancel(const std::string&) override { return splat::Status::error("nope"); }
    splat::Status remove(const std::string&) override { return splat::Status::error("nope"); }
    splat::Status select(const std::string&) override { return splat::Status::error("nope"); }
    std::string selected() const override { return {}; }
};

splat::UiInput press(uint32_t action) {
    splat::UiInput input;
    input.pressed = action;
    return input;
}

struct Fixture {
    splat_test::TempDir dir{"ui-convert"};
    Path scenes = dir.path() / "scenes";
    splat::FakeJobService jobs;
    splat::FakeModelService models;
    splat::IModelService* model_service = &models;
    bool with_jobs = true;
    splat::UiServices services;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    splat::ConvertDialog* dialog = nullptr;
    splat::UiCanvas canvas{1280, 720};
    std::vector<std::string> toasts;

    Fixture() {
        registry.add(splat::ScreenId::Scenes, [] { return std::make_unique<StubScreen>("trip"); });
        registry.add(splat::ScreenId::Photos,
                     [] { return std::make_unique<StubScreen>("Add photos"); });
        registry.add(splat::ScreenId::ConvertDialog, [this] {
            auto screen = std::make_unique<splat::ConvertDialog>();
            dialog = screen.get();
            return screen;
        });
        registry.add(splat::ScreenId::Models,
                     [] { return std::make_unique<StubScreen>("Models"); });
        context.content = splat::Rect{64, 112, 1152, 532};
        context.toast_sink = [this](std::string text, double) {
            toasts.push_back(std::move(text));
        };
        stack.set_context(&context);
    }

    // The way the panel gets here: the folder, its photo picker, the dialog.
    void enter(const std::vector<Path>& photos, const std::string& album = "trip") {
        services.paths.scenes = scenes;
        services.paths.inbox = dir.path() / "inbox";
        services.paths.state = dir.path() / "state";
        services.jobs = with_jobs ? &jobs : nullptr;
        services.models = model_service;
        splat::ScreenArgs args;
        args.a = album;
        for (const Path& photo : photos) args.list.push_back(photo.string());
        REQUIRE(stack.push(splat::ScreenId::Scenes, args));
        REQUIRE(stack.push(splat::ScreenId::Photos, args));
        REQUIRE(stack.push(splat::ScreenId::ConvertDialog, args));
    }

    void update(const splat::UiInput& input) { stack.update(context, input); }
    void idle() { update(splat::UiInput{}); }
};

}  // namespace

TEST_CASE("ui_convert lists the ready models, the remembered one first") {
    Fixture fixture;
    fixture.enter({Path("a.jpg")});
    REQUIRE(fixture.dialog != nullptr);
    REQUIRE(fixture.dialog->list().rows().size() == 1);
    CHECK(fixture.dialog->list().rows()[0].label == "DA2-Small");
    CHECK(fixture.dialog->model_id() == "da2");
    CHECK(fixture.dialog->photos().size() == 1);
    CHECK(fixture.dialog->album() == "trip");
    CHECK(fixture.dialog->message().empty());
    CHECK(fixture.dialog->title() == "Choose a model");

    // A second ready model appears while the screen is open: the focused one
    // survives the refresh.
    fixture.models.accept("sharp");
    fixture.models.download("sharp");
    fixture.models.tick(10.0);
    fixture.idle();
    REQUIRE(fixture.dialog->models().size() == 2);
    CHECK(fixture.dialog->model_id() == "da2");

    // Up moves to the other model.
    fixture.update(press(splat::Up));
    CHECK(fixture.dialog->model_id() == "sharp");

    // The model remembered from the last conversion is the first row.
    Fixture again;
    again.models.accept("sharp");
    again.models.download("sharp");
    again.models.tick(10.0);
    REQUIRE(again.models.select("sharp").ok());
    again.enter({Path("a.jpg")});
    CHECK(again.dialog->list().rows()[0].label == "SHARP");
    CHECK(again.dialog->model_id() == "sharp");
}

TEST_CASE("ui_convert offers exactly the backends the job chain registers") {
    Fixture fixture;
    // Both pinned models ready: DA2-Small already is, SHARP after its download.
    REQUIRE(fixture.models.accept("sharp").ok());
    REQUIRE(fixture.models.download("sharp").ok());
    fixture.models.tick(10.0);

    fixture.enter({Path("a.jpg")});
    REQUIRE(fixture.dialog != nullptr);

    std::vector<std::string> offered;
    for (const splat::ModelView& model : fixture.dialog->models()) offered.push_back(model.id);
    std::sort(offered.begin(), offered.end());
    // The console factory registers exactly these two (ps5_jobs.cpp: sharp and
    // da2); no other model row may reach the list.
    CHECK(offered == std::vector<std::string>{"da2", "sharp"});
    REQUIRE(fixture.dialog->list().rows().size() == 2);

    // Each row queues its own backend, not a default: with no remembered
    // choice the list keeps the model service order (SHARP first).
    REQUIRE(fixture.dialog->model_id() == "sharp");
    fixture.update(press(splat::Select));
    const std::vector<splat::JobView> queued = fixture.jobs.snapshot();
    REQUIRE(queued.size() == 1);
    CHECK(queued[0].backend == "sharp");
}

TEST_CASE("ui_convert queues into the folder and returns to it") {
    Fixture fixture;
    fixture.enter({Path("a.jpg"), Path("b.jpg")}, "trip");
    REQUIRE(fixture.stack.size() == 3);

    fixture.update(press(splat::Select));

    const std::vector<splat::JobView> queued = fixture.jobs.snapshot();
    REQUIRE(queued.size() == 2);
    CHECK(queued[0].name == "a.jpg");
    CHECK(queued[0].photo == Path("a.jpg"));
    CHECK(queued[0].backend == "da2");
    CHECK(queued[0].album == "trip");
    CHECK(queued[1].name == "b.jpg");
    // The picker and the dialog are gone: the folder shows the new tiles.
    CHECK(fixture.stack.size() == 1);
    CHECK(fixture.stack.top_id() == splat::ScreenId::Scenes);
    // The choice is remembered for the next conversion.
    CHECK(fixture.models.selected() == "da2");
    // No toast: the folder's new tiles and the conversion chip show it.
    CHECK(fixture.toasts.empty());
    // No folder was created: the destination is the folder the photos came from.
    CHECK_FALSE(std::filesystem::exists(fixture.scenes));
}

TEST_CASE("ui_convert queues loose scenes with the empty album") {
    Fixture fixture;
    fixture.enter({Path("a.jpg")}, "");
    fixture.update(press(splat::Select));
    const std::vector<splat::JobView> queued = fixture.jobs.snapshot();
    REQUIRE(queued.size() == 1);
    CHECK(queued[0].album.empty());
    CHECK(fixture.stack.top_id() == splat::ScreenId::Scenes);
}

TEST_CASE("ui_convert points at the models screen when none is ready") {
    NoReadyModelService models;
    Fixture fixture;
    fixture.model_service = &models;
    fixture.enter({Path("a.jpg")});

    REQUIRE(fixture.dialog->list().rows().size() == 1);
    CHECK(fixture.dialog->list().rows()[0].label == "Open Models");
    CHECK(fixture.dialog->model_id().empty());
    CHECK(fixture.dialog->hints()[0].label == "Open Models");

    fixture.update(press(splat::Select));
    CHECK(fixture.stack.top_id() == splat::ScreenId::Models);
    CHECK(fixture.jobs.snapshot().empty());
}

TEST_CASE("ui_convert will not start without photos") {
    Fixture fixture;
    fixture.enter({});
    fixture.update(press(splat::Select));
    CHECK(fixture.jobs.snapshot().empty());
    CHECK(fixture.stack.top_id() == splat::ScreenId::ConvertDialog);
    REQUIRE_FALSE(fixture.toasts.empty());
    CHECK(fixture.toasts.back() == "No photos selected");
}

TEST_CASE("ui_convert says when conversion is not available") {
    Fixture fixture;
    fixture.with_jobs = false;
    fixture.enter({Path("a.jpg")});

    CHECK(fixture.dialog->message() == "Conversion is not available on this build");
    REQUIRE(fixture.dialog->list().rows().size() == 1);
    CHECK(fixture.dialog->list().rows()[0].dim);

    // Cross explains itself and stays on the screen.
    fixture.update(press(splat::Select));
    REQUIRE_FALSE(fixture.toasts.empty());
    CHECK(fixture.toasts.back() == "Conversion is not available on this build");
    CHECK(fixture.stack.top_id() == splat::ScreenId::ConvertDialog);

    // The warning line draws without a job service.
    fixture.dialog->draw(fixture.context, fixture.canvas, fixture.context.content);
}
