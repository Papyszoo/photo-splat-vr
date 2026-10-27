// The panel canvas without a GPU: clipping, blending, the panel font (Inter
// through stb_truetype), the anti-aliased rounded shapes, scaled blits,
// progress bars, the dirty flag and the PNG path the preview tool and the
// console frame dump share.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "jobs/image_io.h"
#include "test_util.h"
#include "ui/canvas.h"
#include "ui/font.h"
#include "ui/theme.h"
#include "ui/ui_input.h"

namespace {

constexpr splat::Rgba kNone{0, 0, 0, 0};
constexpr splat::Rgba kRed{255, 0, 0, 255};
constexpr splat::Rgba kGreen{0, 255, 0, 255};
constexpr splat::Rgba kBlue{0, 0, 255, 255};
constexpr splat::Rgba kWhite{255, 255, 255, 255};
constexpr splat::Rgba kBlack{0, 0, 0, 255};

splat::Rgba pixel_at(const splat::UiCanvas& canvas, int x, int y) {
    const uint8_t* pixel =
        canvas.pixels().data() + (static_cast<size_t>(y) * canvas.width() + x) * 4;
    return splat::Rgba{pixel[0], pixel[1], pixel[2], pixel[3]};
}

int opaque_pixels(const splat::UiCanvas& canvas) {
    int count = 0;
    for (size_t i = 3; i < canvas.pixels().size(); i += 4) {
        if (canvas.pixels()[i] != 0) ++count;
    }
    return count;
}

splat::UiImage solid_image(uint32_t w, uint32_t h, splat::Rgba color) {
    splat::UiImage image;
    image.resize(w, h);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* pixel = image.pixel(x, y);
            pixel[0] = color.r;
            pixel[1] = color.g;
            pixel[2] = color.b;
            pixel[3] = color.a;
        }
    }
    return image;
}

void set_pixel(splat::UiImage& image, uint32_t x, uint32_t y, splat::Rgba color) {
    uint8_t* pixel = image.pixel(x, y);
    pixel[0] = color.r;
    pixel[1] = color.g;
    pixel[2] = color.b;
    pixel[3] = color.a;
}

}  // namespace

TEST_CASE("ui_canvas clips rects at all four edges") {
    constexpr int kSize = 16;
    // A 12x12 rect hanging over an edge covers the 4x4 corner and nothing else.
    const struct {
        int x;
        int y;
        int corner_x;
        int corner_y;
    } cases[4] = {{-8, -8, 0, 0}, {12, -8, 15, 0}, {-8, 12, 0, 15}, {12, 12, 15, 15}};

    for (const auto& edge : cases) {
        splat::UiCanvas canvas(kSize, kSize);
        canvas.clear(kNone);
        canvas.fill_rect(edge.x, edge.y, 12, 12, kRed);
        CHECK(pixel_at(canvas, edge.corner_x, edge.corner_y) == kRed);
        CHECK(opaque_pixels(canvas) == 16);
    }

    // Entirely outside: no pixels, no crash.
    splat::UiCanvas offscreen(kSize, kSize);
    offscreen.clear(kNone);
    offscreen.fill_rect(100, 100, 8, 8, kRed);
    offscreen.fill_rect(-8, 0, 8, 8, kRed);
    offscreen.fill_rect(0, -8, 8, 8, kRed);
    CHECK(opaque_pixels(offscreen) == 0);

    // Zero and negative extents draw nothing.
    offscreen.fill_rect(2, 2, 0, 4, kRed);
    offscreen.fill_rect(2, 2, 4, -3, kRed);
    CHECK(opaque_pixels(offscreen) == 0);
}

