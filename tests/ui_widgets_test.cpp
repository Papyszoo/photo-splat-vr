// ListView, GridView and PopupMenu without a screen: focus rules (no wrap),
// paging, pointer hover/click, scroll-into-view, the popup's choice and close,
// and the drawing of rows, cells and their states.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ui/fakes.h"
#include "ui/text_fit.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace {

splat::UiInput press(uint32_t action) {
    splat::UiInput input;
    input.pressed = action;
    return input;
}

splat::UiInput pointer(float x, float y, bool click) {
    splat::UiInput input;
    input.pointer_valid = true;
    input.px = x;
    input.py = y;
    input.click = click;
    return input;
}

std::vector<splat::ListRow> rows(int count) {
    std::vector<splat::ListRow> out;
    for (int index = 0; index < count; ++index) {
        out.push_back(splat::ListRow{"row " + std::to_string(index), "", std::to_string(index), false});
    }
    return out;
}

}  // namespace

TEST_CASE("ui_widgets list focus never wraps and scrolls into view") {
    const splat::UiTheme theme = splat::default_theme();
    // Four rows of the theme's height: four visible rows.
    const splat::Rect area{0, 0, 400, 4 * theme.row_height};

    splat::ListView list;
    list.set_rows(rows(10));
    CHECK(list.focus() == 0);
    CHECK(list.scroll() == 0);

    list.update(press(splat::Up), area, theme);
    CHECK(list.focus() == 0);  // no wrap from the first row

    list.update(press(splat::Down), area, theme);
    list.update(press(splat::Down), area, theme);
    list.update(press(splat::Down), area, theme);
    CHECK(list.focus() == 3);
    CHECK(list.scroll() == 0);

    list.update(press(splat::Down), area, theme);
    CHECK(list.focus() == 4);
    CHECK(list.scroll() == 1);  // the focused row is the last visible one

    // Paging moves one page (four rows) and clamps at the ends.
    list.update(press(splat::PageNext), area, theme);
    CHECK(list.focus() == 8);
    list.update(press(splat::PageNext), area, theme);
    CHECK(list.focus() == 9);
    list.update(press(splat::PagePrev), area, theme);
    CHECK(list.focus() == 5);
    CHECK(list.scroll() == 5);
}

TEST_CASE("ui_widgets list pointer hovers and clicks") {
    const splat::UiTheme theme = splat::default_theme();
    const splat::Rect area{10, 20, 400, 4 * theme.row_height};  // four rows, first at y=20
    const float second_row = 20.0f + static_cast<float>(theme.row_height) + 1.0f;

    splat::ListView list;
    list.set_rows(rows(10));

    // Hover the second visible row: focus follows, no activation.
    list.update(pointer(30.0f, second_row, false), area, theme);
    CHECK(list.focus() == 1);
    CHECK_FALSE(list.activated());

    // A click activates that row.
    list.update(pointer(30.0f, second_row, true), area, theme);
    CHECK(list.activated());
    list.clear_activated();
    CHECK_FALSE(list.activated());

    // Outside the area nothing moves or activates.
    list.update(pointer(5.0f, 5.0f, true), area, theme);
    CHECK(list.focus() == 1);
    CHECK_FALSE(list.activated());

    // Cross activates the focused row.
    list.update(press(splat::Select), area, theme);
    CHECK(list.activated());
}

TEST_CASE("ui_widgets list rows clamp when the data shrinks") {
    splat::ListView list;
    list.set_rows(rows(6));
    list.set_focus(5);
    list.set_rows(rows(2));
    CHECK(list.focus() == 1);
    list.set_rows({});
    CHECK(list.focus() == 0);
    CHECK(list.scroll() == 0);
}

TEST_CASE("ui_widgets grid moves focus in two dimensions") {
    const splat::UiTheme theme = splat::default_theme();
    // 192 px cells with a 16 px gap: (400 + 16) / 208 = 2 columns.
    const splat::Rect area{0, 0, 400, 340};

    splat::GridView grid;
    std::vector<splat::GridCell> cells(10);
    for (int index = 0; index < 10; ++index) {
        cells[static_cast<std::size_t>(index)].caption = "cell " + std::to_string(index);
    }
    grid.set_cells(std::move(cells));
    CHECK(grid.columns(area) == 2);
    CHECK(grid.visible_rows(area) == 2);

    grid.update(press(splat::Right), area, theme);
    CHECK(grid.focus() == 1);
    grid.update(press(splat::Right), area, theme);
    CHECK(grid.focus() == 1);  // no wrap into the next row

    grid.update(press(splat::Down), area, theme);
    CHECK(grid.focus() == 3);
    grid.update(press(splat::Left), area, theme);
    CHECK(grid.focus() == 2);
    grid.update(press(splat::Left), area, theme);
    CHECK(grid.focus() == 2);  // first column, no wrap

    grid.update(press(splat::Up), area, theme);
    CHECK(grid.focus() == 0);
    grid.update(press(splat::Up), area, theme);
    CHECK(grid.focus() == 0);

    // Down at the bottom of a full last row keeps the column (no jump).
    grid.set_focus(8);
    grid.update(press(splat::Down), area, theme);
    CHECK(grid.focus() == 8);
    grid.update(press(splat::Up), area, theme);
    CHECK(grid.focus() == 6);
}

