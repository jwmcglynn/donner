#pragma once
/// @file
/// Shared scene for the `Device::writeTexture` destination-origin tests.
///
/// Every backend test fills the same destination with the same sentinel, uploads the same
/// rectangle at the same nonzero origin, and checks the same expected bytes, so a backend that
/// ignores the origin, transposes it, or clamps it to the texture edge shows up as a byte
/// difference rather than as a test that agrees with itself.
///
/// The rectangle is taken from an interior position whose column and row differ, and whose width
/// and height differ, so neither a swapped axis nor a transposed extent reproduces the expected
/// image. A transposed origin still lands wholly inside the texture, so that mistake surfaces as
/// misplaced texels rather than as a refusal the runtime would have caught anyway. The uploaded
/// texels encode their own destination coordinates, so a texel that arrives in the wrong place
/// names where it was supposed to go.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/Vector2.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::gpu::tests {

/// Width and height of the destination texture, in texels.
inline constexpr uint32_t kSubRectUploadExtent = 8;

/// Bytes per row of the staging buffers, meeting the runtime's 256-byte row pitch.
inline constexpr uint32_t kSubRectUploadBytesPerRow = 256;

/// Width of the uploaded rectangle, in texels.
inline constexpr uint32_t kSubRectUploadWidth = 3;

/// Height of the uploaded rectangle, in texels. Different from the width so a backend that
/// transposed the extent would not still produce the expected image.
inline constexpr uint32_t kSubRectUploadHeight = 2;

/// Column of the upload's top-left texel in the destination.
inline constexpr uint32_t kSubRectUploadX = 4;

/// Row of the upload's top-left texel in the destination. Different from \ref kSubRectUploadX so
/// a backend that swapped the axes would be caught, and small enough that the swapped rectangle
/// still fits, so the swap shows up as misplaced texels instead of a refusal.
inline constexpr uint32_t kSubRectUploadY = 1;

/// The sentinel every destination texel starts at. Distinct from every uploaded texel, so an
/// untouched destination texel cannot be mistaken for an uploaded one.
inline std::array<uint8_t, 4> SubRectUploadDestinationFill() {
  return {0x01, 0x02, 0x03, 0xFF};
}

/// Texel the upload carries for destination texel (\p x, \p y), RGBA in texel units. Each texel
/// encodes the destination coordinates it belongs at.
/// @param x Column in the destination texture.
/// @param y Row in the destination texture.
inline std::array<uint8_t, 4> SubRectUploadTexel(uint32_t x, uint32_t y) {
  return {static_cast<uint8_t>(0x10u + x * 0x11u), static_cast<uint8_t>(0x20u + y * 0x11u), 0x40,
          0xFF};
}

/// True if destination texel (\p x, \p y) lies inside the uploaded rectangle.
/// @param x Column in the destination texture.
/// @param y Row in the destination texture.
inline bool SubRectUploadCoversDestination(uint32_t x, uint32_t y) {
  return x >= kSubRectUploadX && x < kSubRectUploadX + kSubRectUploadWidth &&
         y >= kSubRectUploadY && y < kSubRectUploadY + kSubRectUploadHeight;
}

/// Expected destination texel at (\p x, \p y) after the upload.
/// @param x Column in the destination texture.
/// @param y Row in the destination texture.
inline std::array<uint8_t, 4> SubRectUploadExpectedTexel(uint32_t x, uint32_t y) {
  if (!SubRectUploadCoversDestination(x, y)) {
    return SubRectUploadDestinationFill();
  }
  return SubRectUploadTexel(x, y);
}

/// The destination's starting fill packed into rows of \ref kSubRectUploadBytesPerRow, ready for
/// a whole-texture `Device::writeTexture`.
inline std::vector<uint8_t> SubRectUploadDestinationFillBytes() {
  std::vector<uint8_t> rows(size_t{kSubRectUploadBytesPerRow} * kSubRectUploadExtent, 0);
  const std::array<uint8_t, 4> fill = SubRectUploadDestinationFill();
  for (uint32_t y = 0; y < kSubRectUploadExtent; ++y) {
    for (uint32_t x = 0; x < kSubRectUploadExtent; ++x) {
      const size_t offset = size_t{y} * kSubRectUploadBytesPerRow + size_t{x} * 4u;
      rows[offset + 0] = fill[0];
      rows[offset + 1] = fill[1];
      rows[offset + 2] = fill[2];
      rows[offset + 3] = fill[3];
    }
  }
  return rows;
}

/// The texels of a \ref kSubRectUploadWidth by \ref kSubRectUploadHeight rectangle whose
/// top-left destination texel is (\p originX, \p originY), packed into rows of
/// \ref kSubRectUploadBytesPerRow and ready for a `Device::writeTexture` at that origin. The
/// payload always starts at byte zero of its first row: the origin addresses the destination,
/// never the source bytes, so a backend that offset into the payload by the origin would read
/// past this buffer's live rows.
/// @param originX Column of the rectangle's top-left destination texel.
/// @param originY Row of the rectangle's top-left destination texel.
inline std::vector<uint8_t> SubRectUploadBytesAt(uint32_t originX, uint32_t originY) {
  std::vector<uint8_t> rows(size_t{kSubRectUploadBytesPerRow} * kSubRectUploadHeight, 0);
  for (uint32_t row = 0; row < kSubRectUploadHeight; ++row) {
    for (uint32_t column = 0; column < kSubRectUploadWidth; ++column) {
      const std::array<uint8_t, 4> texel = SubRectUploadTexel(originX + column, originY + row);
      const size_t offset = size_t{row} * kSubRectUploadBytesPerRow + size_t{column} * 4u;
      rows[offset + 0] = texel[0];
      rows[offset + 1] = texel[1];
      rows[offset + 2] = texel[2];
      rows[offset + 3] = texel[3];
    }
  }
  return rows;
}

