// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#include "ui/pointer_mode.h"
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

namespace {

// The five steps as column counts: the smallest step shows the most entries,
// the largest the fewest. A tile is the width the panel leaves for one column
// after the one shared gap, so at every size and on every screen the row fills
// the panel's width with the same gap. Stage TF scaled a 176 px tile by a
// percentage and spread the spare width over the column gaps; at the largest
// step that gave the folder grid four 220 px tiles with 90 px gaps while the
// photo picker (whose shorter area needs a fifth column for its six entries)
// showed 19 px gaps. Column counts keep the two screens identical at every step.
constexpr int kThumbnailColumns[kThumbnailSizeCount] = {9, 8, 7, 6, 5};
// The space between two columns (and between two rows) in design pixels: 18 px
// on the 1280x720 canvas, 13 px on the VR panel's 0.7 theme.
constexpr int kThumbnailGap = 18;
// One picture shape for every tile (stage TF): the photo screen's default
// picture, 176x102 design pixels (about 1.73:1, what the review measured on the
// default photo screen). A tile's picture is always `width * 102 / 176` tall and
// only the label block is added on top, so every size and every screen shows
// the same crop of a photo instead of a strip that gets flatter as the tile
// grows.
constexpr int kThumbnailPictureWidth = 176;
constexpr int kThumbnailPictureHeight = 102;
// The fewest entries a step must show when the area is small. The default and
// the small step keep 12, the large 8 and the largest 6; the smallest only has
// to stay legible (0 = no count requirement).
constexpr int kThumbnailMinEntries[kThumbnailSizeCount] = {0, 12, 12, 8, 6};
// The legibility floor a shrink may not cross (design pixels): a tile still
// leaves room for a picture and a caption at this size.
constexpr int kThumbnailFloorWidth = 110;

// The one columns/rows formula. thumbnail_metrics() below and GridView's own
// columns()/visible_rows() both call these, so a size's entry-count guarantee
// is computed with the layout the grid really draws.
int grid_columns(int width, int cell_width, int gap) {
    const int stride = std::max(1, cell_width + gap);
    return std::max(1, (width + gap) / stride);
}

int grid_rows(int height, int cell_height, int gap) {
    const int stride = std::max(1, cell_height + gap);
    return std::max(1, (height + gap) / stride);
}

// The picture of a tile `width` px wide at the one shared shape.
int picture_height(int width) {
    return std::max(1, static_cast<int>(std::lround(static_cast<double>(width) *
                                                    kThumbnailPictureHeight /
                                                    kThumbnailPictureWidth)));
}

// The scroll chevrons: 36 x 16 px with 4 px strokes at the design size. They
// sit in the panel's right gutter (the empty margin beside the grid), never in
// the area itself: at the eight-column step the photo picker has only 4 px of
// bottom spare, so the "more below" chevron covered the last row's second
// caption line when it was drawn inside the grid (stage TF2 fix).
constexpr int kChevronHalfWidth = 18;
constexpr int kChevronHeight = 16;
constexpr int kChevronInset = 8;
constexpr int kChevronStroke = 4;
// A little past the gutter's middle: a focused tile in the last column grows
// and draws its ring 4 px out, so the chevron keeps clear of that too.
constexpr int kChevronGutterShift = 4;

}  // namespace

int thumbnail_label_height(const UiTheme& theme, int caption_lines, bool sublabel) {
    if (caption_lines <= 0) return 0;
    // The gap between the picture and its caption is a design constant too, so
    // the whole block scales with the panel.
    int height = theme.scaled(10) + caption_lines * UiCanvas::line_height(theme.body_size - 2);
    if (sublabel) height += UiCanvas::line_height(theme.small_size);
    return height;
}

