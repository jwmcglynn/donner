#include "donner/svg/renderer/PixelFormatUtils.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>

namespace donner::svg {
namespace {

constexpr unsigned kUnpremultiplyFractionBits = 24;
constexpr std::uint64_t kUnpremultiplyHalf = std::uint64_t{1} << (kUnpremultiplyFractionBits - 1);

/// Ceiling reciprocals including the 255 normalization, with 24 fractional bits.
/// The channel error is less than 255/2^24, below the 1/(2*255) distance of a non-tie from a
/// rounding boundary. Ceiling sends exact ties upward; the zero-alpha entry is unused.
constexpr std::array<std::uint32_t, 256> kUnpremultiplyScales = [] {
  std::array<std::uint32_t, 256> scales{};
  constexpr std::uint64_t numerator = std::uint64_t{255} << kUnpremultiplyFractionBits;
  for (std::uint32_t alpha = 1; alpha < scales.size(); ++alpha) {
    scales[alpha] = static_cast<std::uint32_t>((numerator + alpha - 1) / alpha);
  }
  return scales;
}();

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

std::optional<std::size_t> TightRgbaBytesForDimensions(int width, int height) {
  if (height <= 0) {
    return std::nullopt;
  }

  const std::optional<std::size_t> tightRowBytes = TightRowBytesForWidth(width);
  if (!tightRowBytes.has_value()) {
    return std::nullopt;
  }

  const std::size_t sizeHeight = static_cast<std::size_t>(height);
  if (sizeHeight > std::numeric_limits<std::size_t>::max() / *tightRowBytes) {
    return std::nullopt;
  }

  return *tightRowBytes * sizeHeight;
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

bool HasExactRgbaPayload(std::span<const std::uint8_t> rgbaPixels, int width, int height) {
  const std::optional<std::size_t> tightBytes = TightRgbaBytesForDimensions(width, height);
  return tightBytes.has_value() && rgbaPixels.size() == *tightBytes;
}

void PremultiplyRgbaInto(std::span<const std::uint8_t> rgbaPixels, std::vector<std::uint8_t>& out) {
  out.assign(rgbaPixels.begin(), rgbaPixels.end());
  PremultiplyRgbaInPlace(out);
}

std::vector<std::uint8_t> PremultiplyRgba(std::span<const std::uint8_t> rgbaPixels) {
  std::vector<std::uint8_t> result;
  PremultiplyRgbaInto(rgbaPixels, result);
  return result;
}

void CopyTightRgbaRowsInto(std::span<const std::uint8_t> rgbaPixels, int width, int height,
                           std::size_t rowBytes, std::vector<std::uint8_t>& out) {
  if (!HasRgbaRows(rgbaPixels, width, height, rowBytes)) {
    out.clear();
    return;
  }

  const std::size_t tightRowBytes = *TightRowBytesForWidth(width);
  const std::size_t sizeHeight = static_cast<std::size_t>(height);
  out.resize(tightRowBytes * sizeHeight);
  if (rowBytes == tightRowBytes) {
    std::copy_n(rgbaPixels.begin(), out.size(), out.begin());
    return;
  }

  for (std::size_t y = 0; y < sizeHeight; ++y) {
    std::copy_n(rgbaPixels.begin() + static_cast<std::ptrdiff_t>(y * rowBytes), tightRowBytes,
                out.begin() + static_cast<std::ptrdiff_t>(y * tightRowBytes));
  }
}

std::vector<std::uint8_t> CopyTightRgbaRows(std::span<const std::uint8_t> rgbaPixels, int width,
                                            int height, std::size_t rowBytes) {
  std::vector<std::uint8_t> result;
  CopyTightRgbaRowsInto(rgbaPixels, width, height, rowBytes, result);
  return result;
}

void PremultiplyRgbaRowsInto(std::span<const std::uint8_t> rgbaPixels, int width, int height,
                             std::size_t rowBytes, std::vector<std::uint8_t>& out) {
  CopyTightRgbaRowsInto(rgbaPixels, width, height, rowBytes, out);
  PremultiplyRgbaInPlace(out);
}

std::vector<std::uint8_t> PremultiplyRgbaRows(std::span<const std::uint8_t> rgbaPixels, int width,
                                              int height, std::size_t rowBytes) {
  std::vector<std::uint8_t> result;
  PremultiplyRgbaRowsInto(rgbaPixels, width, height, rowBytes, result);
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
      const std::uint64_t scale = kUnpremultiplyScales[a];
      for (std::size_t channel = 0; channel < 3; ++channel) {
        const std::uint32_t rounded = static_cast<std::uint32_t>(
            (rgba[i + channel] * scale + kUnpremultiplyHalf) >> kUnpremultiplyFractionBits);
        rgba[i + channel] = static_cast<std::uint8_t>(std::min(255u, rounded));
      }
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
