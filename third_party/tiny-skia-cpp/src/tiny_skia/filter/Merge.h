#pragma once

/// @file Merge.h
/// @brief Composites multiple pixmap layers using Source Over.

#include <span>

#include "tiny_skia/Pixmap.h"

namespace tiny_skia::filter {

/// Composites multiple premultiplied RGBA layers onto the destination using Source Over,
/// bottom-to-top (first element is the bottom layer).
///
/// @param layers Input layers to composite, in bottom-to-top order.
/// @param dst Output pixmap (cleared to transparent first, must be same dimensions as inputs).
void merge(std::span<const Pixmap* const> layers, Pixmap& dst);

}  // namespace tiny_skia::filter
