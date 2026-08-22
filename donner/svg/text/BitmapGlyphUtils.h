#pragma once
/// @file
/// Internal checked layout validation for decoded BGRA bitmap glyphs.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace donner::svg::details {

/// Maximum accepted width or height of one decoded bitmap glyph.
inline constexpr std::size_t kMaximumBitmapGlyphDimension = 2048;

/// Maximum accepted decoded or converted byte span for one bitmap glyph.
inline constexpr std::size_t kMaximumBitmapGlyphBytes = 16 * 1024 * 1024;

/// Checked byte layout for a decoded BGRA bitmap glyph.
struct BgraBitmapLayout {
  int width = 0;
  int height = 0;
  std::ptrdiff_t pitch = 0;
  std::size_t rowBytes = 0;
  std::size_t sourceSpanBytes = 0;
  std::size_t rgbaBytes = 0;
};

/// Return the checked RGBA output size for bounded bitmap dimensions.
inline std::optional<std::size_t> ValidatedBgraOutputBytes(unsigned int width, unsigned int rows) {
  if (width == 0 || rows == 0 || width > kMaximumBitmapGlyphDimension ||
      rows > kMaximumBitmapGlyphDimension) {
    return std::nullopt;
  }

  constexpr std::size_t kBytesPerPixel = 4;
  if (width > kMaximumBitmapGlyphBytes / kBytesPerPixel) {
    return std::nullopt;
  }
  const std::size_t rowBytes = static_cast<std::size_t>(width) * kBytesPerPixel;
  if (rows > kMaximumBitmapGlyphBytes / rowBytes) {
    return std::nullopt;
  }

  return static_cast<std::size_t>(rows) * rowBytes;
}

/**
 * Validate decoded BGRA bitmap metadata before accessing or copying its buffer.
 *
 * The source buffer's allocation length is owned by the decoder and is not exposed by FreeType.
 * This validates every available part of that contract: a non-null buffer, bounded dimensions,
 * checked output size, sufficient absolute pitch, and a bounded checked source row span.
 */
inline std::optional<BgraBitmapLayout> ValidateBgraBitmapLayout(unsigned int width,
                                                                unsigned int rows, int pitch,
                                                                const uint8_t* buffer) {
  if (!buffer) {
    return std::nullopt;
  }

  constexpr std::size_t kBytesPerPixel = 4;
  const std::optional<std::size_t> rgbaBytes = ValidatedBgraOutputBytes(width, rows);
  if (!rgbaBytes) {
    return std::nullopt;
  }
  const std::size_t rowBytes = static_cast<std::size_t>(width) * kBytesPerPixel;

  const std::int64_t signedPitch = pitch;
  const std::uint64_t pitchMagnitude64 = signedPitch < 0 ? static_cast<std::uint64_t>(-signedPitch)
                                                         : static_cast<std::uint64_t>(signedPitch);
  if (pitchMagnitude64 < rowBytes || pitchMagnitude64 > kMaximumBitmapGlyphBytes) {
    return std::nullopt;
  }
  const std::size_t pitchMagnitude = static_cast<std::size_t>(pitchMagnitude64);

  const std::size_t precedingRows = static_cast<std::size_t>(rows - 1);
  if (precedingRows > (kMaximumBitmapGlyphBytes - rowBytes) / pitchMagnitude) {
    return std::nullopt;
  }
  const std::size_t sourceSpanBytes = precedingRows * pitchMagnitude + rowBytes;

  return BgraBitmapLayout{
      .width = static_cast<int>(width),
      .height = static_cast<int>(rows),
      .pitch = static_cast<std::ptrdiff_t>(pitch),
      .rowBytes = rowBytes,
      .sourceSpanBytes = sourceSpanBytes,
      .rgbaBytes = *rgbaBytes,
  };
}

/// Convert one validated BGRA bitmap into tightly packed logical-row-order RGBA pixels.
inline std::vector<uint8_t> ConvertValidatedBgraToRgba(const uint8_t* buffer,
                                                       const BgraBitmapLayout& layout) {
  std::vector<uint8_t> rgba(layout.rgbaBytes);
  // FreeType stores the first bytes of a negative-pitch buffer in the lower row. Rebase to the
  // logical upper row, then the signed pitch advances toward each following lower scanline.
  // https://freetype.org/freetype2/docs/glyphs/glyphs-7.html
  const uint8_t* logicalFirstRow =
      layout.pitch < 0 ? buffer + (layout.sourceSpanBytes - layout.rowBytes) : buffer;
  for (int row = 0; row < layout.height; ++row) {
    const uint8_t* source = logicalFirstRow + static_cast<std::ptrdiff_t>(row) * layout.pitch;
    uint8_t* destination = rgba.data() + static_cast<std::size_t>(row) * layout.rowBytes;
    for (int column = 0; column < layout.width; ++column) {
      destination[column * 4 + 0] = source[column * 4 + 2];
      destination[column * 4 + 1] = source[column * 4 + 1];
      destination[column * 4 + 2] = source[column * 4 + 0];
      destination[column * 4 + 3] = source[column * 4 + 3];
    }
  }
  return rgba;
}

}  // namespace donner::svg::details
