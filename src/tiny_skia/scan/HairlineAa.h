#pragma once

#include <cstdint>

#include "tiny_skia/Blitter.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Path.h"

namespace tiny_skia {

namespace scan {
namespace hairline_aa {

void strokePath(const Path& path, LineCap lineCap, const ScreenIntRect& clip, Blitter& blitter);

void fillRect(const Rect& rect, const ScreenIntRect& clip, Blitter& blitter);

}  // namespace hairline_aa
}  // namespace scan

}  // namespace tiny_skia
