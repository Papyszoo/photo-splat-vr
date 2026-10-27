// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#include "ui/screens/placeholder.h"
#include "ui/text_fit.h"

namespace splat {
namespace {

int clamp_int(int value, int low, int high) { return std::max(low, std::min(value, high)); }

// Section headings are never a focus target: `from` is moved to the nearest
// selectable row in `direction` (0 = from itself, then forward).
int selectable_row(const std::vector<ListRow>& rows, int from, int direction = 0) {
    const int count = static_cast<int>(rows.size());
    if (count == 0) return 0;
    int index = clamp_int(from, 0, count - 1);
    const int step = direction >= 0 ? 1 : -1;
    while (index >= 0 && index < count && rows[static_cast<std::size_t>(index)].header) {
        index += step;
    }
    if (index >= 0 && index < count) return index;
    // No selectable row in that direction: look the other way before giving up.
    index = clamp_int(from, 0, count - 1);
    while (index >= 0 && index < count && rows[static_cast<std::size_t>(index)].header) {
        index -= step;
    }
    return index >= 0 && index < count ? index : 0;
}

constexpr int kRowInset = 4;  // vertical gap between two row plates
constexpr int kRowRadius = 12;

// The "+" and folder icons drawn on an empty tile.
void draw_icon(UiCanvas& canvas, GridCell::Icon icon, const Rect& box, const UiTheme& theme,
               bool focused) {
    const Rgba ink = focused ? theme.text : theme.dim_text;
    const float cx = static_cast<float>(box.x) + static_cast<float>(box.w) * 0.5f;
    const float cy = static_cast<float>(box.y) + static_cast<float>(box.h) * 0.5f;
    const float unit = static_cast<float>(std::min(box.w, box.h));
    if (icon == GridCell::Icon::Add) {
        const float arm = unit * 0.16f;
        const float stroke = std::max(3.0f, unit * 0.035f);
        canvas.line(cx - arm, cy, cx + arm, cy, stroke, ink);
        canvas.line(cx, cy - arm, cx, cy + arm, stroke, ink);
        return;
    }
    // Folder: a tab and a body.
    const int w = static_cast<int>(unit * 0.46f);
    const int h = static_cast<int>(unit * 0.32f);
    const int x = static_cast<int>(cx) - w / 2;
    const int y = static_cast<int>(cy) - h / 2 + h / 10;
    canvas.fill_round_rect(x, y - h / 6, w * 2 / 5, h / 3, 4, ink);
    canvas.fill_round_rect(x, y, w, h, 6, ink);
}

}  // namespace

void draw_focus_ring(UiCanvas& canvas, const Rect& box, int radius, const UiTheme& theme,
                     int gap) {
    canvas.glow_round_rect(box.x - gap, box.y - gap, box.w + 2 * gap, box.h + 2 * gap,
                           radius + gap, 16, theme.glow);
    canvas.stroke_round_rect(box.x - gap, box.y - gap, box.w + 2 * gap, box.h + 2 * gap,
                             radius + gap, 3, theme.focus);
}

void draw_row_progress(UiCanvas& canvas, const Rect& area, int row, std::string_view right,
                       float fraction, bool scrollbar, const UiTheme& theme) {
    const int plate_y = area.y + row * theme.row_height + kRowInset;
    const int plate_h = theme.row_height - 2 * kRowInset;
    const int plate_right = area.right() - (scrollbar ? 12 : 0);
    const int text_w = static_cast<int>(UiCanvas::text_width(right, theme.small_size));
    const int w = std::max(140, text_w);
    const int x = plate_right - 24 - w;
    const int y = plate_y + plate_h / 2 + 14;
    canvas.fill_round_rect(x, y, w, 4, 2, Rgba{255, 255, 255, 40});
    const float clamped = std::clamp(std::isfinite(fraction) ? fraction : 0.0f, 0.0f, 1.0f);
    const int filled = static_cast<int>(static_cast<float>(w) * clamped + 0.5f);
    if (filled > 0) canvas.fill_round_rect(x, y, std::max(4, filled), 4, 2, theme.accent);
}

ConversionStatus conversion_status(const IJobService* jobs) {
    ConversionStatus status;
    if (jobs == nullptr) return status;
    for (const JobView& job : jobs->snapshot()) {
        if (job.state == "running") status.fraction = job.fraction;
        if (job.state == "queued") ++status.queued;
        if (job.state == "running" || job.state == "queued") ++status.active;
    }
    return status;
}

std::string conversion_label(const ConversionStatus& status) {
    if (status.queued <= 0) return "Converting";
    return "Converting \xC2\xB7 " + std::to_string(status.queued) + " queued";
}

namespace {
constexpr int kChipHeight = 34;
constexpr int kChipRing = 8;
}  // namespace

int conversion_chip_width(const ConversionStatus& status, const UiTheme& theme) {
    if (status.active <= 0) return 0;
    const int label_w =
        static_cast<int>(UiCanvas::text_width(conversion_label(status), theme.small_size));
    return 14 + 2 * kChipRing + 10 + label_w + 16;
}

int draw_conversion_chip(UiCanvas& canvas, int right, int cy, const ConversionStatus& status,
                         const UiTheme& theme) {
    const int width = conversion_chip_width(status, theme);
    if (width == 0) return 0;
    constexpr int kHeight = kChipHeight;
    constexpr int kRing = kChipRing;
    const int size = theme.small_size;
    const int x = right - width;
    const int top = cy - kHeight / 2;
    canvas.fill_round_rect(x, top, width, kHeight, kHeight / 2, Rgba{255, 255, 255, 26});
    const float ring_x = static_cast<float>(x + 14 + kRing);
    canvas.stroke_circle(ring_x, static_cast<float>(cy), static_cast<float>(kRing), 3.0f,
                         Rgba{255, 255, 255, 46});
    canvas.stroke_arc(ring_x, static_cast<float>(cy), static_cast<float>(kRing), 3.0f,
                      std::max(status.fraction, 0.02f), theme.accent);
    canvas.text(x + 14 + 2 * kRing + 10, UiCanvas::text_top_centered(top, kHeight, size),
                conversion_label(status), size, theme.text);
    return width;
}

void draw_card(UiCanvas& canvas, const Rect& box, const UiTheme& theme) {
    constexpr int kRadius = 22;
    canvas.glow_round_rect(box.x, box.y, box.w, box.h, kRadius, 30, Rgba{0, 0, 0, 120});
    canvas.fill_round_rect(box.x, box.y, box.w, box.h, kRadius, theme.card);
    canvas.stroke_round_rect(box.x, box.y, box.w, box.h, kRadius, 1, Rgba{255, 255, 255, 30});
}

void ListView::set_rows(std::vector<ListRow> rows) {
    rows_ = std::move(rows);
    focus_ = rows_.empty() ? 0 : selectable_row(rows_, focus_);
    scroll_ = rows_.empty() ? 0 : clamp_int(scroll_, 0, static_cast<int>(rows_.size()) - 1);
    activated_ = false;
}

void ListView::set_right(int index, std::string right) {
    if (index < 0 || index >= static_cast<int>(rows_.size())) return;
    std::string& slot = rows_[static_cast<std::size_t>(index)].right;
    if (slot != right) slot = std::move(right);
}

void ListView::set_focus(int index) {
    if (rows_.empty()) {
        focus_ = 0;
        return;
    }
    focus_ = selectable_row(rows_, clamp_int(index, 0, static_cast<int>(rows_.size()) - 1));
}

int ListView::visible_rows(const Rect& area, const UiTheme& theme) const {
    const int row_height = std::max(1, theme.row_height);
    return std::max(1, area.h / row_height);
}

void ListView::ensure_visible(int visible) {
    if (rows_.empty()) {
        scroll_ = 0;
        return;
    }
    if (focus_ < scroll_) scroll_ = focus_;
    if (focus_ >= scroll_ + visible) scroll_ = focus_ - visible + 1;
    const int max_scroll = std::max(0, static_cast<int>(rows_.size()) - visible);
    scroll_ = clamp_int(scroll_, 0, max_scroll);
}

void ListView::update(const UiInput& input, const Rect& area, const UiTheme& theme) {
    if (rows_.empty()) {
        focus_ = 0;
        scroll_ = 0;
        activated_ = false;
        return;
    }
    const int count = static_cast<int>(rows_.size());
    const int visible = visible_rows(area, theme);

    // Headings are skipped: Up/Down stops on the next real row, and a page
    // moves `visible` selectable rows (not rows, which may include headings).
    auto step_focus = [&](int direction, int steps) {
        int index = focus_;
        for (int step = 0; step < steps; ++step) {
            int next = index + direction;
            while (next >= 0 && next < count && rows_[static_cast<std::size_t>(next)].header) {
                next += direction;
            }
            if (next < 0 || next >= count) break;
            index = next;
        }
        focus_ = index;
    };
    if ((input.pressed & Up) != 0) step_focus(-1, 1);
    if ((input.pressed & Down) != 0) step_focus(1, 1);
    if ((input.pressed & PagePrev) != 0) step_focus(-1, visible);
    if ((input.pressed & PageNext) != 0) step_focus(1, visible);

    if (input.pointer_valid && area.contains(input.px, input.py)) {
        const int row = scroll_ + static_cast<int>((input.py - static_cast<float>(area.y)) /
                                                   static_cast<float>(std::max(1, theme.row_height)));
        if (row >= 0 && row < count && !rows_[static_cast<std::size_t>(row)].header) {
            focus_ = row;
            if (input.click) activated_ = true;
        }
    }

    if ((input.pressed & Select) != 0) activated_ = true;
    ensure_visible(visible);
}

void ListView::draw(UiCanvas& canvas, const Rect& area, const UiTheme& theme) const {
    const int row_height = std::max(1, theme.row_height);
    const int visible = visible_rows(area, theme);
    const int label_size = theme.body_size;
    const int sub_size = theme.small_size;
    const int label_h = UiCanvas::line_height(label_size);
    const int sub_h = UiCanvas::line_height(sub_size);
    const int scrollbar = static_cast<int>(rows_.size()) > visible ? 12 : 0;

    for (int row = 0; row < visible; ++row) {
        const int index = scroll_ + row;
        if (index >= static_cast<int>(rows_.size())) break;
        const ListRow& item = rows_[static_cast<std::size_t>(index)];
        const bool focused = index == focus_;
        const Rect plate{area.x, area.y + row * row_height + kRowInset, area.w - scrollbar,
                         row_height - 2 * kRowInset};

        if (item.header) {
            // A section heading: a small, dim line with no plate or focus ring.
            canvas.text(plate.x + 24,
                        UiCanvas::text_top_centered(plate.y, plate.h, sub_size),
                        item.label, sub_size, theme.dim_text);
            continue;
        }

        if (focused) {
            draw_focus_ring(canvas, plate, kRowRadius, theme, 0);
            canvas.fill_round_rect(plate.x, plate.y, plate.w, plate.h, kRowRadius, theme.selected);
        }

        int x = plate.x + 24;
        if (item.add) {
            // An action row: a "+" in front, like the "+" tiles.
            const float cx = static_cast<float>(x + 11);
            const float cy = static_cast<float>(plate.y + plate.h / 2);
            const Rgba ink = focused ? theme.text : theme.dim_text;
            canvas.line(cx - 9.0f, cy, cx + 9.0f, cy, 3.0f, ink);
            canvas.line(cx, cy - 9.0f, cx, cy + 9.0f, 3.0f, ink);
            x += 36;
        }
        const int right_size = sub_size;
        const int right_width =
            item.right.empty() ? 0 : static_cast<int>(UiCanvas::text_width(item.right, right_size));
        const int right_x = plate.right() - 24 - right_width;
        const int text_limit =
            std::max(0, (item.right.empty() ? plate.right() - 24 : right_x - 24) - x);

        const Rgba label_color =
            item.dim ? theme.faint_text : (focused ? theme.text : Rgba{222, 228, 240, 255});
        if (item.sublabel.empty()) {
            canvas.text(x, UiCanvas::text_top_centered(plate.y, plate.h, label_size),
                        fit_text(item.label, label_size, text_limit), label_size, label_color);
        } else {
            const int block = label_h + sub_h - 2;
            const int top = plate.y + (plate.h - block) / 2;
            canvas.text(x, top, fit_text(item.label, label_size, text_limit), label_size,
                        label_color);
            canvas.text(x, top + label_h - 2, fit_text(item.sublabel, sub_size, text_limit),
                        sub_size, item.dim ? theme.faint_text : theme.dim_text);
        }
        if (!item.right.empty()) {
            canvas.text(right_x, UiCanvas::text_top_centered(plate.y, plate.h, right_size),
                        item.right, right_size, focused ? theme.text : theme.dim_text);
        }
    }

    if (scrollbar > 0) {
        // A thin track with a thumb for the visible part of a long list.
        const int track_h = visible * row_height - 2 * kRowInset;
        const int track_x = area.right() - 4;
        const int track_y = area.y + kRowInset;
        canvas.fill_round_rect(track_x, track_y, 4, track_h, 2, theme.panel);
        const int count = static_cast<int>(rows_.size());
        const int thumb_h = std::max(24, track_h * visible / count);
        const int thumb_y =
            track_y + (track_h - thumb_h) * scroll_ / std::max(1, count - visible);
        canvas.fill_round_rect(track_x, thumb_y, 4, thumb_h, 2, theme.dim_text);
    }
}

void GridView::set_cells(std::vector<GridCell> cells) {
    cells_ = std::move(cells);
    focus_ = cells_.empty() ? 0 : clamp_int(focus_, 0, static_cast<int>(cells_.size()) - 1);
    scroll_row_ = 0;
    activated_ = false;
}

void GridView::set_focus(int index) {
    if (cells_.empty()) {
        focus_ = 0;
        return;
    }
    focus_ = clamp_int(index, 0, static_cast<int>(cells_.size()) - 1);
}

int GridView::columns(const Rect& area) const {
    const int stride = std::max(1, cell_width + gap);
    return std::max(1, (area.w + gap) / stride);
}

int GridView::visible_rows(const Rect& area) const {
    const int stride = std::max(1, cell_height + gap);
    return std::max(1, (area.h + gap) / stride);
}

Rect GridView::cell_rect(int index, const Rect& area) const {
    const int per_row = columns(area);
    const int column = index % per_row;
    const int row = index / per_row - scroll_row_;
    return Rect{area.x + column * (cell_width + gap), area.y + row * (cell_height + gap), cell_width,
                cell_height};
}

Rect GridView::image_rect(const Rect& cell, const UiTheme& theme) const {
    int text_block = 0;
    if (caption_lines >= 1) {
        text_block += 10 + caption_lines * UiCanvas::line_height(theme.body_size - 2);
        if (show_sublabel) text_block += UiCanvas::line_height(theme.small_size);
    }
    return Rect{cell.x, cell.y, cell.w, std::max(1, cell.h - text_block)};
}

void GridView::update(const UiInput& input, const Rect& area, const UiTheme& theme) {
    (void)theme;
    if (cells_.empty()) {
        focus_ = 0;
        scroll_row_ = 0;
        activated_ = false;
        return;
    }
    const int count = static_cast<int>(cells_.size());
    const int per_row = columns(area);
    const int visible = visible_rows(area);

    if ((input.pressed & Left) != 0 && focus_ % per_row > 0) focus_ -= 1;
    if ((input.pressed & Right) != 0 && focus_ + 1 < count && (focus_ + 1) % per_row != 0) focus_ += 1;
    // Vertical steps keep the column: no move when there is no row above, and no
    // move when the item directly below does not exist (top row, last row, or a
    // partial row below that is too short for this column).
    if ((input.pressed & Up) != 0 && focus_ >= per_row) focus_ -= per_row;
    if ((input.pressed & Down) != 0 && focus_ + per_row < count) focus_ += per_row;

    if (input.pointer_valid && area.contains(input.px, input.py)) {
        const int column = static_cast<int>(input.px - static_cast<float>(area.x)) /
                           std::max(1, cell_width + gap);
        const int row = scroll_row_ + static_cast<int>(input.py - static_cast<float>(area.y)) /
                                          std::max(1, cell_height + gap);
        const int cell_x = area.x + column * (cell_width + gap);
        const int cell_y = area.y + (row - scroll_row_) * (cell_height + gap);
        const bool inside_cell = input.px >= static_cast<float>(cell_x) &&
                                 input.px < static_cast<float>(cell_x + cell_width) &&
                                 input.py >= static_cast<float>(cell_y) &&
                                 input.py < static_cast<float>(cell_y + cell_height);
        const int index = row * per_row + column;
        if (inside_cell && column < per_row && row >= 0 && index >= 0 && index < count) {
            focus_ = index;
            if (input.click) activated_ = true;
        }
    }

    if ((input.pressed & Select) != 0) activated_ = true;

    // Scroll the focused row into view.
    const int focus_row = focus_ / per_row;
    if (focus_row < scroll_row_) scroll_row_ = focus_row;
    if (focus_row >= scroll_row_ + visible) scroll_row_ = focus_row - visible + 1;
    const int total_rows = (count + per_row - 1) / per_row;
    scroll_row_ = clamp_int(scroll_row_, 0, std::max(0, total_rows - visible));
}

void GridView::draw_cell(UiCanvas& canvas, const GridCell& cell, const Rect& box, bool focused,
                         const UiTheme& theme) const {
    const int radius = theme.radius;
    Rect image = image_rect(box, theme);
    if (focused) {
        // The focused tile grows by about 5 % around its centre.
        const int grow_x = std::max(4, image.w / 40);
        const int grow_y = grow_x * image.h / std::max(1, image.w);
        image = Rect{image.x - grow_x, image.y - grow_y, image.w + 2 * grow_x,
                     image.h + 2 * grow_y};
    }

    if (cell.stack) {
        // Two thinner plates behind the cover: this tile is a folder.
        canvas.fill_round_rect(image.x + 22, image.y - 12, image.w - 44, 16, radius / 2,
                               Rgba{255, 255, 255, static_cast<uint8_t>(focused ? 60 : 26)});
        canvas.fill_round_rect(image.x + 11, image.y - 6, image.w - 22, 16, radius / 2,
                               Rgba{255, 255, 255, static_cast<uint8_t>(focused ? 100 : 48)});
    }
    if (cell.thumb != nullptr && !cell.thumb->empty()) {
        canvas.blit_cover(image.x, image.y, image.w, image.h, *cell.thumb, radius);
    } else if (cell.icon != GridCell::Icon::None) {
        // Opaque, so a folder's stacked plates do not show through.
        canvas.fill_round_rect(image.x, image.y, image.w, image.h, radius,
                               focused ? Rgba{60, 76, 120, 255} : Rgba{30, 42, 78, 255});
        draw_icon(canvas, cell.icon, image, theme, focused);
    } else {
        draw_placeholder(canvas, image, radius);
    }
    if (!focused || cell.dim) {
        // Unfocused tiles recede a little; a dim one (unsupported) much more.
        canvas.fill_round_rect(image.x, image.y, image.w, image.h, radius,
                               Rgba{0, 0, 0, static_cast<uint8_t>(cell.dim ? 150 : 46)});
    }

    if (focused) draw_focus_ring(canvas, image, radius, theme);

    if (cell.progress >= 0.0f) {
        canvas.fill_round_rect(image.x, image.y, image.w, image.h, radius, Rgba{2, 6, 18, 150});
        const float cx = static_cast<float>(image.x) + static_cast<float>(image.w) * 0.5f;
        const float ring = static_cast<float>(std::min(image.w, image.h)) * 0.17f;
        const float cy = static_cast<float>(image.y) + static_cast<float>(image.h) * 0.5f -
                         (cell.badge.empty() ? 0.0f : ring * 0.45f);
        canvas.stroke_circle(cx, cy, ring, 4.0f, Rgba{255, 255, 255, 50});
        canvas.stroke_arc(cx, cy, ring, 4.0f, cell.progress, theme.accent);
        const int size = theme.small_size;
        // A dark disc inside the ring keeps the percentage readable on a
        // bright photo.
        canvas.fill_circle(cx, cy, ring - 3.0f, Rgba{4, 8, 20, 200});
        if (cell.progress > 0.0f) {
            char percent[16];
            std::snprintf(percent, sizeof(percent), "%d%%",
                          static_cast<int>(std::lround(std::min(cell.progress, 1.0f) * 100.0f)));
            const int width = static_cast<int>(UiCanvas::text_width(percent, size));
            canvas.text(static_cast<int>(cx) - width / 2,
                        UiCanvas::text_top_centered(static_cast<int>(cy) - 10, 20, size), percent,
                        size, theme.text);
        }
        if (!cell.badge.empty()) {
            const int badge_w = static_cast<int>(UiCanvas::text_width(cell.badge, size));
            const int badge_y = static_cast<int>(cy + ring) + 8;
            canvas.fill_round_rect(static_cast<int>(cx) - badge_w / 2 - 10, badge_y - 2,
                                   badge_w + 20, UiCanvas::line_height(size) + 4, 12,
                                   Rgba{4, 8, 20, 200});
            canvas.text(static_cast<int>(cx) - badge_w / 2, badge_y, cell.badge, size, theme.text);
        }
    }

    if (cell.checked) {
        canvas.stroke_round_rect(image.x, image.y, image.w, image.h, radius, 3, theme.accent);
        const float r = 15.0f;
        const float cx = static_cast<float>(image.right()) - r - 8.0f;
        const float cy = static_cast<float>(image.y) + r + 8.0f;
        canvas.fill_circle(cx, cy, r, theme.accent);
        canvas.line(cx - r * 0.42f, cy + r * 0.02f, cx - r * 0.1f, cy + r * 0.34f, 3.0f,
                    theme.text);
        canvas.line(cx - r * 0.1f, cy + r * 0.34f, cx + r * 0.44f, cy - r * 0.3f, 3.0f,
                    theme.text);
    }

    if (caption_lines <= 0) return;
    const int caption_size = theme.body_size - 2;
    int y = box.y + image_rect(box, theme).h + 10;
    const Rgba caption_color =
        cell.dim ? theme.faint_text : (focused ? theme.text : theme.dim_text);
    // The focused tile shows its whole name on up to `caption_lines` lines
    // inside the label area; the others keep one line so the grid stays calm.
    const std::vector<std::string> lines =
        focused ? wrap_lines(cell.caption, caption_size, box.w - 4, caption_lines)
                : std::vector<std::string>{fit_text(cell.caption, caption_size, box.w - 4)};
    for (const std::string& line : lines) {
        canvas.text(box.x + 2, y, line, caption_size, caption_color);
        y += UiCanvas::line_height(caption_size);
    }
    if (show_sublabel && !cell.sublabel.empty() && (focused || !sublabel_focus_only)) {
        canvas.text(box.x + 2, y, fit_text(cell.sublabel, theme.small_size, box.w - 4),
                    theme.small_size, focused ? theme.dim_text : theme.faint_text);
    }
}

void GridView::draw(UiCanvas& canvas, const Rect& area, const UiTheme& theme) const {
    const int per_row = columns(area);
    const int visible = visible_rows(area);
    // The focused cell is drawn last, so its halo lies over its neighbours.
    int focused_index = -1;
    for (int row = 0; row < visible; ++row) {
        for (int column = 0; column < per_row; ++column) {
            const int index = (scroll_row_ + row) * per_row + column;
            if (index >= static_cast<int>(cells_.size())) break;
            if (index == focus_) {
                focused_index = index;
                continue;
            }
            draw_cell(canvas, cells_[static_cast<std::size_t>(index)], cell_rect(index, area), false,
                      theme);
        }
    }
    if (focused_index >= 0) {
        draw_cell(canvas, cells_[static_cast<std::size_t>(focused_index)],
                  cell_rect(focused_index, area), true, theme);
    }

    // More rows below or above the visible ones: a small chevron at the edge.
    const int total_rows = (static_cast<int>(cells_.size()) + per_row - 1) / per_row;
    const float cx = static_cast<float>(area.x + area.w / 2);
    // 36 x 18 px chevrons, readable in the headset.
    if (scroll_row_ + visible < total_rows) {
        const float y = static_cast<float>(area.bottom()) + 2.0f;
        canvas.line(cx - 18.0f, y, cx, y + 16.0f, 4.0f, theme.dim_text);
        canvas.line(cx, y + 16.0f, cx + 18.0f, y, 4.0f, theme.dim_text);
    }
    if (scroll_row_ > 0) {
        const float y = static_cast<float>(area.y) - 4.0f;
        canvas.line(cx - 18.0f, y, cx, y - 16.0f, 4.0f, theme.dim_text);
        canvas.line(cx, y - 16.0f, cx + 18.0f, y, 4.0f, theme.dim_text);
    }
}

void PopupMenu::open(std::string title, std::vector<std::string> items) {
    title_ = std::move(title);
    items_ = std::move(items);
    std::vector<ListRow> rows;
    rows.reserve(items_.size());
    for (const std::string& item : items_) rows.push_back(ListRow{item, "", "", false});
    list_.set_rows(std::move(rows));
    list_.set_focus(0);
    open_ = !items_.empty();
}

Rect PopupMenu::card(const Rect& content, const UiTheme& theme) const {
    const int width = 460;
    const int header = title_.empty() ? 20 : 64;
    const int height = header + static_cast<int>(items_.size()) * theme.row_height + 20;
    return Rect{content.x + (content.w - width) / 2, content.y + (content.h - height) / 2, width,
                height};
}

Rect PopupMenu::list_area(const Rect& content, const UiTheme& theme) const {
    const Rect box = card(content, theme);
    const int header = title_.empty() ? 20 : 64;
    return Rect{box.x + 16, box.y + header, box.w - 32,
                static_cast<int>(items_.size()) * theme.row_height};
}

int PopupMenu::update(const UiInput& input, const Rect& content, const UiTheme& theme) {
    if (!open_) return -1;
    list_.update(input, list_area(content, theme), theme);
    if (!list_.activated()) return -1;
    list_.clear_activated();
    open_ = false;
    return list_.focus();
}

void PopupMenu::draw(UiCanvas& canvas, const Rect& content, const UiTheme& theme) const {
    if (!open_) return;
    canvas.fill_rect(0, 0, static_cast<int>(canvas.width()), static_cast<int>(canvas.height()),
                     theme.veil);
    const Rect box = card(content, theme);
    draw_card(canvas, box, theme);
    if (!title_.empty()) {
        canvas.text(box.x + 40, box.y + 22, fit_text(title_, theme.small_size, box.w - 80),
                    theme.small_size, theme.dim_text);
    }
    list_.draw(canvas, list_area(content, theme), theme);
}

void ConfirmCard::open(std::string title, std::string detail, std::string action, bool allowed) {
    title_ = std::move(title);
    detail_ = std::move(detail);
    action_ = std::move(action);
    allowed_ = allowed;
    focus_ = 0;  // Cancel first, as on the console
    open_ = true;
}

Rect ConfirmCard::card_rect(const Rect& content) {
    const int width = 640;
    const int height = 240;
    return Rect{content.x + (content.w - width) / 2, content.y + (content.h - height) / 2, width,
                height};
}

Rect ConfirmCard::button_rect(const Rect& content, int index) {
    const Rect card = card_rect(content);
    const int gap = 16;
    const int w = (card.w - 80 - gap) / 2;
    return Rect{card.x + 40 + index * (w + gap), card.bottom() - 36 - 56, w, 56};
}

ConfirmCard::Choice ConfirmCard::update(const UiInput& input, const Rect& content) {
    if (!open_) return Choice::None;
    if ((input.pressed & Left) != 0) focus_ = 0;
    if ((input.pressed & Right) != 0 && allowed_) focus_ = 1;
    int pressed = -1;
    if (input.pointer_valid) {
        for (int index = 0; index < 2; ++index) {
            if (!button_rect(content, index).contains(input.px, input.py)) continue;
            if (index == 1 && !allowed_) continue;
            focus_ = index;
            if (input.click) pressed = index;
        }
    }
    if ((input.pressed & Select) != 0) pressed = focus_;
    if (pressed < 0) return Choice::None;
    open_ = false;
    return pressed == 1 ? Choice::Confirm : Choice::Cancel;
}

void ConfirmCard::draw(UiCanvas& canvas, const Rect& content, const UiTheme& theme) const {
    if (!open_) return;
    const Rect card = card_rect(content);
    draw_card(canvas, card, theme);
    int y = card.y + 32;
    canvas.text(card.x + 40, y, fit_text(title_, theme.heading_size, card.w - 80),
                theme.heading_size, theme.text);
    y += UiCanvas::line_height(theme.heading_size) + 4;
    canvas.text(card.x + 40, y, fit_text(detail_, theme.body_size - 2, card.w - 80),
                theme.body_size - 2, allowed_ ? theme.dim_text : theme.warning);

    const int size = theme.body_size;
    for (int index = 0; index < 2; ++index) {
        const Rect button = button_rect(content, index);
        const bool focused = index == focus_;
        const bool action = index == 1;
        Rgba fill = focused ? theme.light : theme.panel;
        Rgba ink = focused ? theme.background_bottom : theme.text;
        if (action && allowed_) {
            // White on a deep red (5:1): the destructive button is unmistakable.
            fill = Rgba{200, 56, 48, 255};
            ink = theme.text;
        } else if (action) {
            ink = theme.faint_text;
        }
        if (focused) draw_focus_ring(canvas, button, 12, theme);
        canvas.fill_round_rect(button.x, button.y, button.w, button.h, 12, fill);
        const std::string label = action ? action_ : std::string("Cancel");
        const int width = static_cast<int>(UiCanvas::text_width(label, size));
        canvas.text(button.x + (button.w - width) / 2,
                    UiCanvas::text_top_centered(button.y, button.h, size), label, size, ink);
    }
}

}  // namespace splat