TEST_CASE("ui_canvas alpha blends in whole bytes") {
    splat::UiCanvas canvas(4, 1);
    canvas.clear(kWhite);

    // 128 of black over white: (0*128 + 255*127 + 127) / 255 = 127.
    canvas.fill_rect(0, 0, 1, 1, splat::Rgba{0, 0, 0, 128});
    CHECK(pixel_at(canvas, 0, 0).r == 127);
    CHECK(pixel_at(canvas, 0, 0).g == 127);
    CHECK(pixel_at(canvas, 0, 0).b == 127);
    CHECK(pixel_at(canvas, 0, 0).a == 255);

    // 64 of red over white keeps red at 255 and puts green/blue at 191.
    canvas.fill_rect(1, 0, 1, 1, splat::Rgba{255, 0, 0, 64});
    CHECK(pixel_at(canvas, 1, 0).r == 255);
    CHECK(pixel_at(canvas, 1, 0).g == 191);
    CHECK(pixel_at(canvas, 1, 0).b == 191);

    // Fully transparent leaves the pixel untouched; opaque replaces it.
    canvas.fill_rect(2, 0, 1, 1, splat::Rgba{0, 255, 0, 0});
    CHECK(pixel_at(canvas, 2, 0) == kWhite);
    canvas.fill_rect(3, 0, 1, 1, kBlue);
    CHECK(pixel_at(canvas, 3, 0) == kBlue);

    // A half-transparent draw over empty canvas records the alpha.
    splat::UiCanvas blank(1, 1);
    blank.fill_rect(0, 0, 1, 1, splat::Rgba{255, 0, 0, 128});
    CHECK(pixel_at(blank, 0, 0).a == 128);
    CHECK(pixel_at(blank, 0, 0).r == 255);
}

TEST_CASE("ui_canvas text width is the panel font's advance") {
    splat::UiCanvas canvas(64, 48);
    canvas.clear(kNone);

    CHECK(splat::UiCanvas::text_width("", 22) == 0);
    const uint32_t ab = splat::UiCanvas::text_width("AB", 22);
    const uint32_t abc = splat::UiCanvas::text_width("ABC", 22);
    CHECK(ab > 0);
    CHECK(abc > ab);
    // Twice the size is twice as wide, give or take the rounding.
    const int small = static_cast<int>(splat::UiCanvas::text_width("Hello", 22));
    const int large = static_cast<int>(splat::UiCanvas::text_width("Hello", 44));
    CHECK(std::abs(large - 2 * small) <= 2);
    // Space advances.
    CHECK(splat::UiCanvas::text_width("A B", 22) > ab);

    // text() returns exactly the measured width, also when it is clipped.
    CHECK(canvas.text(0, 0, "ABC", 22, kWhite) == abc);
    CHECK(canvas.text(-50, 0, "ABC", 22, kWhite) == abc);
    CHECK(canvas.text(60, 0, "ABC", 22, kWhite) == abc);
    CHECK(canvas.text(0, 0, "", 22, kWhite) == 0);

    // UTF-8: a Polish letter has its own glyph; a malformed byte still
    // advances (drawn as '?') instead of stalling or vanishing.
    CHECK(splat::UiCanvas::text_width("\xC5\x82", 22) > 0);
    CHECK(splat::UiCanvas::text_width("\xFF", 22) == splat::UiCanvas::text_width("?", 22));
}

TEST_CASE("ui_canvas decodes UTF-8 code points") {
    std::size_t index = 0;
    const std::string polish = "\xC5\xBC" "a";  // "za" with a dot above the z
    CHECK(splat::ui_font::next_codepoint(polish, index) == 0x17Cu);
    CHECK(index == 2);
    CHECK(splat::ui_font::next_codepoint(polish, index) == 'a');
    CHECK(index == 3);

    index = 0;
    CHECK(splat::ui_font::next_codepoint("\xFF", index) == 0xFFFDu);
    CHECK(index == 1);
    index = 0;
    CHECK(splat::ui_font::next_codepoint("\xC5", index) == 0xFFFDu);  // truncated
    CHECK(index == 1);
    index = 0;
    CHECK(splat::ui_font::next_codepoint("\xE2\x80\xA6", index) == 0x2026u);  // ellipsis
    CHECK(index == 3);
}