ThumbnailMetrics thumbnail_metrics(const Rect& area, const UiTheme& theme, int size,
                                   ThumbnailLabel label) {
    const int index = clamp_thumbnail_size(size);
    const int label_height =
        thumbnail_label_height(theme, label.caption_lines, label.sublabel);
    // The count guarantee is computed with the tallest label block a thumbnail
    // screen can draw, not with this screen's: the photo picker reserves its
    // sublabel line only when the folder lists a splat file, and a step must not
    // change its column count when such a file appears. Only the guard uses the
    // taller block; the cell keeps the label the screen really draws, so the
    // picture box stays the one shape.
    const int guard_label_height = thumbnail_label_height(theme, label.caption_lines, true);
    const int floor_width = std::max(1, theme.scaled(kThumbnailFloorWidth));
    ThumbnailMetrics metrics;
    metrics.gap = std::max(2, theme.scaled(kThumbnailGap));
    // The step's share of the panel: the column count is fixed and the gap is
    // shared, so the tile takes the rest of the width. The same numbers come out
    // on the folder grids and on the photo picker, whatever their height.
    const int columns = kThumbnailColumns[index];
    const int inner = area.w - metrics.gap * (columns - 1);
    metrics.cell_width = std::max(floor_width, inner / std::max(1, columns));
    metrics.cell_height = picture_height(metrics.cell_width) + label_height;

    // A panel too small for the step's guaranteed entry count gets smaller
    // tiles, never fewer entries: the 2-entries-in-VR case cannot come back by
    // running a different canvas. The tile shrinks on the one shared shape, so
    // the picture stays the same crop; past the legibility floor only the gap
    // shrinks (to its 2 px minimum), which is what lets a small panel keep the
    // count with the smallest allowed tiles.
    const int required = kThumbnailMinEntries[index];
    while (required > 0 &&
           grid_columns(area.w, metrics.cell_width, metrics.gap) *
                   grid_rows(area.h, picture_height(metrics.cell_width) + guard_label_height,
                             metrics.gap) <
               required) {
        if (metrics.cell_width <= floor_width && metrics.gap <= 2) break;
        if (metrics.cell_width > floor_width) {
            metrics.cell_width = std::max(
                floor_width, metrics.cell_width - std::max(1, metrics.cell_width / 40));
        }
        metrics.gap = std::max(2, metrics.gap - 1);
        metrics.cell_height = picture_height(metrics.cell_width) + label_height;
    }

    // The grid uses the panel's whole width (stage TF): the tiles keep the
    // step's width and the rounding's spare pixels are spread over the spaces
    // between columns, so the right margin is never wider than one gap. Only
    // the space between columns moves, so the number of rows (and the entry
    // count) does not.
    const int drawn = grid_columns(area.w, metrics.cell_width, metrics.gap);
    metrics.column_gap = metrics.gap;
    if (drawn > 1) {
        const int spare = area.w - drawn * metrics.cell_width;
        if (spare > 0) metrics.column_gap = std::max(metrics.gap, spare / (drawn - 1));
    }
    return metrics;
}

void apply_thumbnail_layout(GridView& grid, const Rect& area, const UiTheme& theme, int size) {
    // One label rule for every tile, whatever the screen: two caption lines,
    // reserved whether or not the tile is focused.
    grid.caption_lines = kThumbnailCaptionLines;
    const ThumbnailMetrics metrics =
        thumbnail_metrics(area, theme, size, ThumbnailLabel{grid.caption_lines, grid.show_sublabel});
    grid.cell_width = metrics.cell_width;
    grid.cell_height = metrics.cell_height;
    grid.gap = metrics.gap;
    grid.column_gap = metrics.column_gap;
}

int thumbnail_columns(const Rect& area, const UiTheme& theme, int size, ThumbnailLabel label) {
    const ThumbnailMetrics metrics = thumbnail_metrics(area, theme, size, label);
    return grid_columns(area.w, metrics.cell_width, metrics.column_gap);
}

int thumbnail_entries_visible(const Rect& area, const UiTheme& theme, int size,
                              ThumbnailLabel label) {
    const ThumbnailMetrics metrics = thumbnail_metrics(area, theme, size, label);
    return grid_columns(area.w, metrics.cell_width, metrics.column_gap) *
           grid_rows(area.h, metrics.cell_height, metrics.gap);
}

Slider::Slider(int steps, int value) : steps_(std::max(1, steps)), value_(0) {
    set_value(value);
}

bool Slider::set_value(int value) {
    const int clamped = std::max(0, std::min(value, steps_ - 1));
    if (clamped == value_) return false;
    value_ = clamped;
    return true;
}

