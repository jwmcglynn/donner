#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "tiny_skia/Blitter.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/LineClipper.h"
#include "tiny_skia/Path.h"

namespace tiny_skia {

using LineProc = void (*)(std::span<const Point>, const ScreenIntRect* clip, Blitter& blitter);

namespace scan {

void strokePath(const Path& path, LineCap lineCap, const ScreenIntRect& clip, Blitter& blitter);

void strokePathImpl(const Path& path, LineCap lineCap, const ScreenIntRect& clip, LineProc lineProc,
                    Blitter& blitter);

}  // namespace scan

}  // namespace tiny_skia