/// The rectangle's texels for the scene's own origin, (\ref kSubRectUploadX,
/// \ref kSubRectUploadY).
inline std::vector<uint8_t> SubRectUploadBytes() {
  return SubRectUploadBytesAt(kSubRectUploadX, kSubRectUploadY);
}

/// Builds a whole-texture image by asking \p expectedTexel for each texel, packed into rows of
/// \ref kSubRectUploadBytesPerRow. The bytes between the last texel of a row and the next row
/// start are left zero in every image built this way, and the comparison reads only the texels.
/// @param expectedTexel Callable taking (x, y) and returning the RGBA texel expected there.
template <typename ExpectedTexelFn>
std::vector<uint8_t> SubRectUploadImageBytes(ExpectedTexelFn expectedTexel) {
  std::vector<uint8_t> rows(size_t{kSubRectUploadBytesPerRow} * kSubRectUploadExtent, 0);
  for (uint32_t y = 0; y < kSubRectUploadExtent; ++y) {
    for (uint32_t x = 0; x < kSubRectUploadExtent; ++x) {
      const std::array<uint8_t, 4> texel = expectedTexel(x, y);
      const size_t offset = size_t{y} * kSubRectUploadBytesPerRow + size_t{x} * 4u;
      rows[offset + 0] = texel[0];
      rows[offset + 1] = texel[1];
      rows[offset + 2] = texel[2];
      rows[offset + 3] = texel[3];
    }
  }
  return rows;
}

/// The whole expected image after an upload at the scene's own origin.
inline std::vector<uint8_t> SubRectUploadExpectedImageBytes() {
  return SubRectUploadImageBytes(SubRectUploadExpectedTexel);
}

/// Wraps whole-texture readback rows as a bitmap. Rows sit \ref kSubRectUploadBytesPerRow apart,
/// which the comparison reads as a pixel stride, so the row padding is never compared.
/// @param rows Readback rows, kSubRectUploadBytesPerRow * kSubRectUploadExtent bytes.
inline svg::RendererBitmap SubRectUploadBitmap(std::vector<uint8_t> rows) {
  svg::RendererBitmap bitmap;
  bitmap.dimensions =
      Vector2i(static_cast<int>(kSubRectUploadExtent), static_cast<int>(kSubRectUploadExtent));
  bitmap.pixels = std::move(rows);
  bitmap.rowBytes = kSubRectUploadBytesPerRow;
  return bitmap;
}

/// Names the first texel where \p actual differs from \p expected, or an empty string when every
/// texel matches. This decides nothing: pixelmatch makes the acceptance decision, and this only
/// names a coordinate, which the comparison's mismatched-pixel count does not.
/// @param actual Readback rows. @param expected Expected image rows.
inline std::string SubRectUploadFirstDifference(const std::vector<uint8_t>& actual,
                                                const std::vector<uint8_t>& expected) {
  const size_t needed = size_t{kSubRectUploadBytesPerRow} * kSubRectUploadExtent;
  if (actual.size() < needed || expected.size() < needed) {
    return "readback is shorter than one whole texture";
  }
  for (uint32_t y = 0; y < kSubRectUploadExtent; ++y) {
    for (uint32_t x = 0; x < kSubRectUploadExtent; ++x) {
      const size_t offset = size_t{y} * kSubRectUploadBytesPerRow + size_t{x} * 4u;
      if (!std::equal(actual.begin() + offset, actual.begin() + offset + 4,
                      expected.begin() + offset)) {
        return "first differing texel is (" + std::to_string(x) + ", " + std::to_string(y) + ")";
      }
    }
  }
  return "";
}

/// Compares a whole-texture readback against \p expected through the project's pixelmatch helper
/// at identity: zero threshold, zero mismatched pixels allowed, and anti-aliased pixels counted,
/// so a single misplaced texel fails rather than being written off as an edge artifact. On
/// mismatch the helper writes inspectable actual, expected and diff images, and the trace names
/// the first differing texel.
/// @param actual Readback rows. @param expected Expected image rows.
/// @param testLabel Label the comparison puts on the artifacts it writes.
inline void ExpectSubRectUploadImageMatches(std::vector<uint8_t> actual,
                                            std::vector<uint8_t> expected,
                                            std::string_view testLabel) {
  SCOPED_TRACE(SubRectUploadFirstDifference(actual, expected));
  editor::tests::CompareBitmapToBitmap(SubRectUploadBitmap(std::move(actual)),
                                       SubRectUploadBitmap(std::move(expected)), testLabel,
                                       editor::tests::PixelmatchIdentityParams());
}

}  // namespace donner::gpu::tests