TEST_CASE("ui_widgets grid vertical focus keeps its column at the edges") {
    const splat::UiTheme theme = splat::default_theme();
    const splat::Rect area{0, 0, 400, 340};  // 192 px cells: two columns

    splat::GridView grid;
    grid.set_cells(std::vector<splat::GridCell>(5));  // rows: 0-1, 2-3, 4
    CHECK(grid.columns(area) == 2);

    // Up on the top row must not slide to the first column.
    grid.set_focus(1);
    grid.update(press(splat::Up), area, theme);
    CHECK(grid.focus() == 1);

    // Down on the last (partial) row must not slide to the last cell.
    grid.set_focus(4);
    grid.update(press(splat::Down), area, theme);
    CHECK(grid.focus() == 4);

    // Down from a column the partial row has moves to that item.
    grid.set_focus(2);
    grid.update(press(splat::Down), area, theme);
    CHECK(grid.focus() == 4);
    grid.update(press(splat::Up), area, theme);
    CHECK(grid.focus() == 2);

    // Down from a column the partial row lacks stays put.
    grid.set_focus(3);
    grid.update(press(splat::Down), area, theme);
    CHECK(grid.focus() == 3);
}

TEST_CASE("ui_widgets grid scrolls the focused row into view") {
    const splat::UiTheme theme = splat::default_theme();
    const splat::Rect area{0, 0, 400, 340};  // two visible rows

    splat::GridView grid;
    grid.set_cells(std::vector<splat::GridCell>(10));
    CHECK(grid.scroll_row() == 0);

    grid.set_focus(8);  // row 4
    grid.update(splat::UiInput{}, area, theme);
    CHECK(grid.scroll_row() == 3);  // rows 3 and 4 visible

    grid.set_focus(0);
    grid.update(splat::UiInput{}, area, theme);
    CHECK(grid.scroll_row() == 0);

    // The cell rect follows the scroll offset.
    grid.set_focus(8);
    grid.update(splat::UiInput{}, area, theme);
    const splat::Rect cell = grid.cell_rect(8, area);
    CHECK(cell.y == area.y + (4 - 3) * (grid.cell_height + grid.gap));
}

TEST_CASE("ui_widgets grid pointer selects a cell") {
    const splat::UiTheme theme = splat::default_theme();
    const splat::Rect area{10, 10, 400, 340};

    splat::GridView grid;
    grid.set_cells(std::vector<splat::GridCell>(6));

    // Second column, first row.
    grid.update(pointer(10.0f + 192.0f + 16.0f + 4.0f, 10.0f + 4.0f, false), area, theme);
    CHECK(grid.focus() == 1);
    CHECK_FALSE(grid.activated());

    grid.update(pointer(10.0f + 192.0f + 16.0f + 4.0f, 10.0f + 4.0f, true), area, theme);
    CHECK(grid.activated());
    grid.clear_activated();

    // The gap between cells belongs to no cell.
    grid.update(pointer(10.0f + 192.0f + 4.0f, 10.0f + 4.0f, true), area, theme);
    CHECK(grid.focus() == 1);
    CHECK_FALSE(grid.activated());
}

TEST_CASE("ui_widgets list right text updates in place") {
    splat::ListView list;
    list.set_rows(rows(3));
    list.set_focus(2);

    list.set_right(1, "2 running");
    CHECK(list.rows()[1].right == "2 running");
    CHECK(list.focus() == 2);  // the update leaves focus alone

    list.set_right(1, "");
    CHECK(list.rows()[1].right.empty());

    // Out-of-range indices are ignored.
    list.set_right(-1, "x");
    list.set_right(3, "x");
    CHECK(list.rows()[0].right == "0");
    CHECK(list.rows()[2].right == "2");
}