TEST_CASE("ui_canvas draws anti-aliased glyphs inside the line box") {
    constexpr int kSize = 32;
    splat::UiCanvas canvas(64, 64);
    canvas.clear(kNone);
    const int width = static_cast<int>(canvas.text(4, 4, "H", kSize, kWhite));
    const int line = splat::UiCanvas::line_height(kSize);
    const int ascent = splat::ui_font::ascent(kSize);
    const int cap = splat::ui_font::cap_height(kSize);
    CHECK(line > kSize);
    CHECK(cap > kSize / 2);
    CHECK(cap < ascent);

    int solid = 0;
    int partial = 0;
    int top = 64;
    int bottom = -1;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const uint8_t alpha = pixel_at(canvas, x, y).a;
            if (alpha == 0) continue;
            CAPTURE(x);
            CAPTURE(y);
            CHECK(x >= 4);
            CHECK(x < 4 + width + 1);
            CHECK(y >= 4);
            CHECK(y < 4 + line);
            if (alpha == 255) ++solid;
            if (alpha < 255) ++partial;
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
    }
    CHECK(solid > 0);    // the stems are fully covered
    CHECK(partial > 0);  // and the edges are anti-aliased
    // 'H' stands on the baseline and is a capital high.
    CHECK(bottom == 4 + ascent - 1);
    CHECK(std::abs((bottom - top + 1) - cap) <= 1);

    // text_top_centered puts the capitals in the middle of a box.
    const int box_top = splat::UiCanvas::text_top_centered(0, 100, 22);
    const int baseline = box_top + splat::ui_font::ascent(22);
    const int cap22 = splat::ui_font::cap_height(22);
    CHECK(std::abs((baseline - cap22) - (100 - baseline)) <= 1);
}

TEST_CASE("ui_canvas rounded shapes are anti-aliased and stay in their box") {
    splat::UiCanvas canvas(24, 24);
    canvas.clear(kNone);
    canvas.fill_round_rect(0, 0, 20, 20, 6, kRed);
    CHECK(pixel_at(canvas, 10, 10) == kRed);
    CHECK(pixel_at(canvas, 0, 10) == kRed);  // the straight edge is solid
    CHECK(pixel_at(canvas, 0, 0).a == 0);    // the corner is cut
    const uint8_t corner = pixel_at(canvas, 1, 1).a;
    CHECK(corner > 0);
    CHECK(corner < 255);  // and smoothed
    CHECK(pixel_at(canvas, 20, 10).a == 0);
    CHECK(pixel_at(canvas, 10, 20).a == 0);

    splat::UiCanvas ring(24, 24);
    ring.clear(kNone);
    ring.stroke_round_rect(0, 0, 20, 20, 6, 2, kRed);
    CHECK(pixel_at(ring, 0, 10) == kRed);
    CHECK(pixel_at(ring, 1, 10) == kRed);
    CHECK(pixel_at(ring, 2, 10).a == 0);
    CHECK(pixel_at(ring, 10, 10).a == 0);

    splat::UiCanvas glow(24, 24);
    glow.clear(kNone);
    glow.glow_round_rect(8, 8, 8, 8, 2, 6, kRed);
    CHECK(pixel_at(glow, 12, 12).a == 0);  // the inside is left alone
    CHECK(pixel_at(glow, 6, 12).a > 0);
    CHECK(pixel_at(glow, 6, 12).a > pixel_at(glow, 3, 12).a);  // fading outwards
    CHECK(pixel_at(glow, 0, 12).a == 0);

    splat::UiCanvas circle(24, 24);
    circle.clear(kNone);
    circle.fill_circle(12.0f, 12.0f, 6.0f, kRed);
    CHECK(pixel_at(circle, 12, 12) == kRed);
    CHECK(pixel_at(circle, 12, 6).a > 200);
    CHECK(pixel_at(circle, 12, 5).a == 0);
    CHECK(pixel_at(circle, 0, 0).a == 0);

    splat::UiCanvas line(24, 24);
    line.clear(kNone);
    line.line(2.0f, 12.0f, 22.0f, 12.0f, 4.0f, kRed);
    CHECK(pixel_at(line, 12, 12) == kRed);
    CHECK(pixel_at(line, 12, 10) == kRed);
    CHECK(pixel_at(line, 12, 8).a == 0);

    splat::UiCanvas arc(24, 24);
    arc.clear(kNone);
    arc.stroke_arc(12.0f, 12.0f, 8.0f, 2.0f, 0.25f, kRed);  // 12 to 3 o'clock
    CHECK(pixel_at(arc, 15, 4).a > 0);   // top right quarter
    CHECK(pixel_at(arc, 4, 12).a == 0);  // the left side is not drawn
    CHECK(pixel_at(arc, 12, 19).a == 0);
}

