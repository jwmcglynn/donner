#include "tiny_skia/filter/ColorMatrix.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace tiny_skia::filter {

void colorMatrix(Pixmap& pixmap, const std::array<double, 20>& matrix) {
  auto data = pixmap.data();
  const std::size_t pixelCount = data.size() / 4;

  for (std::size_t i = 0; i < pixelCount; ++i) {
    const std::size_t offset = i * 4;
    const double pa = data[offset + 3];

    if (pa == 0) {
      // Fully transparent: only the translation components can produce non-zero output.
      // Apply matrix to [0,0,0,0,1].
      const double nr = matrix[4] * 255.0;
      const double ng = matrix[9] * 255.0;
      const double nb = matrix[14] * 255.0;
      const double na = matrix[19] * 255.0;

      const double ca = std::clamp(na, 0.0, 255.0);
      if (ca == 0) {
        continue;  // Still transparent.
      }
      const double alphaScale = ca / 255.0;
      data[offset + 0] =
          static_cast<std::uint8_t>(std::clamp(std::round(nr * alphaScale), 0.0, 255.0));
      data[offset + 1] =
          static_cast<std::uint8_t>(std::clamp(std::round(ng * alphaScale), 0.0, 255.0));
      data[offset + 2] =
          static_cast<std::uint8_t>(std::clamp(std::round(nb * alphaScale), 0.0, 255.0));
      data[offset + 3] = static_cast<std::uint8_t>(std::round(ca));
      continue;
    }

    // Unpremultiply.
    const double invAlpha = 255.0 / pa;
    const double r = data[offset + 0] * invAlpha;
    const double g = data[offset + 1] * invAlpha;
    const double b = data[offset + 2] * invAlpha;
    const double a = pa;

    // Apply 5x4 matrix: [R,G,B,A,1] -> [R',G',B',A']
    // Translation components (matrix[4], [9], [14], [19]) are in 0-1 range per SVG spec,
    // scaled to 0-255 here.
    const double nr = matrix[0] * r + matrix[1] * g + matrix[2] * b + matrix[3] * a +
                      matrix[4] * 255.0;
    const double ng = matrix[5] * r + matrix[6] * g + matrix[7] * b + matrix[8] * a +
                      matrix[9] * 255.0;
    const double nb = matrix[10] * r + matrix[11] * g + matrix[12] * b + matrix[13] * a +
                      matrix[14] * 255.0;
    const double na = matrix[15] * r + matrix[16] * g + matrix[17] * b + matrix[18] * a +
                      matrix[19] * 255.0;

    // Clamp and re-premultiply.
    const double ca = std::clamp(na, 0.0, 255.0);
    const double alphaScale = ca / 255.0;
    data[offset + 0] =
        static_cast<std::uint8_t>(std::clamp(std::round(std::clamp(nr, 0.0, 255.0) * alphaScale), 0.0, 255.0));
    data[offset + 1] =
        static_cast<std::uint8_t>(std::clamp(std::round(std::clamp(ng, 0.0, 255.0) * alphaScale), 0.0, 255.0));
    data[offset + 2] =
        static_cast<std::uint8_t>(std::clamp(std::round(std::clamp(nb, 0.0, 255.0) * alphaScale), 0.0, 255.0));
    data[offset + 3] = static_cast<std::uint8_t>(std::round(ca));
  }
}

std::array<double, 20> saturateMatrix(double s) {
  // clang-format off
  return {
    0.2126 + 0.7874 * s, 0.7152 - 0.7152 * s, 0.0722 - 0.0722 * s, 0, 0,
    0.2126 - 0.2126 * s, 0.7152 + 0.2848 * s, 0.0722 - 0.0722 * s, 0, 0,
    0.2126 - 0.2126 * s, 0.7152 - 0.7152 * s, 0.0722 + 0.9278 * s, 0, 0,
    0,                   0,                    0,                    1, 0,
  };
  // clang-format on
}

std::array<double, 20> hueRotateMatrix(double angleDeg) {
  const double rad = angleDeg * M_PI / 180.0;
  const double cosA = std::cos(rad);
  const double sinA = std::sin(rad);
  // clang-format off
  return {
    0.213 + cosA * 0.787 - sinA * 0.213,
    0.715 - cosA * 0.715 - sinA * 0.715,
    0.072 - cosA * 0.072 + sinA * 0.928,
    0, 0,
    0.213 - cosA * 0.213 + sinA * 0.143,
    0.715 + cosA * 0.285 + sinA * 0.140,
    0.072 - cosA * 0.072 - sinA * 0.283,
    0, 0,
    0.213 - cosA * 0.213 - sinA * 0.787,
    0.715 - cosA * 0.715 + sinA * 0.715,
    0.072 + cosA * 0.928 + sinA * 0.072,
    0, 0,
    0, 0, 0, 1, 0,
  };
  // clang-format on
}

std::array<double, 20> luminanceToAlphaMatrix() {
  // clang-format off
  return {
    0, 0, 0, 0, 0,
    0, 0, 0, 0, 0,
    0, 0, 0, 0, 0,
    0.2126, 0.7152, 0.0722, 0, 0,
  };
  // clang-format on
}

std::array<double, 20> identityMatrix() {
  // clang-format off
  return {
    1, 0, 0, 0, 0,
    0, 1, 0, 0, 0,
    0, 0, 1, 0, 0,
    0, 0, 0, 1, 0,
  };
  // clang-format on
}

}  // namespace tiny_skia::filter