TEST_CASE("ui_widgets draw rows and cells onto the canvas") {
    const splat::UiTheme theme = splat::default_theme();
    splat::UiCanvas canvas(400, 200);
    canvas.clear(theme.background);

    splat::ListView list;
    list.set_rows({splat::ListRow{"A", "sub", "right", false}, splat::ListRow{"B", "", "", true}});
    list.draw(canvas, splat::Rect{0, 0, 400, 200}, theme);

    // The focused plate and marker changed pixels away from the background.
    int changed = 0;
    for (std::size_t index = 3; index < canvas.pixels().size(); index += 4) {
        if (canvas.pixels()[index] != theme.background.a ||
            canvas.pixels()[index - 3] != theme.background.r) {
            ++changed;
        }
    }
    CHECK(changed > 0);

    splat::UiCanvas grid_canvas(400, 200);
    grid_canvas.clear(theme.background);
    splat::UiImage thumb;
    thumb.resize(2, 2);
    for (std::size_t index = 0; index < thumb.rgba.size(); index += 4) {
        thumb.rgba[index] = 255;
        thumb.rgba[index + 3] = 255;
    }
    splat::GridView grid;
    grid.set_cells({splat::GridCell{"thumb", "", &thumb, false}});
    grid.draw(grid_canvas, splat::Rect{0, 0, 400, 200}, theme);
    CHECK(grid_canvas.dirty());

    // Drawing an empty widget is a no-op that does not crash.
    splat::ListView empty;
    empty.draw(canvas, splat::Rect{0, 0, 400, 200}, theme);
}

TEST_CASE("ui_widgets grid draws every tile state") {
    const splat::UiTheme theme = splat::default_theme();
    splat::UiImage thumb;
    thumb.resize(8, 6);
    for (std::size_t index = 0; index < thumb.rgba.size(); index += 4) {
        thumb.rgba[index] = 200;
        thumb.rgba[index + 3] = 255;
    }
    std::vector<splat::GridCell> cells(5);
    cells[0].caption = "Add photos";
    cells[0].icon = splat::GridCell::Icon::Add;
    cells[1].caption = "folder";
    cells[1].icon = splat::GridCell::Icon::Folder;
    cells[1].stack = true;
    cells[2].caption = "photo";
    cells[2].thumb = &thumb;
    cells[2].checked = true;
    cells[3].caption = "converting";
    cells[3].thumb = &thumb;
    cells[3].progress = 0.4f;
    cells[3].badge = "Converting";
    cells[4].caption = "no thumbnail";  // the labelled placeholder

    splat::GridView grid;
    grid.set_cells(cells);
    const splat::Rect area{20, 20, 1000, 300};
    // Every tile differs from the others and from the empty backdrop.
    std::vector<std::uint64_t> sums;
    for (int focus = 0; focus < 5; ++focus) {
        splat::UiCanvas canvas(1040, 340);
        canvas.clear(theme.background);
        grid.set_focus(focus);
        grid.draw(canvas, area, theme);
        std::uint64_t sum = 0;
        for (const uint8_t byte : canvas.pixels()) sum = sum * 131 + byte;
        sums.push_back(sum);
    }
    for (std::size_t a = 0; a < sums.size(); ++a) {
        for (std::size_t b = a + 1; b < sums.size(); ++b) CHECK(sums[a] != sums[b]);
    }

    // In-place access changes a cell without moving the focus or the scroll.
    grid.set_focus(3);
    REQUIRE(grid.cell(3) != nullptr);
    grid.cell(3)->progress = 0.9f;
    CHECK(grid.cells()[3].progress == doctest::Approx(0.9f));
    CHECK(grid.focus() == 3);
    CHECK(grid.cell(5) == nullptr);
    CHECK(grid.cell(-1) == nullptr);
}

TEST_CASE("ui_widgets popup menu chooses and closes") {
    const splat::UiTheme theme = splat::default_theme();
    const splat::Rect content{64, 112, 1152, 532};
    splat::PopupMenu menu;
    CHECK_FALSE(menu.is_open());
    CHECK(menu.update(press(splat::Select), content, theme) == -1);

    menu.open("trip", {"Rename", "Delete", "New folder"});
    REQUIRE(menu.is_open());
    CHECK(menu.title() == "trip");
    CHECK(menu.update(press(splat::Down), content, theme) == -1);
    CHECK(menu.focus() == 1);
    CHECK(menu.update(press(splat::Select), content, theme) == 1);
    CHECK_FALSE(menu.is_open());  // a choice closes it

    // Reopening starts at the top again; close() is what Circle does.
    menu.open("trip", {"Rename", "New folder"});
    CHECK(menu.focus() == 0);
    menu.close();
    CHECK_FALSE(menu.is_open());

    // An empty menu never opens; drawing a closed or open one is safe.
    menu.open("x", {});
    CHECK_FALSE(menu.is_open());
    splat::UiCanvas canvas(1280, 720);
    canvas.clear(theme.background);
    menu.draw(canvas, content, theme);
    menu.open("x", {"One"});
    menu.draw(canvas, content, theme);
    CHECK(canvas.dirty());
}