TEST_CASE("ui_canvas blit_cover crops to fill the box") {
    // Red | green | green | blue: covering a square box shows the middle.
    splat::UiImage source = solid_image(4, 2, kGreen);
    set_pixel(source, 0, 0, kRed);
    set_pixel(source, 0, 1, kRed);
    set_pixel(source, 3, 0, kBlue);
    set_pixel(source, 3, 1, kBlue);

    splat::UiCanvas canvas(10, 10);
    canvas.clear(kNone);
    canvas.blit_cover(0, 0, 10, 10, source, 0);
    CHECK(pixel_at(canvas, 5, 5).g > 200);
    CHECK(pixel_at(canvas, 0, 5).r < pixel_at(canvas, 0, 5).g);  // red was cropped away
    CHECK(pixel_at(canvas, 0, 0).a == 255);

    splat::UiCanvas rounded(10, 10);
    rounded.clear(kNone);
    rounded.blit_cover(0, 0, 10, 10, source, 4);
    CHECK(pixel_at(rounded, 0, 0).a == 0);
    CHECK(pixel_at(rounded, 5, 5).a == 255);
}

TEST_CASE("ui_canvas blits scaled images with the right corners") {
    splat::UiImage source;
    source.resize(2, 2);
    set_pixel(source, 0, 0, kRed);
    set_pixel(source, 1, 0, kGreen);
    set_pixel(source, 0, 1, kBlue);
    set_pixel(source, 1, 1, kWhite);

    splat::UiCanvas canvas(8, 8);
    canvas.clear(kNone);
    canvas.blit(1, 2, 4, 4, source);

    // Nearest: each 2x2 destination block takes one source texel.
    CHECK(pixel_at(canvas, 1, 2) == kRed);
    CHECK(pixel_at(canvas, 2, 3) == kRed);
    CHECK(pixel_at(canvas, 3, 2) == kGreen);
    CHECK(pixel_at(canvas, 4, 3) == kGreen);
    CHECK(pixel_at(canvas, 1, 4) == kBlue);
    CHECK(pixel_at(canvas, 2, 5) == kBlue);
    CHECK(pixel_at(canvas, 3, 4) == kWhite);
    CHECK(pixel_at(canvas, 4, 5) == kWhite);
    CHECK(pixel_at(canvas, 0, 0) == kNone);
    CHECK(pixel_at(canvas, 5, 6) == kNone);

    // Clipped: the visible quarter of a 2x overhang maps to the far corner.
    splat::UiCanvas clipped(8, 8);
    clipped.clear(kNone);
    clipped.blit(-2, -2, 4, 4, source);
    CHECK(pixel_at(clipped, 0, 0) == kWhite);
    CHECK(pixel_at(clipped, 1, 1) == kWhite);
    CHECK(opaque_pixels(clipped) == 4);

    // Bilinear on a solid source stays solid everywhere.
    splat::UiCanvas smooth(6, 6);
    smooth.clear(kNone);
    smooth.blit(0, 0, 6, 6, solid_image(3, 3, splat::Rgba{10, 20, 30, 255}), true);
    for (int y = 0; y < 6; ++y) {
        for (int x = 0; x < 6; ++x) {
            CHECK(pixel_at(smooth, x, y) == splat::Rgba{10, 20, 30, 255});
        }
    }

    // An empty image and a zero-sized box are no-ops.
    splat::UiCanvas untouched(4, 4);
    untouched.clear(kNone);
    untouched.blit(0, 0, 4, 4, splat::UiImage{});
    untouched.blit(0, 0, 0, 4, source);
    CHECK(opaque_pixels(untouched) == 0);
}

