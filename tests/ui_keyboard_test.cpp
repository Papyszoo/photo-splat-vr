// KeyboardScreen: typing on the grid (Cross and a pointer click), one-shot
// Shift, Square/Backspace, the validity line (empty, leading dot, collision,
// the 48-character limit, the reserved name), OK calling create_album() and
// rename_album() on a temp tree, the delete-album confirmation refusing a
// non-empty album, and the key layout having no NUL key.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/path.h"
#include "gallery/gallery.h"
#include "test_util.h"
#include "ui/app.h"
#include "ui/fakes.h"
#include "ui/screens/keyboard.h"
#include "ui/screen_stack.h"

using splat::Path;

namespace {

class CapturingScreen : public splat::Screen {
public:
    void on_enter(splat::UiContext&, const splat::ScreenArgs& args) override { last = args; }
    void update(splat::UiContext&, const splat::UiInput&) override {}
    void draw(splat::UiContext&, splat::UiCanvas&, const splat::Rect&) override {}
    std::string title() const override { return "stub"; }

    splat::ScreenArgs last;
};

splat::UiInput press(uint32_t action) {
    splat::UiInput input;
    input.pressed = action;
    input.dt = 1.0 / 60.0;
    return input;
}

// A scene the gallery recognises: a folder holding scene.psplat. meta.json and
// thumb.jpg are optional, so the gallery reports a default meta and no thumb.
void make_scene(const Path& folder) {
    std::filesystem::create_directories(folder);
    std::ofstream(folder / "scene.psplat") << "scene";
}

struct Fixture {
    splat_test::TempDir temp{"ui-keyboard"};
    Path root = temp.path() / "scenes";
    splat::UiServices services;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    std::vector<std::string> toasts;
    splat::KeyboardScreen* keyboard = nullptr;
    CapturingScreen* base = nullptr;

    Fixture() {
        std::filesystem::create_directories(root);
        services.paths.scenes = root;
        context.content = splat::Rect{16, 78, 1248, 586};
        context.toast_sink = [this](std::string text, double) { toasts.push_back(std::move(text)); };
        stack.set_context(&context);

        registry.add(splat::ScreenId::Settings, [this] {
            auto screen = std::make_unique<CapturingScreen>();
            base = screen.get();
            return screen;
        });
        registry.add(splat::ScreenId::Keyboard, [this] {
            auto screen = std::make_unique<splat::KeyboardScreen>();
            keyboard = screen.get();
            return screen;
        });
        REQUIRE(stack.push(splat::ScreenId::Settings));
    }

    void enter(const std::string& mode, const std::string& text) {
        splat::ScreenArgs args;
        args.a = mode;
        args.b = text;
        REQUIRE(stack.push(splat::ScreenId::Keyboard, args));
        REQUIRE(keyboard != nullptr);
    }

    void tap(uint32_t action) { stack.update(context, press(action)); }

    int find_char(char character) const {
        for (int index = 0; index < static_cast<int>(keyboard->keys().size()); ++index) {
            const splat::KeyboardKey& key = keyboard->keys()[static_cast<std::size_t>(index)];
            if (key.kind == splat::KeyboardKey::Kind::Character && key.character == character) {
                return index;
            }
        }
        return -1;
    }

    int find_kind(splat::KeyboardKey::Kind kind) const {
        for (int index = 0; index < static_cast<int>(keyboard->keys().size()); ++index) {
            if (keyboard->keys()[static_cast<std::size_t>(index)].kind == kind) return index;
        }
        return -1;
    }

    // Focus a key with the real focus setter, then press Cross on it.
    void key_char(char character) {
        const int index = find_char(character);
        REQUIRE(index >= 0);
        keyboard->focus_key(index);
        tap(splat::Select);
    }

    void type(const std::string& text) {
        for (const char character : text) key_char(character);
    }

    void press_ok() {
        const int index = find_kind(splat::KeyboardKey::Kind::Ok);
        REQUIRE(index >= 0);
        keyboard->focus_key(index);
        tap(splat::Select);
    }

    void click_key(int index) {
        const splat::Rect box = keyboard->key_rect(index);
        splat::UiInput input;
        input.pointer_valid = true;
        input.px = static_cast<float>(box.x + box.w / 2);
        input.py = static_cast<float>(box.y + box.h / 2);
        input.click = true;
        input.dt = 1.0 / 60.0;
        stack.update(context, input);
    }
};

}  // namespace