Rect Slider::track_rect(const Rect& area, int text_width, const UiTheme& theme) {
    const int knob = std::max(4, theme.scaled(9));
    const int right_gap = std::max(2, theme.scaled(16));
    const int wanted = area.w - std::max(0, text_width) - right_gap;
    const int height = std::max(4, theme.scaled(6));
    const int min_width = std::max(knob * 2, theme.scaled(90));
    const int width = std::max(min_width, wanted);
    return Rect{area.x, area.y + (area.h - height) / 2, width, height};
}

Rect Slider::hit_rect(const Rect& area, int text_width, const UiTheme& theme) {
    // The track's x range, widened by the knob's radius so a drag just past an
    // end still clamps there, over the row's whole height (the track itself is
    // a few px tall and would be hard to hit with a laser). The value text is a
    // label, not a control: a press on it changes nothing, which is why this
    // box stops short of it instead of covering the whole slider_area.
    const Rect track = track_rect(area, text_width, theme);
    const int margin = std::max(4, theme.scaled(9));
    const int x = std::max(area.x, track.x - margin);
    const int right = std::max(x + 1, std::min(area.right(), track.right() + margin));
    return Rect{x, area.y, right - x, area.h};
}

int Slider::value_at(float px, const Rect& track) const {
    if (steps_ <= 1) return 0;
    const float t = (px - static_cast<float>(track.x)) / static_cast<float>(std::max(1, track.w));
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    return std::max(0, std::min(static_cast<int>(std::lround(clamped * (steps_ - 1))),
                                  steps_ - 1));
}

void Slider::draw(UiCanvas& canvas, const Rect& area, std::string_view value_text, int text_width,
                  bool focused, const UiTheme& theme) const {
    const int text_size = theme.small_size;
    const Rect track = track_rect(area, text_width, theme);
    const int radius = std::max(2, track.h / 2);
    const int knob = std::max(4, theme.scaled(9));

    canvas.fill_round_rect(track.x, track.y, track.w, track.h, radius,
                           focused ? Rgba{255, 255, 255, 54} : Rgba{255, 255, 255, 34});
    // A step tick per value, centred on the knob's travel.
    const int travel = std::max(1, track.w - 1);
    const int tick = std::max(2, theme.scaled(3));
    for (int step = 0; step < steps_; ++step) {
        const int x = track.x + travel * step / std::max(1, steps_ - 1);
        canvas.fill_round_rect(x - tick / 2, track.y - tick, tick, track.h + 2 * tick,
                               std::max(1, tick / 2), Rgba{255, 255, 255, 60});
    }
    const int knob_x = track.x + travel * value_ / std::max(1, steps_ - 1);
    if (knob_x > track.x) {
        canvas.fill_round_rect(track.x, track.y, knob_x - track.x, track.h, radius, theme.accent);
    }
    const float cy = static_cast<float>(track.y) + static_cast<float>(track.h) * 0.5f;
    canvas.fill_circle(static_cast<float>(knob_x), cy, static_cast<float>(knob),
                       focused ? theme.text : theme.light);
    if (focused) {
        canvas.stroke_circle(static_cast<float>(knob_x), cy, static_cast<float>(knob + 4), 2.0f,
                             theme.glow);
    }
    canvas.text(area.right() - text_width,
                UiCanvas::text_top_centered(area.y, area.h, text_size), value_text, text_size,
                focused ? theme.text : theme.dim_text);
}

void draw_row_progress(UiCanvas& canvas, const Rect& plate, std::string_view right,
                       float fraction, const UiTheme& theme) {
    const int plate_right = plate.right();
    const int text_w = static_cast<int>(UiCanvas::text_width(right, theme.small_size));
    const int w = std::max(140, text_w);
    const int x = plate_right - 24 - w;
    const int y = plate.y + plate.h / 2 + 14;
    canvas.fill_round_rect(x, y, w, 4, 2, Rgba{255, 255, 255, 40});
    const float clamped = std::clamp(std::isfinite(fraction) ? fraction : 0.0f, 0.0f, 1.0f);
    const int filled = static_cast<int>(static_cast<float>(w) * clamped + 0.5f);
    if (filled > 0) canvas.fill_round_rect(x, y, std::max(4, filled), 4, 2, theme.accent);
}