TEST_CASE("ui_canvas bar fills the fraction and leaves the track") {
    constexpr splat::Rgba kTrack{10, 10, 10, 255};
    constexpr splat::Rgba kFill{200, 20, 20, 255};
    splat::UiCanvas canvas(100, 4);
    canvas.clear(kNone);
    canvas.bar(0, 0, 100, 4, 0.58f, kTrack, kFill);

    for (int x = 0; x < 100; ++x) {
        CAPTURE(x);
        CHECK(pixel_at(canvas, x, 1) == (x < 58 ? kFill : kTrack));
    }
    // Clamps at both ends.
    splat::UiCanvas full(100, 1);
    full.bar(0, 0, 100, 1, 1.5f, kTrack, kFill);
    CHECK(pixel_at(full, 99, 0) == kFill);
    splat::UiCanvas empty(100, 1);
    empty.bar(0, 0, 100, 1, -1.0f, kTrack, kFill);
    CHECK(pixel_at(empty, 0, 0) == kTrack);
    CHECK(pixel_at(empty, 99, 0) == kTrack);
}

TEST_CASE("ui_canvas stroke draws only the outline bands") {
    splat::UiCanvas canvas(10, 10);
    canvas.clear(kNone);
    canvas.stroke_rect(1, 1, 8, 8, 2, kRed);

    CHECK(pixel_at(canvas, 1, 1) == kRed);
    CHECK(pixel_at(canvas, 8, 8) == kRed);
    CHECK(pixel_at(canvas, 4, 1) == kRed);  // top band
    CHECK(pixel_at(canvas, 4, 8) == kRed);  // bottom band
    CHECK(pixel_at(canvas, 1, 4) == kRed);  // left band
    CHECK(pixel_at(canvas, 8, 4) == kRed);  // right band
    CHECK(pixel_at(canvas, 4, 4) == kNone);  // hollow
    CHECK(pixel_at(canvas, 0, 0) == kNone);

    // thickness is clamped to half the short side, so a thick stroke is solid
    splat::UiCanvas solid(6, 6);
    solid.clear(kNone);
    solid.stroke_rect(0, 0, 6, 6, 99, kRed);
    CHECK(opaque_pixels(solid) == 36);
}

TEST_CASE("ui_canvas stroke blends every covered pixel exactly once") {
    // 128 of red over an empty canvas records alpha 128; a second draw of the
    // same colour over that pixel raises it to 192, so an overlap shows up
    // both in the alpha and in the colour.
    constexpr splat::Rgba kHalfRed{255, 0, 0, 128};

    // Odd extents with a thick stroke: the two bands plus a gap cannot fit, so
    // the box is filled once instead of blending the shared row twice.
    splat::UiCanvas odd(8, 8);
    odd.clear(kNone);
    odd.stroke_rect(2, 2, 3, 3, 2, kHalfRed);
    for (int y = 2; y < 5; ++y) {
        for (int x = 2; x < 5; ++x) {
            CAPTURE(x);
            CAPTURE(y);
            CHECK(pixel_at(odd, x, y) == kHalfRed);
        }
    }
    CHECK(pixel_at(odd, 1, 2) == kNone);
    CHECK(pixel_at(odd, 5, 5) == kNone);

    // Narrower than two bands: a 1x5 box is one column, not the same column
    // drawn as both the left and the right band.
    splat::UiCanvas narrow(8, 8);
    narrow.clear(kNone);
    narrow.stroke_rect(2, 2, 1, 5, 1, kHalfRed);
    for (int y = 2; y < 7; ++y) {
        CAPTURE(y);
        CHECK(pixel_at(narrow, 2, y) == kHalfRed);
    }
    CHECK(pixel_at(narrow, 3, 4) == kNone);

    // An odd outline that does fit: corners and side bands are painted once
    // and the hollow middle stays empty.
    splat::UiCanvas outline(12, 12);
    outline.clear(kNone);
    outline.stroke_rect(1, 1, 9, 7, 2, kHalfRed);
    for (int y = 1; y < 8; ++y) {
        for (int x = 1; x < 10; ++x) {
            const bool edge = x < 3 || x >= 8 || y < 3 || y >= 6;
            CAPTURE(x);
            CAPTURE(y);
            CHECK(pixel_at(outline, x, y) == (edge ? kHalfRed : kNone));
        }
    }
    CHECK(pixel_at(outline, 0, 0) == kNone);
    CHECK(pixel_at(outline, 10, 8) == kNone);
}

