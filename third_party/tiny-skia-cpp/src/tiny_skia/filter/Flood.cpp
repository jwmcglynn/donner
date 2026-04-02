#include "tiny_skia/filter/Flood.h"

#include <cstddef>

namespace tiny_skia::filter {

void flood(Pixmap& pixmap, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
  auto data = pixmap.data();
  for (std::size_t i = 0; i < data.size(); i += 4) {
    data[i + 0] = r;
    data[i + 1] = g;
    data[i + 2] = b;
    data[i + 3] = a;
  }
}

void flood(FloatPixmap& pixmap, float r, float g, float b, float a) {
  // Values are already premultiplied (from floodToPremul in the renderer).
  pixmap.fill(r, g, b, a);
}

}  // namespace tiny_skia::filter