TEST_CASE("ui_keyboard types letters, shift, backspace and a pointer click") {
    Fixture fixture;
    fixture.enter("new-album", "");

    CHECK(fixture.keyboard->mode() == "new-album");
    CHECK(fixture.keyboard->title() == "New folder");
    CHECK_FALSE(fixture.keyboard->overlay());
    CHECK(fixture.keyboard->text().empty());
    CHECK_FALSE(fixture.keyboard->valid());

    fixture.type("ab");
    CHECK(fixture.keyboard->text() == "ab");

    // Shift applies to exactly one letter, then resets.
    fixture.keyboard->focus_key(fixture.find_kind(splat::KeyboardKey::Kind::Shift));
    fixture.tap(splat::Select);
    CHECK(fixture.keyboard->shift_active());
    fixture.key_char('c');
    CHECK(fixture.keyboard->text() == "abC");
    CHECK_FALSE(fixture.keyboard->shift_active());

    // Square is a backspace regardless of where the focus is.
    fixture.tap(splat::Secondary);
    CHECK(fixture.keyboard->text() == "ab");

    // A pointer click on a key types it. The grid widget maps the pixel to the
    // cell, so this uses the same path as the Sense laser.
    fixture.click_key(fixture.find_char('d'));
    CHECK(fixture.keyboard->text() == "abd");
    CHECK(fixture.keyboard->valid());

    const std::vector<splat::Hint> hints = fixture.keyboard->hints();
    REQUIRE(hints.size() == 3);
    CHECK(hints[0].button == "Cross");
    CHECK(hints[0].label == "Type");
    CHECK(hints[1].button == "Square");
    CHECK(hints[1].label == "Backspace");
    CHECK(hints[2].button == "Circle");

    fixture.press_ok();
    CHECK(fixture.stack.top_id() == splat::ScreenId::Settings);
    CHECK(std::filesystem::is_directory(fixture.root / "abd"));
    REQUIRE_FALSE(fixture.toasts.empty());
    CHECK(fixture.toasts.back().find("Created folder abd") != std::string::npos);
}

TEST_CASE("ui_keyboard validity line: empty, dot, collision and the 48 character cap") {
    Fixture fixture;
    fixture.enter("new-album", "");

    CHECK_FALSE(fixture.keyboard->valid());
    CHECK(fixture.keyboard->validity_text().find("Type a name") != std::string::npos);

    // OK is disabled while invalid: it neither pops nor creates anything.
    fixture.press_ok();
    CHECK(fixture.stack.top_id() == splat::ScreenId::Keyboard);
    CHECK(std::filesystem::is_empty(fixture.root));

    // A leading dot is outside the gallery's name rules.
    fixture.key_char('.');
    CHECK(fixture.keyboard->text() == ".");
    CHECK_FALSE(fixture.keyboard->valid());
    CHECK(fixture.keyboard->validity_text().find("dot") != std::string::npos);
    fixture.press_ok();
    CHECK(fixture.stack.top_id() == splat::ScreenId::Keyboard);

    fixture.tap(splat::Secondary);
    CHECK(fixture.keyboard->text().empty());

    // A name that already exists is refused before the gallery call.
    std::filesystem::create_directories(fixture.root / "taken");
    fixture.type("taken");
    CHECK(fixture.keyboard->text() == "taken");
    CHECK_FALSE(fixture.keyboard->valid());
    CHECK(fixture.keyboard->validity_text().find("already exists") != std::string::npos);
    fixture.press_ok();
    CHECK(fixture.stack.top_id() == splat::ScreenId::Keyboard);

    for (int index = 0; index < 5; ++index) fixture.tap(splat::Secondary);
    REQUIRE(fixture.keyboard->text().empty());

    // 49 characters is over the limit; deleting one makes the name valid again.
    fixture.type(std::string(49, 'a'));
    CHECK(fixture.keyboard->text().size() == 49);
    CHECK_FALSE(fixture.keyboard->valid());
    CHECK(fixture.keyboard->validity_text().find("48") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(fixture.root / std::string(49, 'a')));

    fixture.tap(splat::Secondary);
    CHECK(fixture.keyboard->text().size() == 48);
    CHECK(fixture.keyboard->valid());

    // The reserved implicit-album name is not a legal album folder name.
    Fixture reserved;
    reserved.enter("new-album", splat::kAllScenesAlbumName);
    CHECK_FALSE(reserved.keyboard->valid());
    CHECK(reserved.keyboard->validity_text().find("reserved") != std::string::npos);
}

