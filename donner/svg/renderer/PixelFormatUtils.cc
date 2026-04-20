#include "donner/svg/renderer/PixelFormatUtils.h"

#include <algorithm>

namespace donner::svg {

std::vector<std::uint8_t> PremultiplyRgba(std::span<const std::uint8_t> rgbaPixels) {
  std::vector<std::uint8_t> result(rgbaPixels.begin(), rgbaPixels.end());
  for (std::size_t i = 0; i + 3 < result.size(); i += 4) {
    const unsigned alpha = result[i + 3];
    result[i + 0] =
        static_cast<std::uint8_t>((static_cast<unsigned>(result[i + 0]) * alpha + 127u) / 255u);
    result[i + 1] =
        static_cast<std::uint8_t>((static_cast<unsigned>(result[i + 1]) * alpha + 127u) / 255u);
    result[i + 2] =
        static_cast<std::uint8_t>((static_cast<unsigned>(result[i + 2]) * alpha + 127u) / 255u);
  }
  return result;
}

void UnpremultiplyRgbaInPlace(std::vector<std::uint8_t>& rgba) {
  for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
    const std::uint8_t a = rgba[i + 3];
    if (a == 0) {
      rgba[i + 0] = 0;
      rgba[i + 1] = 0;
      rgba[i + 2] = 0;
    } else if (a != 255) {
      // Integer (r * 255 * 256 / a + 128) >> 8 — carried forward
      // verbatim from `CompositorController::UnpremultiplyPixels`
      // (the pre-shared-helper origin), which has been validated
      // against the full compositor golden suite. Preserve this
      // exact math to keep `BuildImageResource` output bit-identical
      // across the helper relocation.
      const std::uint32_t scale = (255u * 256u) / a;  // inverse alpha * 256
      rgba[i + 0] = static_cast<std::uint8_t>(
          std::min<std::uint32_t>(255u, (rgba[i + 0] * scale + 128u) >> 8u));
      rgba[i + 1] = static_cast<std::uint8_t>(
          std::min<std::uint32_t>(255u, (rgba[i + 1] * scale + 128u) >> 8u));
      rgba[i + 2] = static_cast<std::uint8_t>(
          std::min<std::uint32_t>(255u, (rgba[i + 2] * scale + 128u) >> 8u));
    }
    // a == 255 → channels unchanged (already straight alpha).
  }
}

std::vector<std::uint8_t> UnpremultiplyRgba(std::span<const std::uint8_t> rgbaPixels) {
  std::vector<std::uint8_t> result(rgbaPixels.begin(), rgbaPixels.end());
  UnpremultiplyRgbaInPlace(result);
  return result;
}

}  // namespace donner::svg
