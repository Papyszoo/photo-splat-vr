// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/screens/keyboard.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "gallery/gallery.h"
#include "ui/screen_stack.h"
#include "ui/text_fit.h"

namespace splat {
namespace {

constexpr int kColumns = 10;
// Design pixels, written for the 1280x720 TV canvas and multiplied through
// UiTheme::scaled() in layout(), so the VR panel's 896x504 canvas (theme scale
// 0.7) draws the same keyboard at the same physical size and nothing clips.
constexpr int kKeyGap = 10;
constexpr int kKeyHeight = 58;
constexpr int kFieldHeight = 60;
constexpr int kFieldInset = 24;  // the typed text's left inset in the field
constexpr int kValidityTop = kFieldHeight + 10;
constexpr int kKeysTop = kFieldHeight + 52;
constexpr int kKeyRadius = 10;
// Long enough to type past the 48-character gallery limit and see the validity
// line say so, but still one line at body scale.
constexpr std::size_t kMaxTyped = 64;

bool is_letter(char c) { return c >= 'a' && c <= 'z'; }

std::string upper(char c) { return std::string(1, static_cast<char>(c - 'a' + 'A')); }

}  // namespace

Status delete_album_checked(const Path& root, const std::string& album) {
    if (album.empty() || album == kAllScenesAlbumName) {
        return Status::error("cannot delete the loose-scene album");
    }

    const Path dir = root / album;
    std::error_code error;
    if (!std::filesystem::is_directory(dir, error) || error) {
        return Status::error("album not found: " + album);
    }
    const std::vector<SceneEntry> scenes = list_scenes(root, album);
    if (!scenes.empty()) return Status::error("album is not empty: " + album);

    // The scene check above gives the clearer message; delete_album() still
    // repeats it and additionally refuses stray files inside the album.
    return delete_album(root, album);
}

void KeyboardScreen::on_enter(UiContext& ctx, const ScreenArgs& args) {
    mode_ = args.a.empty() ? std::string("new-album") : args.a;
    delete_mode_ = mode_ == "delete-album";
    source_ = args.b;
    root_ = ctx.svc.paths.scenes;
    shift_ = false;
    // A scene is renamed inside its own folder; albums live in the root.
    if (mode_ == "rename-scene") {
        name_dir_ = Path(source_).parent_path();
        current_name_ = Path(source_).filename().string();
    } else {
        name_dir_ = root_;
        current_name_ = source_;
    }
    text_ = current_name_;

    build_keys();
    layout(ctx.content, ctx.theme);
    grid_.set_focus(0);
    refresh_validity();
    if (delete_mode_) {
        confirm_.open("Delete \u201C" + source_ + "\u201D?", validity_, "Delete", valid_);
        return;
    }
    // The system dialog is asked first; the grid above is the fallback.
    begin_system_keyboard(ctx);
}

bool KeyboardScreen::begin_system_keyboard(UiContext& ctx) {
    if (delete_mode_ || ctx.svc.text_input == nullptr) return false;
    splat::ITextInput& input = *ctx.svc.text_input;
    if (!input.available()) return false;
    const Status opened = input.open(title(), text_);
    if (!opened.ok()) {
        if (ctx.svc.log) ctx.svc.log("ui: keyboard: system dialog: " + opened.message());
        return false;
    }
    ime_open_ = true;
    return true;
}

bool KeyboardScreen::update_system_keyboard(UiContext& ctx) {
    if (!ime_open_ || ctx.svc.text_input == nullptr) return false;
    std::string text;
    const ITextInput::State state = ctx.svc.text_input->poll(&text);
    if (state == ITextInput::State::pending) return true;
    ime_open_ = false;
    if (state == ITextInput::State::accepted) {
        text_ = text;
        refresh_validity();
        // The dialog's OK is the screen's OK: a valid name is applied at once,
        // an invalid one stays on the grid with the validity line visible.
        if (valid_) confirm_name(ctx);
        return true;
    }
    // Cancelled (or the dialog disappeared): leave the screen as Circle would,
    // so a cancelled dialog never traps the user on a keyboard with no focus.
    ctx.nav.pop();
    return true;
}

bool KeyboardScreen::handle_back(UiContext& ctx) {
    if (!ime_open_) return false;
    if (ctx.svc.text_input != nullptr) ctx.svc.text_input->close();
    ime_open_ = false;
    return true;
}

void KeyboardScreen::confirm_name(UiContext& ctx) {
    Status status = Status::success();
    std::string done;
    if (mode_ == "rename-scene") {
        status = rename_scene(root_, Path(source_), text_);
        done = "Renamed scene to " + text_;
    } else if (mode_ == "rename-album") {
        status = rename_album(root_, source_, text_);
        done = "Renamed folder to " + text_;
    } else {
        status = create_album(root_, text_);
        done = "Created folder " + text_;
    }
    ctx.toast(status.ok() ? done : status.message());
    ctx.nav.pop();
}

void KeyboardScreen::build_keys() {
    keys_.clear();

    // QWERTY (headset test 6: "QWERTY, not ABC"): the digit row, qwertyuiop,
    // asdfghjkl + Shift, zxcvbnm, then the symbols and the action keys. An
    // initializer list, not a char array literal: the latter carries the '\0'
    // terminator and would add a blank key that types a null byte.
    for (const char* row : {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"}) {
        for (const char* letter = row; *letter != '\0'; ++letter) {
            keys_.push_back(
                KeyboardKey{KeyboardKey::Kind::Character, *letter, std::string(1, *letter)});
        }
        if (std::string(row) == "asdfghjkl") {
            keys_.push_back(KeyboardKey{KeyboardKey::Kind::Shift, 0, "Shift"});
        }
    }
    for (const char symbol : {'.', '-', '_'}) {
        keys_.push_back(
            KeyboardKey{KeyboardKey::Kind::Character, symbol, std::string(1, symbol)});
    }
    keys_.push_back(KeyboardKey{KeyboardKey::Kind::Character, ' ', "Space"});
    keys_.push_back(KeyboardKey{KeyboardKey::Kind::Backspace, 0, "Delete"});
    keys_.push_back(KeyboardKey{KeyboardKey::Kind::Ok, 0, "OK"});

    std::vector<GridCell> cells;
    cells.reserve(keys_.size());
    for (const KeyboardKey& key : keys_) {
        GridCell cell;
        cell.caption = key.label;
        cells.push_back(std::move(cell));
    }
    grid_.set_cells(std::move(cells));
}

void KeyboardScreen::layout(const Rect& content, const UiTheme& theme) {
    // One pass of the theme's scale for every design-time size: 1 on the TV
    // (unchanged), 0.7 on the VR canvas, where the five rows and the field then
    // fit the shorter content area instead of being cut off at content.bottom().
    const int gap = std::max(1, theme.scaled(kKeyGap));
    const int key_height = std::max(1, theme.scaled(kKeyHeight));
    field_height_ = std::max(1, theme.scaled(kFieldHeight));
    field_inset_ = theme.scaled(kFieldInset);
    validity_top_ = theme.scaled(kValidityTop);
    keys_top_ = theme.scaled(kKeysTop);

    grid_.gap = gap;
    grid_.cell_height = key_height;
    grid_.cell_width = std::max(theme.scaled(40),
                                (content.w - (kColumns - 1) * gap) / kColumns);
    keys_area_ = Rect{content.x, content.y + keys_top_, content.w,
                      (key_height + gap) * 5 - gap};
}

void KeyboardScreen::refresh_validity() {
    validity_.clear();
    valid_ = false;

    if (delete_mode_) {
        if (source_.empty() || source_ == kAllScenesAlbumName) {
            validity_ = "Unsorted is not a folder and cannot be deleted";
            return;
        }
        std::error_code error;
        if (!std::filesystem::is_directory(root_ / source_, error) || error) {
            validity_ = "Folder not found: " + source_;
            return;
        }
        const std::size_t scenes = list_scenes(root_, source_).size();
        if (scenes != 0) {
            validity_ = scenes == 1 ? "The folder holds a scene, so it stays"
                                    : "The folder holds scenes, so it stays";
            return;
        }
        valid_ = true;
        validity_ = "The folder is empty and will be removed.";
        return;
    }

    if (text_.empty()) {
        validity_ = "Type a name";
        return;
    }
    if (text_.size() > 48) {
        validity_ = "Name is too long (48 characters max)";
        return;
    }
    if (text_.front() == '.') {
        validity_ = "Name cannot start with a dot";
        return;
    }
    if (!valid_album_name(text_)) {
        validity_ = "Use A-Z, 0-9, dot, dash, underscore and space";
        return;
    }
    if (text_ == kAllScenesAlbumName) {
        validity_ = "That name is reserved";
        return;
    }

    const bool renaming = mode_ == "rename-album" || mode_ == "rename-scene";
    if (renaming && text_ == current_name_) {
        valid_ = true;
        validity_ = "OK keeps the current name";
        return;
    }

    std::error_code error;
    if (std::filesystem::exists(name_dir_ / text_, error) && !error) {
        validity_ = "That name already exists";
        return;
    }

    valid_ = true;
    if (mode_ == "rename-scene") {
        validity_ = "OK renames the scene";
    } else {
        validity_ = renaming ? "OK renames the folder" : "OK creates the folder";
    }
}

void KeyboardScreen::type_character(char character) {
    if (text_.size() >= kMaxTyped) return;
    if (shift_ && is_letter(character)) {
        text_.push_back(static_cast<char>(character - 'a' + 'A'));
    } else {
        text_.push_back(character);
    }
    shift_ = false;
    refresh_validity();
}

void KeyboardScreen::backspace() {
    if (!text_.empty()) text_.pop_back();
    refresh_validity();
}

void KeyboardScreen::update(UiContext& ctx, const UiInput& input) {
    if (update_system_keyboard(ctx)) return;
    if (delete_mode_) {
        update_delete(ctx, input);
        return;
    }
    if (ctx.content.empty()) return;
    layout(ctx.content, ctx.theme);

    grid_.update(input, keys_area_, ctx.theme);

    if ((input.pressed & Secondary) != 0) {
        grid_.clear_activated();
        backspace();
        return;
    }
    if (!grid_.activated()) return;
    grid_.clear_activated();

    const int focus = grid_.focus();
    if (focus < 0 || focus >= static_cast<int>(keys_.size())) return;
    const KeyboardKey& key = keys_[static_cast<std::size_t>(focus)];

    switch (key.kind) {
        case KeyboardKey::Kind::Character:
            type_character(key.character);
            break;
        case KeyboardKey::Kind::Shift:
            shift_ = !shift_;
            break;
        case KeyboardKey::Kind::Backspace:
            backspace();
            break;
        case KeyboardKey::Kind::Ok: {
            if (!valid_) return;  // OK is disabled while the name is invalid.
            confirm_name(ctx);
            break;
        }
    }
}

void KeyboardScreen::update_delete(UiContext& ctx, const UiInput& input) {
    switch (confirm_.update(input, ctx.content)) {
        case ConfirmCard::Choice::None: return;
        case ConfirmCard::Choice::Cancel: ctx.nav.pop(); return;
        case ConfirmCard::Choice::Confirm: confirm_delete(ctx); return;
    }
}

void KeyboardScreen::confirm_delete(UiContext& ctx) {
    const Status status = delete_album_checked(root_, source_);
    if (!status.ok()) {
        ctx.toast(status.message());
        // Refused: the card stays up with the reason and only Cancel.
        refresh_validity();
        confirm_.open("Delete \u201C" + source_ + "\u201D?", validity_, "Delete", false);
        return;
    }
    ctx.toast("Deleted folder " + source_);
    ctx.nav.pop();
}

std::string KeyboardScreen::display_label(const KeyboardKey& key) const {
    if (key.kind != KeyboardKey::Kind::Character) return key.label;
    if (key.character == ' ') return "Space";
    if (shift_ && is_letter(key.character)) return upper(key.character);
    return std::string(1, key.character);
}

void KeyboardScreen::draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) {
    if (content.empty()) return;
    layout(content, ctx.theme);

    if (delete_mode_) {
        draw_delete(ctx, canvas, content);
        return;
    }

    draw_field(ctx, canvas, content);
    draw_keys(ctx, canvas, content);
}

void KeyboardScreen::draw_field(UiContext& ctx, UiCanvas& canvas, const Rect& content) {
    const UiTheme& theme = ctx.theme;
    const int size = theme.heading_size;

    canvas.fill_round_rect(content.x, content.y, content.w, field_height_, theme.radius,
                           Rgba{255, 255, 255, 30});
    canvas.stroke_round_rect(content.x, content.y, content.w, field_height_, theme.radius, 2,
                             valid_ ? Rgba{255, 255, 255, 90} : theme.warning);

    const int text_y = UiCanvas::text_top_centered(content.y, field_height_, size);
    const int text_x = content.x + field_inset_;
    const uint32_t typed_width = canvas.text(text_x, text_y, text_, size, theme.text);
    const int cap = UiCanvas::line_height(size);
    canvas.fill_rect(text_x + static_cast<int>(typed_width) + theme.scaled(3),
                     content.y + (field_height_ - cap) / 2, std::max(1, theme.scaled(2)), cap,
                     theme.accent);

    canvas.text(content.x + theme.scaled(4), content.y + validity_top_, validity_,
                theme.small_size, valid_ ? theme.dim_text : theme.warning);
}

void KeyboardScreen::draw_keys(UiContext& ctx, UiCanvas& canvas, const Rect& content) {
    const UiTheme& theme = ctx.theme;
    const int radius = std::max(1, theme.scaled(kKeyRadius));

    for (int index = 0; index < static_cast<int>(keys_.size()); ++index) {
        const Rect box = grid_.cell_rect(index, keys_area_);
        if (box.y >= content.bottom() || box.y + box.h <= content.y) continue;

        const KeyboardKey& key = keys_[static_cast<std::size_t>(index)];
        const bool focused = index == grid_.focus();
        const bool active_shift = key.kind == KeyboardKey::Kind::Shift && shift_;
        const bool disabled = key.kind == KeyboardKey::Kind::Ok && !valid_;
        const bool ok = key.kind == KeyboardKey::Kind::Ok && valid_;
        Rgba plate = theme.panel;
        if (ok) plate = Rgba{64, 148, 255, 200};
        if (active_shift) plate = theme.selected;
        if (focused) {
            draw_focus_ring(canvas, box, theme.scaled(10), theme, 0);
            plate = theme.light;
        }
        canvas.fill_round_rect(box.x, box.y, box.w, box.h, radius, plate);
        if (active_shift) {
            const int inset = theme.scaled(16);
            canvas.fill_round_rect(box.x + inset, box.bottom() - theme.scaled(7),
                                   box.w - 2 * inset, std::max(1, theme.scaled(3)), 1,
                                   theme.accent);
        }

        const std::string label = display_label(key);
        const bool word = label.size() > 1;
        const int size = word ? theme.small_size : theme.body_size + 2;
        const int width = static_cast<int>(UiCanvas::text_width(label, size));
        const Rgba ink = focused ? theme.background_bottom
                                 : (disabled ? theme.faint_text : theme.text);
        canvas.text(box.x + (box.w - width) / 2, UiCanvas::text_top_centered(box.y, box.h, size),
                    label, size, ink);
    }
}

void KeyboardScreen::draw_delete(UiContext& ctx, UiCanvas& canvas, const Rect& content) {
    confirm_.draw(canvas, content, ctx.theme);
}

std::vector<Hint> KeyboardScreen::hints() const {
    if (delete_mode_) return {Hint{"Cross", "Choose"}, Hint{"Circle", "Cancel"}};
    return {Hint{"Cross", "Type"}, Hint{"Square", "Backspace"}, Hint{"Circle", "Cancel"}};
}

std::string KeyboardScreen::title() const {
    if (delete_mode_) return "Delete folder";
    if (mode_ == "rename-scene") return "Rename scene";
    if (mode_ == "rename-album") return "Rename folder";
    return "New folder";
}

}  // namespace splat
