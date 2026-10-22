// PS5Droid - 8x8 bitmap font for framebuffer text rendering.
//
// The glyph set is authored in this repository (no third-party font data).
// Format: 8 rows per glyph, MSB = leftmost pixel, 5 usable columns
// (bits 7..3; bits 2..0 are always zero and validated by tests).
// Charset: A-Z, 0-9, space and a small punctuation subset - enough for
// status screens ("PS5DROID 1.0.2", "CPU: 8 CORES", "[OK] MMAP", ...).
#pragma once

#include <cstdint>
#include <string>

#include "platform/display.h"

namespace ps5droid {
namespace ui {

// Returns the 8-row glyph data for an ASCII character, or nullptr when the
// character is outside the supported charset.
const uint8_t* font8x8_glyph(char ascii);

// Renders text at (x, y) into a framebuffer. Returns the number of pixels
// set. Out-of-bounds pixels are clipped.
uint32_t render_text(platform::Framebuffer* fb, int x, int y, const std::string& text,
                     platform::Rgba color, int scale = 1);

// Renders text centered horizontally at the given row.
uint32_t render_text_centered(platform::Framebuffer* fb, int y, const std::string& text,
                              platform::Rgba color, int scale = 1);

int text_pixel_width(const std::string& text, int scale = 1);

} // namespace ui
} // namespace ps5droid
