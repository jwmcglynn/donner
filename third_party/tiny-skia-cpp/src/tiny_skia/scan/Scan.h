#pragma once

#include "tiny_skia/Blitter.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Path.h"

namespace tiny_skia {

namespace scan {

void fillRect(const Rect& rect, const ScreenIntRect& clip, Blitter& blitter);

void fillRectAa(const Rect& rect, const ScreenIntRect& clip, Blitter& blitter);

}  // namespace scan

}  // namespace tiny_skia
