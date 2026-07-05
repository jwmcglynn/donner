#include "donner/svg/renderer/PixelFormatUtils.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace donner::svg {
namespace {

std::optional<std::size_t> TightRowBytesForWidth(int width) {
  if (width <= 0) {
    return std::nullopt;
  }

  const std::size_t sizeWidth = static_cast<std::size_t>(width);
  if (sizeWidth > std::numeric_limits<std::size_t>::max() / 4u) {
    return std::nullopt;
  }

  return sizeWidth * 4u;
}

bool HasRgbaRows(std::span<const std::uint8_t> rgbaPixels, int width, int height,
                 std::size_t rowBytes) {
  if (height <= 0) {
    return false;
  }

  const std::optional<std::size_t> tightRowBytes = TightRowBytesForWidth(width);
  if (!tightRowBytes.has_value() || rowBytes < *tightRowBytes) {
    return false;
  }

  const std::size_t sizeHeight = static_cast<std::size_t>(height);
  if (sizeHeight > std::numeric_limits<std::size_t>::max() / rowBytes) {
    return false;
  }

  return rgbaPixels.size() >= rowBytes * sizeHeight;
}

void PremultiplyRgbaInPlace(std::vector<std::uint8_t>& rgba) {
  for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
    const unsigned alpha = rgba[i + 3];
    rgba[i + 0] =
        static_cast<std::uint8_t>((static_cast<unsigned>(rgba[i + 0]) * alpha + 127u) / 255u);
    rgba[i + 1] =
        static_cast<std::uint8_t>((static_cast<unsigned>(rgba[i + 1]) * alpha + 127u) / 255u);
    rgba[i + 2] =
        static_cast<std::uint8_t>((static_cast<unsigned>(rgba[i + 2]) * alpha + 127u) / 255u);
  }
}

}  // namespace

std::vector<std::uint8_t> PremultiplyRgba(std::span<const std::uint8_t> rgbaPixels) {
  std::vector<std::uint8_t> result(rgbaPixels.begin(), rgbaPixels.end());
  PremultiplyRgbaInPlace(result);
  return result;
}

std::vector<std::uint8_t> CopyTightRgbaRows(std::span<const std::uint8_t> rgbaPixels, int width,
                                            int height, std::size_t rowBytes) {
  if (!HasRgbaRows(rgbaPixels, width, height, rowBytes)) {
    return {};
  }

  const std::size_t tightRowBytes = *TightRowBytesForWidth(width);
  const std::size_t sizeHeight = static_cast<std::size_t>(height);
  std::vector<std::uint8_t> result(tightRowBytes * sizeHeight);
  if (rowBytes == tightRowBytes) {
    std::copy_n(rgbaPixels.begin(), result.size(), result.begin());
    return result;
  }

  for (std::size_t y = 0; y < sizeHeight; ++y) {
    std::copy_n(rgbaPixels.begin() + static_cast<std::ptrdiff_t>(y * rowBytes), tightRowBytes,
                result.begin() + static_cast<std::ptrdiff_t>(y * tightRowBytes));
  }

  return result;
}

std::vector<std::uint8_t> PremultiplyRgbaRows(std::span<const std::uint8_t> rgbaPixels, int width,
                                              int height, std::size_t rowBytes) {
  std::vector<std::uint8_t> result = CopyTightRgbaRows(rgbaPixels, width, height, rowBytes);
  PremultiplyRgbaInPlace(result);
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
      // Integer (r * 255 * 256 / a + 128) >> 8 - carried forward
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

std::vector<std::uint8_t> UnpremultiplyRgbaRows(std::span<const std::uint8_t> rgbaPixels, int width,
                                                int height, std::size_t rowBytes) {
  std::vector<std::uint8_t> result = CopyTightRgbaRows(rgbaPixels, width, height, rowBytes);
  UnpremultiplyRgbaInPlace(result);
  return result;
}

}  // namespace donner::svg
