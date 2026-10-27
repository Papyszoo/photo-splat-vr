// The panel keyboard: the shipped way to name an album (the PS5 system IME is
// not relied on). It is pushed with `a` = mode and `b` = the starting text:
//
//   "new-album"    b = the suggested name (default_album_name()) or ""
//   "rename-album" b = the existing album folder name
//   "rename-scene" b = the scene folder path (renamed inside its album)
//   "delete-album" b = the existing album folder name (confirmation only)
//
// The grid is QWERTY (VR9, headset test 6): the digit row 1-0, qwertyuiop,
// asdfghjkl + Shift, zxcvbnm, then '.', '-', '_', Space, Delete and OK. The
// d-pad/left stick moves the focus (holding repeats, UiRepeat in UiApp), Cross
// types (or toggles Shift / deletes / confirms), Square is a second Backspace,
// and a pointer click on a key types it. OK stays disabled while the name is
// invalid: the empty name, a leading dot, more than 48 characters, a name
// outside the gallery's alphabet, the reserved "Unsorted" name, and a name
// that already exists.
//
// The system keyboard (the PS5 IME) is asked first through
// UiServices::text_input: when it is available the screen opens it with the
// current text and uses its result; otherwise (the desktop/fake build, and the
// console until the IME spike lands) the grid above is the fallback. VR9 adds
// only the hook, not the console dialog.
//
// On OK the screen calls create_album()/rename_album(), toasts the result and
// pops; the caller re-lists the filesystem on return (on_enter runs again).
//
// delete-album only confirms: a card `Delete "X"?` with Cancel (focused) and
// Delete over the dimmed folder grid (an overlay). delete_album_checked()
// refuses a non-empty album with a toast and leaves the card up with only
// Cancel, so the user sees why nothing was removed.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "common/path.h"
#include "common/status.h"
#include "ui/rect.h"
#include "ui/screen.h"
#include "ui/widgets.h"

namespace splat {

// One key of the on-panel keyboard.
struct KeyboardKey {
    enum class Kind { Character, Shift, Backspace, Ok };

    Kind kind = Kind::Character;
    char character = 0;   // Character: the character typed (space included)
    std::string label;    // the label drawn on the key ("Space", "Shift", ...)
};

// Deletes an empty album after checking it exists and holds no scene. This is
// the helper the album screen (W10) uses from KeyboardScreen's delete mode; the
// filesystem rules live in gallery.cpp, so the checks repeat nothing.
//
// Returns a message a screen can toast. `Result<void>` would say the same, but
// core/common/status.h's Result<T> stores a T and so has no void form; Status
// is this codebase's void result.
Status delete_album_checked(const Path& root, const std::string& album);

class KeyboardScreen : public Screen {
public:
    void on_enter(UiContext& ctx, const ScreenArgs& args) override;
    void update(UiContext& ctx, const UiInput& input) override;
    void draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) override;
    std::vector<Hint> hints() const override;
    std::string title() const override;
    bool overlay() const override { return delete_mode_; }
    // Circle while the system dialog is open abandons it first.
    bool handle_back(UiContext& ctx) override;

    // The mode the screen was entered with ("new-album", "rename-album",
    // "rename-scene" or "delete-album").
    const std::string& mode() const { return mode_; }
    // delete-album's Cancel / Delete card.
    const ConfirmCard& confirm() const { return confirm_; }
    // The text typed so far; for delete-album, the album being confirmed.
    const std::string& text() const { return text_; }
    bool shift_active() const { return shift_; }
    // True while the system dialog (ITextInput) owns the input.
    bool system_keyboard_open() const { return ime_open_; }
    // True when OK would call the gallery (or, in delete mode, when the album
    // is empty and may be removed).
    bool valid() const { return valid_; }
    // One line for the validity area ("That name already exists", ...).
    const std::string& validity_text() const { return validity_; }

    const std::vector<KeyboardKey>& keys() const { return keys_; }
    int focus() const { return grid_.focus(); }
    void focus_key(int index) { grid_.set_focus(index); }
    // The drawn box of a key, for pointer tests and for the preview.
    Rect key_rect(int index) const { return grid_.cell_rect(index, keys_area_); }
    const Rect& keys_area() const { return keys_area_; }

private:
    void build_keys();
    // Sizes the grid from the content rect and the theme's scale (VR9: the VR
    // panel's 0.7 theme must fit all five rows in the shorter area as well).
    void layout(const Rect& content, const UiTheme& theme);
    void refresh_validity();
    void type_character(char character);
    void backspace();
    void confirm_name(UiContext& ctx);
    void confirm_delete(UiContext& ctx);
    void update_delete(UiContext& ctx, const UiInput& input);
    // The system-dialog hook (VR9): start it when the service is available,
    // poll it while open, and act on its result. False when the panel grid
    // should be used.
    bool begin_system_keyboard(UiContext& ctx);
    bool update_system_keyboard(UiContext& ctx);
    std::string display_label(const KeyboardKey& key) const;

    void draw_field(UiContext& ctx, UiCanvas& canvas, const Rect& content);
    void draw_keys(UiContext& ctx, UiCanvas& canvas, const Rect& content);
    void draw_delete(UiContext& ctx, UiCanvas& canvas, const Rect& content);


    std::string mode_ = "new-album";
    std::string source_;  // rename/delete: the album folder name (a scene's path) in `b`
    std::string current_name_;  // the name being renamed
    Path name_dir_;             // where the name lives: the root, or the scene's folder
    ConfirmCard confirm_;       // delete-album: Cancel / Delete
    std::string text_;
    std::string validity_;
    Path root_;
    bool valid_ = false;
    bool shift_ = false;
    bool delete_mode_ = false;
    // The system dialog (ITextInput) owns the input while this is true.
    bool ime_open_ = false;

    GridView grid_;
    std::vector<KeyboardKey> keys_;
    Rect keys_area_;
    // The theme-scaled metrics layout() derives from the design constants:
    // 1 on the TV, 0.7 on the VR canvas.
    int field_height_ = 60;
    int field_inset_ = 24;
    int validity_top_ = 70;
    int keys_top_ = 112;
};

}  // namespace splat