ConversionStatus conversion_status(const IJobService* jobs) {
    ConversionStatus status;
    if (jobs == nullptr) return status;
    for (const JobView& job : jobs->snapshot()) {
        // A scene turn is its own entry's busy mark, not a conversion: the
        // top-bar chip speaks about photos becoming scenes.
        if (job.kind == "rotate") continue;
        if (job.state == "running") status.fraction = job.fraction;
        if (job.state == "queued") ++status.queued;
        if (job.state == "waiting") ++status.waiting;
        if (job.state == "running" || job.state == "queued" || job.state == "waiting") {
            ++status.active;
        }
    }
    return status;
}

std::string conversion_label(const ConversionStatus& status) {
    // Nothing converting or queued, only scenes waiting for their share.
    if (status.waiting > 0 && status.active <= status.waiting) {
        return status.waiting == 1 ? std::string("Waiting")
                                   : "Waiting \xC2\xB7 " + std::to_string(status.waiting);
    }
    std::string label = "Converting";
    if (status.queued > 0) label += " \xC2\xB7 " + std::to_string(status.queued) + " queued";
    if (status.waiting > 0) label += " \xC2\xB7 " + std::to_string(status.waiting) + " waiting";
    return label;
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

Rect ListView::row_rect(int index, const Rect& area, const UiTheme& theme) const {
    const int row_height = std::max(1, theme.row_height);
    const int visible = visible_rows(area, theme);
    const int row = index - scroll_;
    if (index < 0 || index >= static_cast<int>(rows_.size()) || row < 0 || row >= visible) {
        return Rect{};
    }
    const int scrollbar = static_cast<int>(rows_.size()) > visible ? 12 : 0;
    return Rect{area.x, area.y + row * row_height + kRowInset, area.w - scrollbar,
                row_height - 2 * kRowInset};
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

    // U3: the stick owns a frame it moved in. A direction press (including the
    // repeats UiApp folds into `pressed`) means the pointer block is skipped, so
    // a ray that happens to sit over another row cannot undo the step - the
    // user's "because pointing fought with my selection I couldn't scroll with
    // a joystick". The state machine (ui/pointer_mode.h) already disarms the
    // pointer on the same frame; this is the widgets' own belt, and it reuses
    // the mode's one definition of the four focus directions.
    const bool direction_pressed = (input.pressed & PointerMode::kDirections) != 0;
    if (!direction_pressed && input.pointer_valid && area.contains(input.px, input.py)) {
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
        // The one plate geometry: row_rect() is also what a screen's pointer
        // hit test and its row overlay (Settings' slider) use.
        const Rect plate = row_rect(index, area, theme);
        if (plate.empty()) continue;

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
    return grid_columns(area.w, cell_width, column_gap);
}

int GridView::visible_rows(const Rect& area) const {
    return grid_rows(area.h, cell_height, gap);
}

Rect GridView::cell_rect(int index, const Rect& area) const {
    const int per_row = columns(area);
    const int column = index % per_row;
    const int row = index / per_row - scroll_row_;
    return Rect{area.x + column * (cell_width + column_gap),
                area.y + row * (cell_height + gap), cell_width, cell_height};
}

Rect GridView::image_rect(const Rect& cell, const UiTheme& theme) const {
    const int text_block = thumbnail_label_height(theme, caption_lines, show_sublabel);
    return Rect{cell.x, cell.y, cell.w, std::max(1, cell.h - text_block)};
}

Rect GridView::chevron_rect(const Rect& area, const UiTheme& theme, bool down) const {
    const int half = theme.scaled(kChevronHalfWidth);
    const int height = theme.scaled(kChevronHeight);
    const int inset = theme.scaled(kChevronInset);
    // The middle of the panel margin to the right of the grid, a little
    // further out than half way so a focused last-column tile's ring cannot
    // reach it.
    const int cx = area.right() + theme.margin / 2 + theme.scaled(kChevronGutterShift);
    const int y = down ? area.bottom() - inset - height : area.y + inset;
    return Rect{cx - half, y, 2 * half, height};
}

void GridView::draw_chevron(UiCanvas& canvas, const Rect& box, bool up,
                            const UiTheme& theme) const {
    const float cx = static_cast<float>(box.x) + static_cast<float>(box.w) * 0.5f;
    const float stroke = static_cast<float>(theme.scaled(kChevronStroke));
    const float tip = up ? static_cast<float>(box.y) : static_cast<float>(box.bottom() - 1);
    const float base = up ? static_cast<float>(box.bottom() - 1) : static_cast<float>(box.y);
    canvas.line(static_cast<float>(box.x), base, cx, tip, stroke, theme.dim_text);
    canvas.line(cx, tip, static_cast<float>(box.right() - 1), base, stroke, theme.dim_text);
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
    // One visible page: the page size follows the layout, so a larger tile
    // setting moves the focus fewer entries per press.
    if ((input.pressed & PagePrev) != 0) {
        focus_ = std::max(0, focus_ - per_row * visible);
    }
    if ((input.pressed & PageNext) != 0) {
        focus_ = std::min(count - 1, focus_ + per_row * visible);
    }

    // U3: a direction press owns the frame (the same belt as ListView::update):
    // the ray hovering another cell must not pull the focus back off the cell
    // the stick just moved to.
    const bool direction_pressed = (input.pressed & PointerMode::kDirections) != 0;
    if (!direction_pressed && input.pointer_valid && area.contains(input.px, input.py)) {
        const int column = static_cast<int>(input.px - static_cast<float>(area.x)) /
                           std::max(1, cell_width + column_gap);
        const int row = scroll_row_ + static_cast<int>(input.py - static_cast<float>(area.y)) /
                                          std::max(1, cell_height + gap);
        const int cell_x = area.x + column * (cell_width + column_gap);
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
        if (((cell.turn_quarters % 4) + 4) % 4 != 0) {
            // A turned photo is fitted, not cropped: the whole turned frame is
            // visible, on the tile's own backdrop.
            canvas.blit_turned_fit(image.x, image.y, image.w, image.h, cell.turn_quarters,
                                   *cell.thumb, radius, theme.background);
        } else if (cell.fit) {
            // A photo tile shows the whole picture (stage TF2): fitted inside
            // the tile, with the backdrop in the bars where the shape differs.
            canvas.blit_fit(image.x, image.y, image.w, image.h, *cell.thumb, radius,
                            theme.background);
        } else {
            canvas.blit_cover(image.x, image.y, image.w, image.h, *cell.thumb, radius);
        }
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
    }

    if (!cell.badge.empty()) {
        // Under the progress ring, or (with no ring: a splat file's SPLAT
        // badge) along the bottom of the tile.
        const int size = theme.small_size;
        const int badge_w = static_cast<int>(UiCanvas::text_width(cell.badge, size));
        int badge_y = image.bottom() - UiCanvas::line_height(size) - 14;
        if (cell.progress >= 0.0f) {
            const float ring = static_cast<float>(std::min(image.w, image.h)) * 0.17f;
            const float cy = static_cast<float>(image.y) + static_cast<float>(image.h) * 0.5f -
                             ring * 0.45f;
            badge_y = static_cast<int>(cy + ring) + 8;
        }
        const int badge_x = image.x + (image.w - badge_w) / 2;
        canvas.fill_round_rect(badge_x - 10, badge_y - 2, badge_w + 20,
                               UiCanvas::line_height(size) + 4, 12, Rgba{4, 8, 20, 200});
        canvas.text(badge_x, badge_y, cell.badge, size, theme.text);
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
    int y = box.y + image_rect(box, theme).h + theme.scaled(10);
    const Rgba caption_color =
        cell.dim ? theme.faint_text : (focused ? theme.text : theme.dim_text);
    // One label rule for every tile: up to `caption_lines` lines, focused or
    // not, and the last line ends in an ellipsis when the name does not fit.
    // The rule used to be "the focused tile wraps, the others keep one line",
    // which at the smallest size showed one name cut short next to a focused
    // one on two lines.
    const std::vector<std::string> lines =
        wrap_lines(cell.caption, caption_size, box.w - 4, caption_lines);
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

    // More rows below or above the visible ones: a chevron in the panel's right
    // gutter, level with the grid's top or bottom edge. It is outside the area
    // at every size, so it hides neither a tile nor a caption, and it keeps the
    // hint bar's ink 28 px away (stage TF2).
    const int total_rows = (static_cast<int>(cells_.size()) + per_row - 1) / per_row;
    if (scroll_row_ + visible < total_rows) {
        draw_chevron(canvas, chevron_rect(area, theme, true), false, theme);
    }
    if (scroll_row_ > 0) {
        draw_chevron(canvas, chevron_rect(area, theme, false), true, theme);
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