TEST_CASE("ui_widgets wrap_lines keeps long names on two lines") {
    constexpr int kSize = 20;
    const int width = 200;
    const std::string name = "apple-trees-in-blossom-behind-the-wooden-shed";
    const std::vector<std::string> lines = splat::wrap_lines(name, kSize, width, 2);
    REQUIRE(lines.size() == 2);
    for (const std::string& line : lines) {
        CHECK(static_cast<int>(splat::UiCanvas::text_width(line, kSize)) <= width);
    }
    // The first line breaks after a dash, the second ends in an ellipsis.
    CHECK(lines[0].back() == '-');
    CHECK(lines[1].find("\xE2\x80\xA6") != std::string::npos);
    // A short name stays one line, untouched.
    CHECK(splat::wrap_lines("garden", kSize, width, 2) == std::vector<std::string>{"garden"});
}

TEST_CASE("ui_widgets conversion chip shows only while converting") {
    const splat::UiTheme theme = splat::default_theme();
    splat::FakeJobService jobs;
    CHECK(splat::conversion_status(nullptr).active == 0);
    CHECK(splat::conversion_status(&jobs).active == 0);
    CHECK(splat::conversion_chip_width(splat::conversion_status(&jobs), theme) == 0);

    REQUIRE(jobs.enqueue({splat::Path("a.jpg"), splat::Path("b.jpg")}, "sharp", "").ok());
    jobs.tick(1.0);
    const splat::ConversionStatus status = splat::conversion_status(&jobs);
    CHECK(status.active == 2);
    CHECK(status.queued == 1);
    CHECK(status.fraction == doctest::Approx(0.25f));
    // One clear form: the running photo, and how many wait behind it.
    CHECK(splat::conversion_label(status) == "Converting \xC2\xB7 1 queued");
    splat::ConversionStatus single;
    single.active = 1;
    CHECK(splat::conversion_label(single) == "Converting");
    splat::UiCanvas canvas(400, 60);
    canvas.clear(theme.background);
    const int width = splat::draw_conversion_chip(canvas, 380, 30, status, theme);
    CHECK(width == splat::conversion_chip_width(status, theme));
    CHECK(width > 100);
}

TEST_CASE("ui_widgets confirm card starts on Cancel") {
    const splat::UiTheme theme = splat::default_theme();
    const splat::Rect content{64, 112, 1152, 532};
    splat::ConfirmCard card;
    CHECK_FALSE(card.is_open());
    CHECK(card.update(press(splat::Select), content) == splat::ConfirmCard::Choice::None);

    card.open("Delete “x”?", "It goes away.", "Delete");
    REQUIRE(card.is_open());
    CHECK(card.focus() == 0);
    // Cross on the focused Cancel cancels.
    CHECK(card.update(press(splat::Select), content) == splat::ConfirmCard::Choice::Cancel);
    CHECK_FALSE(card.is_open());

    card.open("Delete “x”?", "It goes away.", "Delete");
    CHECK(card.update(press(splat::Right), content) == splat::ConfirmCard::Choice::None);
    CHECK(card.focus() == 1);
    CHECK(card.update(press(splat::Left), content) == splat::ConfirmCard::Choice::None);
    CHECK(card.focus() == 0);
    CHECK(card.update(press(splat::Right), content) == splat::ConfirmCard::Choice::None);
    CHECK(card.update(press(splat::Select), content) == splat::ConfirmCard::Choice::Confirm);

    // A click on a button presses it.
    card.open("Delete “x”?", "It goes away.", "Delete");
    const splat::Rect action = splat::ConfirmCard::button_rect(content, 1);
    CHECK(card.update(pointer(static_cast<float>(action.x + 10), static_cast<float>(action.y + 10),
                              true),
                      content) == splat::ConfirmCard::Choice::Confirm);

    // A refused action keeps the focus on Cancel and cannot be clicked.
    card.open("Delete “x”?", "It holds scenes.", "Delete", false);
    CHECK(card.update(press(splat::Right), content) == splat::ConfirmCard::Choice::None);
    CHECK(card.focus() == 0);
    CHECK(card.update(pointer(static_cast<float>(action.x + 10), static_cast<float>(action.y + 10),
                              true),
                      content) == splat::ConfirmCard::Choice::None);
    CHECK(card.is_open());

    splat::UiCanvas canvas(1280, 720);
    canvas.clear(theme.background);
    card.draw(canvas, content, theme);
    CHECK(canvas.dirty());
}