TEST_CASE("ui_keyboard OK calls create_album and rename_album on the temp tree") {
    Fixture fixture;
    const std::string date = "2026-10-26";
    fixture.enter("new-album", date);
    CHECK(fixture.keyboard->text() == date);
    CHECK(fixture.keyboard->valid());

    fixture.press_ok();
    CHECK(fixture.stack.top_id() == splat::ScreenId::Settings);
    CHECK(std::filesystem::is_directory(fixture.root / date));
    REQUIRE_FALSE(fixture.toasts.empty());
    CHECK(fixture.toasts.back().find("Created folder " + date) != std::string::npos);

    // Rename: the old name is the initial text; remove it and type the new one.
    std::filesystem::create_directories(fixture.root / "old");
    splat::ScreenArgs args;
    args.a = "rename-album";
    args.b = "old";
    REQUIRE(fixture.stack.push(splat::ScreenId::Keyboard, args));
    REQUIRE(fixture.keyboard != nullptr);
    CHECK(fixture.keyboard->title() == "Rename folder");
    CHECK(fixture.keyboard->text() == "old");

    for (int index = 0; index < 3; ++index) fixture.tap(splat::Secondary);
    CHECK(fixture.keyboard->text().empty());
    fixture.type("new");
    CHECK(fixture.keyboard->text() == "new");
    CHECK(fixture.keyboard->valid());

    fixture.press_ok();
    CHECK(fixture.stack.top_id() == splat::ScreenId::Settings);
    CHECK(std::filesystem::is_directory(fixture.root / "new"));
    CHECK_FALSE(std::filesystem::exists(fixture.root / "old"));
    CHECK(fixture.toasts.back().find("Renamed folder to new") != std::string::npos);
}

TEST_CASE("ui_keyboard layout has no null key and every key types a real character") {
    Fixture fixture;
    fixture.enter("new-album", "");

    const std::vector<splat::KeyboardKey>& keys = fixture.keyboard->keys();

    // A string-literal digit array used to leak its '\0' terminator in as a
    // blank 11th key that typed a NUL byte and shifted the whole grid. The
    // action keys (Shift/Backspace/OK) legitimately leave `character` at 0, so
    // the check is on the character keys.
    for (const splat::KeyboardKey& key : keys) {
        CHECK(key.label.find('\0') == std::string::npos);
        CHECK_FALSE(key.label.empty());
        if (key.kind == splat::KeyboardKey::Kind::Character) CHECK(key.character != '\0');
    }

    // The digit row is the first ten keys, in order, with no extra key.
    std::string digits;
    for (const splat::KeyboardKey& key : keys) {
        if (key.kind != splat::KeyboardKey::Kind::Character) continue;
        if (key.character >= '0' && key.character <= '9') digits.push_back(key.character);
    }
    CHECK(digits == "1234567890");
    CHECK(keys[10].character == 'q');  // the QWERTY row starts here

    // QWERTY, not ABC (headset test 6): the letter rows are qwertyuiop,
    // asdfghjkl and zxcvbnm, with Shift ending the home row.
    std::string letters;
    for (const splat::KeyboardKey& key : keys) {
        if (key.kind == splat::KeyboardKey::Kind::Character &&
            key.character >= 'a' && key.character <= 'z') {
            letters.push_back(key.character);
        }
    }
    CHECK(letters == "qwertyuiopasdfghjklzxcvbnm");
    CHECK(keys[20].character == 'a');
    CHECK(keys[29].kind == splat::KeyboardKey::Kind::Shift);
    CHECK(keys[30].character == 'z');
    CHECK(keys[37].character == '.');
    CHECK(keys[40].character == ' ');
    CHECK(keys[41].kind == splat::KeyboardKey::Kind::Backspace);
    CHECK(keys[42].kind == splat::KeyboardKey::Kind::Ok);

    // Typing through every key puts exactly the key characters in the field.
    int typed_keys = 0;
    for (int index = 0; index < static_cast<int>(keys.size()); ++index) {
        if (keys[static_cast<std::size_t>(index)].kind != splat::KeyboardKey::Kind::Character) continue;
        ++typed_keys;
        fixture.keyboard->focus_key(index);
        fixture.tap(splat::Select);
    }
    const std::string& text = fixture.keyboard->text();
    CHECK(text.find('\0') == std::string::npos);
    CHECK(text.size() == static_cast<std::size_t>(typed_keys));
    CHECK(text == "1234567890qwertyuiopasdfghjklzxcvbnm.-_ ");
}

