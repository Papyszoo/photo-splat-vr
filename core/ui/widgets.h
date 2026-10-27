// Focusable list and grid widgets, shared by every screen, plus the popup menu
// behind the context button. All are pure of services: the screen fills
// rows/cells, calls update() with the frame input and the area, then draw().
// Focus never wraps; PagePrev/PageNext move a list by one page; the focused
// item is always scrolled into view.
//
// Look: rows are plain text until focused, when they get a light plate with a
// white ring; grid cells are rounded image tiles with their caption below, and
// the focused tile grows a little with a white ring and a blue halo.
//
// Pointer input (the Sense laser or a mouse) hovers a row/cell to move focus,
// and a click activates it exactly like Cross. `dim` only changes the drawing:
// the screen decides what a dim row means (an unavailable feature, an HDR
// capture) and may toast when it is activated.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ui/canvas.h"
#include "ui/image.h"
#include "ui/rect.h"
#include "ui/services.h"
#include "ui/theme.h"
#include "ui/ui_input.h"

namespace splat {

struct ListRow {
    std::string label;
    std::string sublabel;  // drawn after the label, dim
    std::string right;     // right-aligned, dim
    bool dim = false;      // drawn in dim_text, still focusable
    bool add = false;      // an action row ("New folder"): a "+" before the label
    // A section heading (Settings' "Comfort" / "Display"): small, dim, never
    // focused or activated; it only groups the rows below it.
    bool header = false;
};

class ListView {
public:
    void set_rows(std::vector<ListRow> rows);
    const std::vector<ListRow>& rows() const { return rows_; }

    // Replace one row's right-aligned text in place, for screens that keep a
    // live status there (e.g. Settings' "n running") without rebuilding the
    // rows. Out-of-range indices are ignored.
    void set_right(int index, std::string right);

    int focus() const { return focus_; }
    void set_focus(int index);
    int scroll() const { return scroll_; }

    // Rows that fit in `area`; at least 1.
    int visible_rows(const Rect& area, const UiTheme& theme) const;

    // Focus movement, paging, pointer hover/click and scrolling.
    void update(const UiInput& input, const Rect& area, const UiTheme& theme);
    void draw(UiCanvas& canvas, const Rect& area, const UiTheme& theme) const;

    // Set by Cross or a pointer click this frame; the screen clears it after
    // acting on it.
    bool activated() const { return activated_; }
    void clear_activated() { activated_ = false; }

private:
    void ensure_visible(int visible);

    std::vector<ListRow> rows_;
    int focus_ = 0;
    int scroll_ = 0;
    bool activated_ = false;
};

struct GridCell {
    std::string caption;
    std::string sublabel;           // optional second caption line
    const UiImage* thumb = nullptr; // optional thumbnail, cropped to fill the tile
    bool dim = false;

    // What the tile shows instead of a thumbnail. None draws the labelled
    // placeholder (core/ui/screens/placeholder.h) for the caption.
    enum class Icon { None, Add, Folder } icon = Icon::None;
    bool stack = false;     // a folder: two plates peek out above the image
    bool checked = false;   // a selection mark in the corner
    float progress = -1.0f; // >= 0: a progress ring over the image
    std::string badge;      // short state under the ring ("Queued")
};

class GridView {
public:
    // Cell size and gap in pixels; columns follow from the area width. The
    // image fills the cell above the label area: room for `caption_lines`
    // caption lines (the focused tile wraps its name onto them; the others
    // keep one line) and, with `show_sublabel`, one sublabel line.
    int cell_width = 192;
    int cell_height = 160;
    int gap = 16;
    int caption_lines = 1;
    bool show_sublabel = true;
    // The sublabel only under the focused tile (a scene's model name).
    bool sublabel_focus_only = false;

    void set_cells(std::vector<GridCell> cells);
    const std::vector<GridCell>& cells() const { return cells_; }
    // In-place access for live state (a progress ring) without resetting the
    // scroll; null when out of range.
    GridCell* cell(int index) {
        return index >= 0 && index < static_cast<int>(cells_.size())
                   ? &cells_[static_cast<std::size_t>(index)]
                   : nullptr;
    }

    int focus() const { return focus_; }
    void set_focus(int index);
    int scroll_row() const { return scroll_row_; }

    int columns(const Rect& area) const;
    int visible_rows(const Rect& area) const;
    // Where cell `index` lands with the current scroll, for pointer hit tests
    // and for screens that draw a marker.
    Rect cell_rect(int index, const Rect& area) const;

