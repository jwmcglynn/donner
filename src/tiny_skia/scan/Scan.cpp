#include "tiny_skia/scan/Scan.h"

#include "tiny_skia/scan/HairlineAa.h"

namespace tiny_skia::scan {

void fillRect(const Rect& rect, const ScreenIntRect& clip, Blitter& blitter) {
  const auto rounded = rect.round();
  if (!rounded.has_value()) {
    return;
  }
  const auto clipped = rounded->intersect(clip.toIntRect());
  if (!clipped.has_value()) {
    return;
  }
  const auto screen = clipped->toScreenIntRect();
  if (!screen.has_value()) {
    return;
  }
  blitter.blitRect(screen.value());
}

void fillRectAa(const Rect& rect, const ScreenIntRect& clip, Blitter& blitter) {
  hairline_aa::fillRect(rect, clip, blitter);
}

}  // namespace tiny_skia::scan
