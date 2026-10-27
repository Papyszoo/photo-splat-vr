// ModelsScreen and LicenceScreen: the licence text wrapping and paging maths,
// the Cancel / Delete card (Cancel focused first), the state-dependent Select (a gated
// model opens the Licence screen, a Ready one becomes the converter, a
// non-Ready one is never selected), and Square cancelling a running download.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <algorithm>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "model/sha256.h"
#include "ui/fakes.h"
#include "ui/screen_stack.h"
#include "ui/screens/licence.h"
#include "ui/screens/models.h"

namespace {

using splat::FakeModelService;
using splat::LicenceScreen;
using splat::ModelsScreen;

splat::UiInput press(uint32_t action) {
    splat::UiInput input;
    input.pressed = action;
    return input;
}

struct Fixture {
    splat::UiServices services;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    std::string toast;

    Fixture() {
        context.content = splat::Rect{16, 78, 1248, 586};
        context.toast_sink = [this](std::string text, double /*seconds*/) {
            toast = std::move(text);
        };
        registry.add(splat::ScreenId::Models,
                     [] { return std::make_unique<ModelsScreen>(); });
        registry.add(splat::ScreenId::Licence,
                     [] { return std::make_unique<LicenceScreen>(); });
        stack.set_context(&context);
    }

    // Pushes the real Models screen and returns it; `services.models` must be
    // set first, because on_enter lists the models.
    ModelsScreen* open_models() {
        stack.push(splat::ScreenId::Models);
        return dynamic_cast<ModelsScreen*>(stack.top());
    }