    void update(const UiInput& input, const Rect& area, const UiTheme& theme);
    void draw(UiCanvas& canvas, const Rect& area, const UiTheme& theme) const;

    bool activated() const { return activated_; }
    void clear_activated() { activated_ = false; }

    // The image part of a cell box.
    Rect image_rect(const Rect& cell, const UiTheme& theme) const;

private:
    void draw_cell(UiCanvas& canvas, const GridCell& cell, const Rect& box, bool focused,
                   const UiTheme& theme) const;

    std::vector<GridCell> cells_;
    int focus_ = 0;
    int scroll_row_ = 0;
    bool activated_ = false;
};

// The context menu: a small card of actions over a veiled screen, opened by
// the screen's context button (Triangle). The screen owns it, routes input to
// it while it is open and closes it from Screen::handle_back().
class PopupMenu {
public:
    void open(std::string title, std::vector<std::string> items);
    void close() { open_ = false; }
    bool is_open() const { return open_; }

    // The index of the item chosen this frame (Cross or a click), else -1. A
    // choice closes the menu.
    int update(const UiInput& input, const Rect& content, const UiTheme& theme);
    void draw(UiCanvas& canvas, const Rect& content, const UiTheme& theme) const;

    const std::string& title() const { return title_; }
    const std::vector<std::string>& items() const { return items_; }
    int focus() const { return list_.focus(); }
    void set_focus(int index) { list_.set_focus(index); }

private:
    Rect card(const Rect& content, const UiTheme& theme) const;
    Rect list_area(const Rect& content, const UiTheme& theme) const;

    std::string title_;
    std::vector<std::string> items_;
    ListView list_;
    bool open_ = false;
};

// A thin progress bar under the right-hand text of ListView row `row` (counted
// from the first visible row) drawn in `area`: the running conversion, the
// model download. `scrollbar` mirrors the list's own track.
void draw_row_progress(UiCanvas& canvas, const Rect& area, int row, std::string_view right,
                       float fraction, bool scrollbar, const UiTheme& theme);

// The one conversion status, in the top bar and in Settings: a chip with a
// ring for the running photo's progress, "Converting" and how many photos are
// still to go. Right-aligned at `right`, centred on `cy`; returns its width.
struct ConversionStatus {
    int active = 0;          // queued + running jobs
    int queued = 0;          // waiting behind the running one
    float fraction = 0.0f;   // the running job's progress
};
// "Converting", or "Converting · 2 queued": the chip's words.
std::string conversion_label(const ConversionStatus& status);
ConversionStatus conversion_status(const IJobService* jobs);
int draw_conversion_chip(UiCanvas& canvas, int right, int cy, const ConversionStatus& status,
                         const UiTheme& theme);
// Its width (0 when nothing converts), for layouts that leave room for it.
int conversion_chip_width(const ConversionStatus& status, const UiTheme& theme);

// A floating card (popup, confirmation): a soft shadow, the card colour and a
// faint rim.
void draw_card(UiCanvas& canvas, const Rect& box, const UiTheme& theme);

// Every destructive confirmation: a card with a question, one line of detail
// and two buttons, Cancel and the action (white on red). Cancel has the focus
// when it opens, as on the console; Left/Right move between the buttons, Cross
// or a click presses one, and the screen maps Circle to close(). A refused
// action (`allowed` false) keeps only Cancel usable.
class ConfirmCard {
public:
    enum class Choice { None, Cancel, Confirm };

    void open(std::string title, std::string detail, std::string action, bool allowed = true);
    void close() { open_ = false; }
    bool is_open() const { return open_; }
    // The button with the focus: 0 = Cancel, 1 = the action.
    int focus() const { return focus_; }
    const std::string& title() const { return title_; }
    const std::string& detail() const { return detail_; }
    bool allowed() const { return allowed_; }

    // What this frame chose; any choice closes the card.
    Choice update(const UiInput& input, const Rect& content);
    void draw(UiCanvas& canvas, const Rect& content, const UiTheme& theme) const;

    // The card and its two buttons, for the pointer and the tests.
    static Rect card_rect(const Rect& content);
    static Rect button_rect(const Rect& content, int index);

private:
    std::string title_;
    std::string detail_;
    std::string action_;
    bool allowed_ = true;
    bool open_ = false;
    int focus_ = 0;
};

// The rounded focus treatment every focusable box uses: a soft halo, then a
// white ring `gap` px outside the box.
void draw_focus_ring(UiCanvas& canvas, const Rect& box, int radius, const UiTheme& theme,
                     int gap = 4);

}  // namespace splat
