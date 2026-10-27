// Controller button prompts drawn as shapes, not letters: the hint bar and the
// photo picker's L1/R1 tab arrows. Our own drawings of the four face-button
// symbols (cross, circle, square, triangle) on a light disc, plus OPTIONS and
// the shoulder buttons as small labelled pills. No console artwork is used.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

#include "ui/canvas.h"

namespace splat {

// Draws the prompt for `button` ("Cross", "Circle", "Square", "Triangle",
// "Options", "L1", "R1", "L1/R1"; anything else is drawn as its text) with its
// left edge at x, centred on cy, `height` px tall, the shape in `ink` on a
// `plate`. Returns the width it used.
int draw_button_glyph(UiCanvas& canvas, int x, int cy, std::string_view button, int height,
                      Rgba plate, Rgba ink);

// The width draw_button_glyph() would use.
int button_glyph_width(std::string_view button, int height);

}  // namespace splat