TEST_CASE("ui_keyboard delete mode refuses a non-empty album and deletes an empty one") {
    Fixture fixture;
    make_scene(fixture.root / "full" / "s1");

    splat::ScreenArgs args;
    args.a = "delete-album";
    args.b = "full";
    REQUIRE(fixture.stack.push(splat::ScreenId::Keyboard, args));
    REQUIRE(fixture.keyboard != nullptr);
    CHECK(fixture.keyboard->title() == "Delete folder");
    // The confirmation is a card over the dimmed folder grid.
    CHECK(fixture.keyboard->overlay());
    CHECK_FALSE(fixture.keyboard->valid());
    CHECK(fixture.keyboard->validity_text().find("scene") != std::string::npos);
    // The card: Cancel focused; a refused Delete cannot take the focus.
    REQUIRE(fixture.keyboard->confirm().is_open());
    CHECK(fixture.keyboard->confirm().title() == "Delete “full”?");
    CHECK_FALSE(fixture.keyboard->confirm().allowed());
    CHECK(fixture.keyboard->confirm().focus() == 0);
    fixture.tap(splat::Right);
    CHECK(fixture.keyboard->confirm().focus() == 0);

    // Cross on Cancel leaves; nothing was removed.
    fixture.tap(splat::Select);
    CHECK(fixture.stack.top_id() == splat::ScreenId::Settings);
    CHECK(std::filesystem::is_directory(fixture.root / "full"));

    // An empty album: Cancel first (still nothing removed), then Delete.
    std::filesystem::create_directories(fixture.root / "empty");
    args.b = "empty";
    REQUIRE(fixture.stack.push(splat::ScreenId::Keyboard, args));
    CHECK(fixture.keyboard->valid());
    CHECK(fixture.keyboard->confirm().focus() == 0);
    fixture.tap(splat::Select);
    CHECK(fixture.stack.top_id() == splat::ScreenId::Settings);
    CHECK(std::filesystem::exists(fixture.root / "empty"));

    REQUIRE(fixture.stack.push(splat::ScreenId::Keyboard, args));
    fixture.tap(splat::Right);
    CHECK(fixture.keyboard->confirm().focus() == 1);
    fixture.tap(splat::Select);
    CHECK(fixture.stack.top_id() == splat::ScreenId::Settings);
    CHECK_FALSE(std::filesystem::exists(fixture.root / "empty"));
    CHECK(fixture.toasts.back().find("Deleted folder empty") != std::string::npos);
}

TEST_CASE("ui_keyboard renames a scene inside its folder") {
    Fixture fixture;
    make_scene(fixture.root / "trip" / "old-name");
    make_scene(fixture.root / "trip" / "taken");
    splat::ScreenArgs args;
    args.a = "rename-scene";
    args.b = (fixture.root / "trip" / "old-name").string();
    REQUIRE(fixture.stack.push(splat::ScreenId::Keyboard, args));
    REQUIRE(fixture.keyboard != nullptr);
    CHECK(fixture.keyboard->title() == "Rename scene");
    CHECK(fixture.keyboard->text() == "old-name");
    CHECK(fixture.keyboard->valid());  // the current name is fine

    // A name already used in that folder is refused.
    for (int index = 0; index < 8; ++index) fixture.tap(splat::Secondary);
    REQUIRE(fixture.keyboard->text().empty());
    for (const char* key : {"t", "a", "k", "e", "n"}) {
        const std::vector<splat::KeyboardKey>& keys = fixture.keyboard->keys();
        for (int index = 0; index < static_cast<int>(keys.size()); ++index) {
            if (keys[static_cast<std::size_t>(index)].label == key) fixture.keyboard->focus_key(index);
        }
        fixture.tap(splat::Select);
    }
    CHECK(fixture.keyboard->text() == "taken");
    CHECK_FALSE(fixture.keyboard->valid());
    CHECK(fixture.keyboard->validity_text().find("already exists") != std::string::npos);

    // One more letter and OK renames it in place.
    for (int index = 0; index < static_cast<int>(fixture.keyboard->keys().size()); ++index) {
        if (fixture.keyboard->keys()[static_cast<std::size_t>(index)].label == "2") {
            fixture.keyboard->focus_key(index);
        }
    }
    fixture.tap(splat::Select);
    REQUIRE(fixture.keyboard->valid());
    for (int index = 0; index < static_cast<int>(fixture.keyboard->keys().size()); ++index) {
        if (fixture.keyboard->keys()[static_cast<std::size_t>(index)].kind ==
            splat::KeyboardKey::Kind::Ok) {
            fixture.keyboard->focus_key(index);
        }
    }
    fixture.tap(splat::Select);
    CHECK(fixture.stack.top_id() == splat::ScreenId::Settings);
    CHECK(std::filesystem::is_directory(fixture.root / "trip" / "taken2"));
    CHECK_FALSE(std::filesystem::exists(fixture.root / "trip" / "old-name"));
    CHECK(fixture.toasts.back() == "Renamed scene to taken2");
}

