#pragma once

#include <cstdint>

#include "tiny_skia/Pixmap.h"
#include "tiny_skia/filter/FloatPixmap.h"

namespace tiny_skia::filter {

/// Fills the entire pixmap with a solid premultiplied RGBA color.
///
/// @param pixmap Output pixmap to fill.
/// @param r Red component (0-255, premultiplied).
/// @param g Green component (0-255, premultiplied).
/// @param b Blue component (0-255, premultiplied).
/// @param a Alpha component (0-255).
void flood(Pixmap& pixmap, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a);

/// Fills the entire float pixmap with a solid premultiplied RGBA color.
///
/// @param pixmap Output float pixmap to fill.
/// @param r Red component (0-1, premultiplied).
/// @param g Green component (0-1, premultiplied).
/// @param b Blue component (0-1, premultiplied).
/// @param a Alpha component (0-1).
void flood(FloatPixmap& pixmap, float r, float g, float b, float a);

}  // namespace tiny_skia::filter