TEST_CASE("ui_canvas PNG round trip matches the pixels") {
    splat::UiCanvas canvas(12, 7);
    canvas.clear(splat::Rgba{10, 20, 30, 255});
    canvas.fill_rect(1, 1, 3, 4, splat::Rgba{200, 100, 50, 255});
    canvas.text(0, 0, "HI", 1, kWhite);

    const splat::Result<std::vector<uint8_t>> encoded =
        splat::encode_png(canvas.pixels().data(), static_cast<int>(canvas.width()),
                          static_cast<int>(canvas.height()));
    REQUIRE(encoded.ok());
    const std::vector<uint8_t>& bytes = encoded.value();
    REQUIRE(bytes.size() > 8);
    // PNG magic.
    CHECK(bytes[0] == 0x89);
    CHECK(bytes[1] == 'P');
    CHECK(bytes[2] == 'N');
    CHECK(bytes[3] == 'G');

    const splat::Result<splat::Image> decoded =
        splat::decode_image(bytes.data(), bytes.size());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().width == 12);
    REQUIRE(decoded.value().height == 7);
    for (int y = 0; y < 7; ++y) {
        for (int x = 0; x < 12; ++x) {
            const splat::Rgba expected = pixel_at(canvas, x, y);
            const uint8_t* rgb = decoded.value().pixel(x, y);
            CAPTURE(x);
            CAPTURE(y);
            CHECK(rgb[0] == expected.r);
            CHECK(rgb[1] == expected.g);
            CHECK(rgb[2] == expected.b);
        }
    }

    // write_png goes through the same encoder; decode the file back.
    splat_test::TempDir dir("ui-png");
    const std::filesystem::path path = dir.path() / "canvas.png";
    const splat::Status written = canvas.write_png(path);
    REQUIRE(written.ok());
    CHECK(std::filesystem::file_size(path) == bytes.size());
    const splat::Result<splat::Image> from_file = splat::decode_image(path);
    REQUIRE(from_file.ok());
    CHECK(from_file.value().width == 12);
    CHECK(from_file.value().height == 7);

    // The encoder rejects nonsense instead of writing a broken file.
    CHECK_FALSE(splat::encode_png(nullptr, 4, 4).ok());
    CHECK_FALSE(splat::encode_png(canvas.pixels().data(), 0, 4).ok());
    CHECK_FALSE(splat::encode_png(canvas.pixels().data(), 4, -1).ok());
}