TEST_CASE("ui_keyboard holding the d-pad repeats through UiApp") {
    // The repeat is generated once in UiApp (ui_input.h's UiRepeat) and folded
    // into each frame's pressed bits, so the keyboard's grid gets the same
    // acceleration as every list and grid (headset test 6: "I can't move fast").
    splat_test::TempDir temp{"ui-keyboard-repeat"};
    splat::UiServices services;
    services.paths.scenes = temp.path() / "scenes";
    std::filesystem::create_directories(services.paths.scenes);
    splat::UiApp app(services);
    splat::KeyboardScreen* keyboard = nullptr;
    app.registry().add(splat::ScreenId::Albums, [] {
        class Stub : public splat::Screen {
        public:
            void update(splat::UiContext&, const splat::UiInput&) override {}
            void draw(splat::UiContext&, splat::UiCanvas&, const splat::Rect&) override {}
            std::string title() const override { return "Scenes"; }
        };
        return std::make_unique<Stub>();
    });
    app.registry().add(splat::ScreenId::Keyboard, [&keyboard] {
        auto screen = std::make_unique<splat::KeyboardScreen>();
        keyboard = screen.get();
        return screen;
    });
    app.start();
    splat::ScreenArgs args;
    args.a = "new-album";
    REQUIRE(app.nav().push(splat::ScreenId::Keyboard, args));
    REQUIRE(keyboard != nullptr);
    const int start = keyboard->focus();

    splat::UiInput press;
    press.pressed = splat::Down;
    press.held = splat::Down;
    press.dt = 1.0 / 60.0;
    app.update(press);
    const int after_one = keyboard->focus();
    CHECK(after_one > start);

    splat::UiInput hold;
    hold.held = splat::Down;
    hold.dt = 1.0 / 60.0;
    for (int frame = 0; frame < 60; ++frame) app.update(hold);
    CHECK(keyboard->focus() > after_one + 2);
}

