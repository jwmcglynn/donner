#pragma once

#include <cstdint>

#include "tiny_skia/Blitter.h"
#include "tiny_skia/EdgeBuilder.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Path.h"

namespace tiny_skia {

namespace scan {

void fillPath(const Path& path, FillRule fillRule, const ScreenIntRect& clip, Blitter& blitter);

void fillPathImpl(const Path& path, FillRule fillRule, const ScreenIntRect& clipRect,
                  std::int32_t startY, std::int32_t stopY, std::int32_t shiftEdgesUp,
                  bool pathContainedInClip, Blitter& blitter);

}  // namespace scan

}  // namespace tiny_skia