TEST_CASE("ui_canvas dirty tracks draw calls") {
    splat::UiImage source = solid_image(2, 2, kRed);
    splat::UiCanvas canvas(8, 8);
    CHECK_FALSE(canvas.dirty());

    canvas.fill_rect(0, 0, 1, 1, kRed);
    CHECK(canvas.dirty());
    canvas.clear_dirty();
    CHECK_FALSE(canvas.dirty());

    canvas.text(0, 0, "A", 1, kWhite);
    CHECK(canvas.dirty());
    canvas.clear_dirty();

    canvas.blit(0, 0, 2, 2, source);
    CHECK(canvas.dirty());
    canvas.clear_dirty();

    canvas.bar(0, 0, 4, 2, 0.5f, kBlack, kRed);
    CHECK(canvas.dirty());
    canvas.clear_dirty();

    canvas.stroke_rect(0, 0, 4, 4, 1, kRed);
    CHECK(canvas.dirty());
    canvas.clear_dirty();

    canvas.clear(kNone);
    CHECK(canvas.dirty());
    canvas.clear_dirty();

    // A fully clipped draw still marks the canvas: the host uploads while
    // dirty, and the contract is "any draw call", not "any changed pixel".
    canvas.fill_rect(-100, -100, 2, 2, kRed);
    CHECK(canvas.dirty());
}

TEST_CASE("ui_canvas theme keeps body text legible") {
    const splat::UiTheme theme = splat::default_theme();
    // On the VR panel a canvas pixel is about 0.7 headset pixels: nothing that
    // must be read is under 18 px, labels are larger, titles larger still.
    CHECK(theme.small_size >= 18);
    CHECK(theme.body_size > theme.small_size);
    CHECK(theme.heading_size > theme.body_size);
    CHECK(theme.title_size > theme.heading_size);
    // A list row holds a label and a sublabel with room to spare.
    CHECK(theme.row_height >= splat::UiTheme::text_height(theme.body_size) +
                                  splat::UiTheme::text_height(theme.small_size) + 8);
    CHECK(theme.padding > 0);
    CHECK(theme.margin >= 48);

    // The surfaces text sits on, and the text itself, are opaque.
    const splat::Rgba colours[8] = {theme.background, theme.background_top,
                                    theme.background_bottom, theme.text,
                                    theme.dim_text, theme.accent, theme.focus, theme.warning};
    for (const splat::Rgba& colour : colours) CHECK(colour.a == 255);

    // Text and focus stand out from the backdrop.
    CHECK(theme.text != theme.dim_text);
    CHECK(theme.text.r > theme.background_top.r + 150);
    CHECK(theme.focus.r > theme.background_top.r + 150);
}

TEST_CASE("ui_canvas input actions keep their bit values") {
    CHECK(splat::Up == 1u);
    CHECK(splat::Down == 2u);
    CHECK(splat::Left == 4u);
    CHECK(splat::Right == 8u);
    CHECK(splat::Select == 16u);
    CHECK(splat::Back == 32u);
    CHECK(splat::PagePrev == 64u);
    CHECK(splat::PageNext == 128u);
    CHECK(splat::Secondary == 256u);
    CHECK(splat::Tooltips == 512u);
    CHECK(splat::Menu == 1024u);

    const splat::UiInput idle;
    CHECK(idle.pressed == 0);
    CHECK(idle.held == 0);
    CHECK_FALSE(idle.pointer_valid);
    CHECK_FALSE(idle.click);
    CHECK(idle.dt == 0.0);

    const splat::UiInput pressed{splat::Select | splat::Down, splat::Down, true, 640.0f, 360.0f,
                                true, 1.0 / 60.0};
    CHECK((pressed.pressed & splat::Select) != 0);
    CHECK((pressed.pressed & splat::Up) == 0);
    CHECK(pressed.pointer_valid);
}

// IPanelContent is the contract the host (viewer) drives: update every frame,
// draw on demand, and draw() reports whether it changed any pixels.
namespace {
class NullContent : public splat::IPanelContent {
public:
    void update(const splat::UiInput&) override { updates_++; }
    bool draw(splat::UiCanvas&) override {
        draws_++;
        return false;
    }
    int updates_ = 0;
    int draws_ = 0;
};
}  // namespace

TEST_CASE("ui_canvas panel content contract") {
    splat::UiCanvas canvas(4, 4);
    NullContent content;
    splat::UiInput input;
    splat::IPanelContent& panel = content;
    panel.update(input);
    CHECK_FALSE(panel.draw(canvas));
    CHECK(content.updates_ == 1);
    CHECK(content.draws_ == 1);
}