TEST_CASE("ui_keyboard fits the 896x504 VR canvas with the 0.7 theme") {
    // Review fix: the VR panel (platform/ps5/viewer/ui_setup.cpp) draws the
    // 1280x720 design on 896x504 with the theme scaled by 896/1280, and the
    // keyboard's five rows plus the field have to end inside content.bottom().
    // With the unscaled theme the content area was 316 px high and rows 3-4
    // were culled by draw_keys().
    splat_test::TempDir temp{"ui-keyboard-vr"};
    splat::UiServices services;
    services.paths.scenes = temp.path() / "scenes";
    std::filesystem::create_directories(services.paths.scenes);

    const float factor = 896.0f / 1280.0f;
    const splat::UiTheme vr = splat::scaled_theme(splat::default_theme(), factor);
    // The same design at 0.7: fonts, row height and margins all scale, and the
    // TV's theme (factor 1) keeps its own numbers exactly.
    CHECK(vr.scale == doctest::Approx(factor));
    CHECK(vr.margin == 45);
    CHECK(vr.padding == 11);
    CHECK(vr.row_height == 50);
    CHECK(vr.small_size == 14);
    CHECK(vr.body_size == 15);
    CHECK(vr.heading_size == 20);
    CHECK(vr.title_size == 28);
    const splat::UiTheme tv = splat::scaled_theme(splat::default_theme(), 1.0f);
    CHECK(tv.margin == 64);
    CHECK(tv.row_height == 72);
    CHECK(tv.body_size == 22);
    CHECK(vr.scaled(58) == 41);  // the keyboard's key height, 10 px -> 7

    splat::UiApp app(services, vr);
    splat::KeyboardScreen* keyboard = nullptr;
    app.registry().add(splat::ScreenId::Albums, [] {
        class Stub : public splat::Screen {
        public:
            void update(splat::UiContext&, const splat::UiInput&) override {}
            void draw(splat::UiContext&, splat::UiCanvas&, const splat::Rect&) override {}
            std::string title() const override { return "Scenes"; }
        };
        return std::make_unique<Stub>();
    });
    app.registry().add(splat::ScreenId::Keyboard, [&keyboard] {
        auto screen = std::make_unique<splat::KeyboardScreen>();
        keyboard = screen.get();
        return screen;
    });
    app.start();
    splat::ScreenArgs args;
    args.a = "new-album";
    REQUIRE(app.nav().push(splat::ScreenId::Keyboard, args));
    REQUIRE(keyboard != nullptr);

    splat::UiCanvas canvas(896, 504);
    REQUIRE(app.draw(canvas));  // the first frame is dirty
    const splat::Rect content = app.context().content;
    CHECK(content.bottom() <= 504);
    CHECK(content.right() <= 896);

    // Every key of every row is fully inside the content area and the canvas,
    // so draw_keys()' cull (`box.y >= content.bottom()`) skips none of them.
    const std::vector<splat::KeyboardKey>& keys = keyboard->keys();
    REQUIRE(keys.size() == 43);
    const splat::Rect area = keyboard->keys_area();
    CHECK(area.y > content.y);
    CHECK(area.bottom() <= content.bottom());
    CHECK(area.bottom() <= 504);
    for (int index = 0; index < static_cast<int>(keys.size()); ++index) {
        const splat::Rect box = keyboard->key_rect(index);
        CHECK(box.y >= content.y);
        CHECK(box.bottom() <= content.bottom());
        CHECK(box.x >= 0);
        CHECK(box.right() <= 896);
        CHECK(box.bottom() <= 504);
        const bool culled = box.y >= content.bottom() || box.y + box.h <= content.y;
        CHECK_FALSE(culled);
    }
    // The rows the review saw disappear: zxcvbnm ('z' at 30) is row 4 in the
    // 10-column grid, and Space/Delete/OK are the last three keys.
    CHECK(keys[30].character == 'z');
    CHECK(keys[29].kind == splat::KeyboardKey::Kind::Shift);
    CHECK(keys[40].label == "Space");
    CHECK(keys[41].label == "Delete");
    CHECK(keys[42].kind == splat::KeyboardKey::Kind::Ok);
}

TEST_CASE("ui_keyboard asks the system text input first and uses its result") {
    class StubTextInput : public splat::ITextInput {
    public:
        bool available() const override { return true; }
        splat::Status open(const std::string& title, const std::string& initial) override {
            opened_title = title;
            opened_initial = initial;
            open_ = true;
            return splat::Status::success();
        }
        State poll(std::string* text) override {
            if (!open_) return State::idle;
            open_ = false;
            *text = "from-ime";
            return State::accepted;
        }
        void close() override { open_ = false; }

        bool open_ = false;
        std::string opened_title;
        std::string opened_initial;
    };

    Fixture fixture;
    StubTextInput ime;
    fixture.services.text_input = &ime;
    fixture.enter("new-album", "");
    CHECK(ime.open_);
    CHECK(ime.opened_title == "New folder");
    CHECK(fixture.keyboard->system_keyboard_open());

    // Any frame polls it; an accepted result is the screen's OK.
    fixture.tap(0);
    CHECK(fixture.stack.top_id() == splat::ScreenId::Settings);
    CHECK(std::filesystem::is_directory(fixture.root / "from-ime"));
    REQUIRE_FALSE(fixture.toasts.empty());
    CHECK(fixture.toasts.back().find("Created folder from-ime") != std::string::npos);
}

TEST_CASE("ui_keyboard falls back to the grid when the system text input is unavailable") {
    Fixture fixture;
    splat::FakeTextInput text_input;  // the desktop/fake implementation
    fixture.services.text_input = &text_input;
    fixture.enter("new-album", "");
    CHECK_FALSE(fixture.keyboard->system_keyboard_open());
    fixture.type("grid");
    CHECK(fixture.keyboard->text() == "grid");
    CHECK(fixture.keyboard->valid());
}