    LicenceScreen* open_licence(const std::string& id) {
        splat::ScreenArgs args;
        args.a = id;
        stack.push(splat::ScreenId::Licence, args);
        return dynamic_cast<LicenceScreen*>(stack.top());
    }
};

const splat::ModelView* find_view(const std::vector<splat::ModelView>& views,
                                  const std::string& id) {
    for (const splat::ModelView& view : views) {
        if (view.id == id) return &view;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("ui_model_screens wraps licence text at the panel width") {
    constexpr int kSize = 20;
    const auto width = [](const char* text) {
        return static_cast<int>(splat::UiCanvas::text_width(text, kSize));
    };
    // A width that holds "one two" and "four five" but not one word more.
    const int line = std::max(width("one two"), width("four five")) + 1;
    REQUIRE(width("one two three") > line);
    REQUIRE(width("three four") > line);

    const std::string text = "one two three four five\n\nsix";
    const std::vector<std::string> lines = splat::wrap_licence_text(text, line, kSize);
    REQUIRE(lines.size() == 5);
    CHECK(lines[0] == "one two");
    CHECK(lines[1] == "three");
    CHECK(lines[2] == "four five");
    CHECK(lines[3] == "");  // the blank line is kept
    CHECK(lines[4] == "six");

    // A word longer than the line is split into lines that each fit, and
    // nothing is lost.
    const int narrow = width("abcde") + 1;
    const std::vector<std::string> split =
        splat::wrap_licence_text("abcdefghijklmnopqrstuvwxyz", narrow, kSize);
    REQUIRE(split.size() >= 3);
    std::string joined;
    for (const std::string& part : split) {
        CHECK(width(part.c_str()) <= narrow);
        joined += part;
    }
    CHECK(joined == "abcdefghijklmnopqrstuvwxyz");

    // UTF-8 is never cut inside a code point.
    const std::vector<std::string> polish =
        splat::wrap_licence_text("\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87",
                                 width("\xC5\xBC\xC3\xB3") + 1, kSize);
    std::string rejoined;
    for (const std::string& part : polish) {
        REQUIRE_FALSE(part.empty());
        CHECK((static_cast<unsigned char>(part[0]) & 0xC0) != 0x80);
        rejoined += part;
    }
    CHECK(rejoined.size() == 16);

    // Runs of whitespace collapse, CRLF behaves like LF, empty stays one line.
    // A single line break is a space (licence files are hard-wrapped), so the
    // text reflows to the full width.
    const std::vector<std::string> collapsed = splat::wrap_licence_text("a  b\tc\r\nd", 600, kSize);
    REQUIRE(collapsed.size() == 1);
    CHECK(collapsed[0] == "a b c d");
    CHECK(splat::wrap_licence_text("", 600, kSize).size() == 1);
    CHECK(splat::wrap_licence_text("", 600, kSize)[0].empty());
}

TEST_CASE("ui_model_screens pages and scrolls the licence text") {
    FakeModelService models;
    Fixture fixture;
    fixture.services.models = &models;
    fixture.context.content = splat::Rect{16, 78, 1248, 80};  // one body line per page

    LicenceScreen* screen = fixture.open_licence("sharp");
    REQUIRE(screen != nullptr);
    CHECK(screen->text_available());
    CHECK(screen->model_id() == "sharp");
    CHECK(screen->model_name() == "SHARP");
    CHECK(screen->visible_lines() == 1);
    REQUIRE(screen->pages() >= 3);
    CHECK(screen->page() == 1);
    CHECK(screen->scroll() == 0);
    CHECK(screen->digest_prefix().size() == 12);

    // The SHA-256 prefix is the hash of exactly the text the manager will record.
    const std::string text = models.licence_text("sharp");
    CHECK(screen->digest_prefix() ==
          splat::Sha256::of_hex(text.data(), text.size()).substr(0, 12));

    // L1/R1 move a page; the page marker follows the scroll offset. The fake
    // text is three paragraphs: five lines with the blank ones.
    REQUIRE(screen->lines().size() == 5);
    fixture.stack.update(fixture.context, press(splat::PageNext));
    CHECK(screen->scroll() == 1);
    CHECK(screen->page() == 2);
    fixture.stack.update(fixture.context, press(splat::Down));
    CHECK(screen->scroll() == 2);
    CHECK(screen->page() == 3);
    fixture.stack.update(fixture.context, press(splat::PageNext));
    fixture.stack.update(fixture.context, press(splat::PageNext));
    CHECK(screen->scroll() == static_cast<int>(screen->lines().size()) - 1);
    CHECK(screen->page() == screen->pages());
    fixture.stack.update(fixture.context, press(splat::PageNext));  // no page after it
    CHECK(screen->scroll() == 4);
    fixture.stack.update(fixture.context, press(splat::PagePrev));
    fixture.stack.update(fixture.context, press(splat::PagePrev));
    fixture.stack.update(fixture.context, press(splat::PagePrev));
    CHECK(screen->scroll() == 1);
    CHECK(screen->page() == 2);
    fixture.stack.update(fixture.context, press(splat::Down));
    CHECK(screen->scroll() == 2);
    fixture.stack.update(fixture.context, press(splat::Up));
    CHECK(screen->scroll() == 1);

    // Drawing does not change the page under the user.
    splat::UiCanvas canvas(1280, 720);
    fixture.stack.draw(fixture.context, canvas, fixture.context.content);
    CHECK(screen->page() == 2);
}

TEST_CASE("ui_model_screens reaches the partial last licence page") {
    FakeModelService models;
    Fixture fixture;
    fixture.services.models = &models;
    // The shortest panel that shows two body lines per page.
    LicenceScreen* screen = nullptr;
    for (int height = 40; height < 400 && screen == nullptr; height += 2) {
        fixture.context.content = splat::Rect{64, 112, 1152, height};
        LicenceScreen* candidate = fixture.open_licence("sharp");
        REQUIRE(candidate != nullptr);
        if (candidate->visible_lines() == 2) {
            screen = candidate;
        } else {
            fixture.stack.pop();
        }
    }
    REQUIRE(screen != nullptr);
    REQUIRE(screen->visible_lines() == 2);
    REQUIRE(screen->lines().size() == 5);  // the last page is one line, not two
    REQUIRE(screen->pages() == 3);
    CHECK(screen->page() == 1);

    // R1 twice lands on the partial last page instead of stopping a page short.
    fixture.stack.update(fixture.context, press(splat::PageNext));
    CHECK(screen->scroll() == 2);
    CHECK(screen->page() == 2);
    fixture.stack.update(fixture.context, press(splat::PageNext));
    CHECK(screen->scroll() == 4);
    CHECK(screen->page() == 3);
    CHECK(screen->page() == screen->pages());

    // There is no page after it, and the clamp does not fall back a page.
    fixture.stack.update(fixture.context, press(splat::PageNext));
    CHECK(screen->scroll() == 4);
    CHECK(screen->page() == screen->pages());

    // The last page really shows the final line.
    CHECK(screen->scroll() + screen->visible_lines() >=
          static_cast<int>(screen->lines().size()));
    splat::UiCanvas canvas(1280, 720);
    fixture.stack.draw(fixture.context, canvas, fixture.context.content);
    CHECK(screen->page() == screen->pages());

    // L1 returns page by page to the first, so every page is reachable both ways.
    fixture.stack.update(fixture.context, press(splat::PagePrev));
    CHECK(screen->scroll() == 2);
    fixture.stack.update(fixture.context, press(splat::PagePrev));
    CHECK(screen->scroll() == 0);
    CHECK(screen->page() == 1);

    // Down/Up still stop at the first and last page, never past them.
    fixture.stack.update(fixture.context, press(splat::Down));
    fixture.stack.update(fixture.context, press(splat::Down));
    CHECK(screen->scroll() == 2);
    CHECK(screen->page() == 2);
    fixture.stack.update(fixture.context, press(splat::Up));
    CHECK(screen->scroll() == 1);
    CHECK(screen->page() == 1);
}

TEST_CASE("ui_model_screens a gated model opens the licence and accepts it") {
    FakeModelService models;
    Fixture fixture;
    fixture.services.models = &models;
    ModelsScreen* models_screen = fixture.open_models();
    REQUIRE(models_screen != nullptr);
    REQUIRE(models_screen->list().rows().size() == 3);  // two models + the default row
    CHECK(models_screen->list().focus() == 0);  // SHARP, licence required

    fixture.stack.update(fixture.context, press(splat::Select));
    CHECK(fixture.stack.size() == 2);
    CHECK(fixture.stack.top_id() == splat::ScreenId::Licence);
    CHECK(models.selected().empty());  // a gated row is not selected

    LicenceScreen* licence = dynamic_cast<LicenceScreen*>(fixture.stack.top());
    REQUIRE(licence != nullptr);
    CHECK(licence->model_id() == "sharp");
    CHECK(licence->text_available());

    fixture.stack.update(fixture.context, press(splat::Select));
    CHECK(fixture.toast.find("Accepted SHARP") != std::string::npos);
    CHECK(fixture.stack.size() == 1);  // back to Models
    CHECK(fixture.stack.top_id() == splat::ScreenId::Models);
    const std::vector<splat::ModelView> accepted_views = models.list();
    const splat::ModelView* sharp = find_view(accepted_views, "sharp");
    REQUIRE(sharp != nullptr);
    CHECK(sharp->state == "downloading");  // accept + download
}

TEST_CASE("ui_model_screens the default model row picks the converter") {
    FakeModelService models;
    Fixture fixture;
    fixture.services.models = &models;
    ModelsScreen* screen = fixture.open_models();
    REQUIRE(screen != nullptr);

    // SHARP is gated, DA2-Small is ready, the last row is the default model.
    REQUIRE(screen->list().rows().size() == 3);
    CHECK(screen->list().rows()[1].right.find("Ready") != std::string::npos);
    CHECK(screen->list().rows()[2].label == "Default model");
    // Nothing chosen yet: the first ready model is the one offered first.
    CHECK(screen->list().rows()[2].right == "DA2-Small");
    CHECK_FALSE(screen->list().rows()[2].dim);
    // Plain words: no licence names in the rows.
    CHECK(screen->list().rows()[0].sublabel.find("licence") == std::string::npos);
    CHECK(screen->list().rows()[1].sublabel.find("Apache") == std::string::npos);

    // Cross on a Ready model row does nothing by itself.
    fixture.stack.update(fixture.context, press(splat::Down));
    fixture.stack.update(fixture.context, press(splat::Select));
    CHECK(models.selected().empty());

    // Cross on the default row switches to the next Ready model.
    fixture.stack.update(fixture.context, press(splat::Down));
    REQUIRE(screen->on_default_row());
    CHECK(screen->hints()[0].label == "Change");
    fixture.stack.update(fixture.context, press(splat::Select));
    CHECK(models.selected() == "da2");
    CHECK(screen->default_name() == "DA2-Small");
    CHECK(screen->list().rows()[2].right == "DA2-Small");

    // With one Ready model it stays on it; a non-Ready model cannot be chosen.
    fixture.stack.update(fixture.context, press(splat::Select));
    CHECK(models.selected() == "da2");
    CHECK_FALSE(models.select("sharp").ok());
}

TEST_CASE("ui_model_screens the default model row says when nothing is installed") {
    FakeModelService models;
    REQUIRE(models.remove("da2").ok());
    Fixture fixture;
    fixture.services.models = &models;
    ModelsScreen* screen = fixture.open_models();
    REQUIRE(screen != nullptr);
    CHECK(screen->list().rows().back().dim);
    fixture.stack.update(fixture.context, press(splat::Down));
    fixture.stack.update(fixture.context, press(splat::Down));
    REQUIRE(screen->on_default_row());
    fixture.stack.update(fixture.context, press(splat::Select));
    CHECK(fixture.toast == "Download a model first");
}

TEST_CASE("ui_model_screens shows the download state and cancels with Square") {
    FakeModelService models;
    REQUIRE(models.accept("sharp").ok());
    models.tick(0.01);  // accepted -> downloading
    models.tick(FakeModelService::kDownloadSeconds * 0.5);

    Fixture fixture;
    fixture.services.models = &models;
    ModelsScreen* screen = fixture.open_models();
    REQUIRE(screen != nullptr);

    const splat::ModelView* sharp = find_view(screen->models(), "sharp");
    REQUIRE(sharp != nullptr);
    CHECK(sharp->state == "downloading");
    CHECK(screen->list().rows()[0].right.find("%") != std::string::npos);
    CHECK(screen->hints()[0].button == "Square");
    CHECK(screen->hints()[0].label == "Cancel");

    fixture.stack.update(fixture.context, press(splat::Secondary));
    CHECK(fixture.toast == "Cancelling SHARP");
    CHECK_FALSE(screen->confirm().is_open());  // cancel does not ask to delete
    CHECK(models.list()[0].state == "accepted");
}

TEST_CASE("ui_model_screens delete asks with Cancel focused first") {
    FakeModelService models;
    Fixture fixture;
    fixture.services.models = &models;
    ModelsScreen* screen = fixture.open_models();
    REQUIRE(screen != nullptr);
    // The card names the model and the bytes Delete frees (source plus
    // runtime file).
    fixture.stack.update(fixture.context, press(splat::Secondary));
    REQUIRE(screen->confirm().is_open());
    CHECK(screen->confirm().title() == "Delete \u201CSHARP\u201D?");
    CHECK(screen->confirm().detail().find("3.9 GB") != std::string::npos);
    CHECK(screen->confirm().focus() == 0);  // Cancel, as on the console
    CHECK(screen->hints()[1].label == "Cancel");

    // Cross on Cancel closes it and deletes nothing; the list does not move.
    fixture.stack.update(fixture.context, press(splat::Select));
    CHECK_FALSE(screen->confirm().is_open());
    CHECK(screen->list().focus() == 0);

    // DA2-Small: Right moves to Delete, Cross deletes it.
    fixture.stack.update(fixture.context, press(splat::Down));
    REQUIRE(screen->list().focus() == 1);
    fixture.stack.update(fixture.context, press(splat::Secondary));
    REQUIRE(screen->confirm().is_open());
    CHECK(screen->confirm().title() == "Delete \u201CDA2-Small\u201D?");
    fixture.stack.update(fixture.context, press(splat::Right));
    CHECK(screen->confirm().focus() == 1);
    fixture.stack.update(fixture.context, press(splat::Select));
    CHECK_FALSE(screen->confirm().is_open());
    CHECK(fixture.toast == "DA2-Small deleted");
    const std::vector<splat::ModelView> deleted_views = models.list();
    const splat::ModelView* da2 = find_view(deleted_views, "da2");
    REQUIRE(da2 != nullptr);
    CHECK(da2->state != "ready");
}

TEST_CASE("ui_model_screens circle closes the delete card") {
    FakeModelService models;
    Fixture fixture;
    fixture.services.models = &models;
    ModelsScreen* screen = fixture.open_models();
    REQUIRE(screen != nullptr);
    fixture.stack.update(fixture.context, press(splat::Down));
    fixture.stack.update(fixture.context, press(splat::Secondary));
    REQUIRE(screen->confirm().is_open());
    CHECK(screen->handle_back(fixture.context));
    CHECK_FALSE(screen->confirm().is_open());
    CHECK_FALSE(screen->handle_back(fixture.context));
    const std::vector<splat::ModelView> still_there = models.list();
    const splat::ModelView* da2 = find_view(still_there, "da2");
    REQUIRE(da2 != nullptr);
    CHECK(da2->state == "ready");
}

TEST_CASE("ui_model_screens handles a missing service and an unavailable licence") {
    Fixture fixture;
    ModelsScreen* screen = fixture.open_models();  // services.models is null
    REQUIRE(screen != nullptr);
    CHECK(screen->models().empty());
    splat::UiCanvas canvas(1280, 720);
    fixture.stack.draw(fixture.context, canvas, fixture.context.content);  // "not available"

    // The Licence screen says so instead of offering an acceptance.
    LicenceScreen* licence = fixture.open_licence("sharp");
    REQUIRE(licence != nullptr);
    CHECK_FALSE(licence->text_available());
    CHECK_FALSE(licence->lines().empty());
    fixture.stack.update(fixture.context, press(splat::Select));
    CHECK(fixture.toast.find("not available") != std::string::npos);
    CHECK(fixture.stack.size() == 2);  // nothing was accepted, screen stays
}
