#pragma once
/// @file
/// Shared scene for the texture-to-texture sub-rectangle copy slices.
///
/// Every backend slice copies the same rectangle out of the same source into the same
/// pre-filled destination and checks the same expected bytes, so a backend that ignores an
/// origin, swaps the two origins, or clamps one of them shows up as a byte difference rather
/// than as a test that agrees with itself.
///
/// The source texel values encode their own coordinates, and the copy rectangle is taken from an
/// interior corner with a different destination corner. A backend that dropped either origin, or
/// applied the source origin to the destination, would land different bytes in different places,
/// so no single mistake reproduces the expected image.

#include <array>
#include <cstdint>
#include <vector>

namespace donner::gpu::tests {

/// Width and height of both the source and the destination texture, in texels.
inline constexpr uint32_t kSubRectCopyExtent = 8;

/// Bytes per row of the staging buffers, meeting the runtime's 256-byte row pitch.
inline constexpr uint32_t kSubRectCopyBytesPerRow = 256;

/// Width of the copied rectangle, in texels.
inline constexpr uint32_t kSubRectCopyWidth = 3;

/// Height of the copied rectangle, in texels. Different from the width so a backend that
/// transposed the extent would not still produce the expected image.
inline constexpr uint32_t kSubRectCopyHeight = 2;

/// Column of the copy's top-left texel in the source.
inline constexpr uint32_t kSubRectCopySourceX = 5;

/// Row of the copy's top-left texel in the source. Different from \ref kSubRectCopySourceX so a
/// backend that swapped the axes would be caught.
inline constexpr uint32_t kSubRectCopySourceY = 1;

/// Column of the copy's top-left texel in the destination. Disjoint from the source rectangle so
/// a backend that applied the source origin to both sides would be caught.
inline constexpr uint32_t kSubRectCopyDestinationX = 1;

/// Row of the copy's top-left texel in the destination.
inline constexpr uint32_t kSubRectCopyDestinationY = 5;

/// Texel of the source pattern at (\p x, \p y), RGBA in texel units. Each texel encodes its own
/// coordinates, so a texel that arrives from the wrong place names where it came from.
/// @param x Column in the source texture.
/// @param y Row in the source texture.
inline std::array<uint8_t, 4> SubRectCopySourceTexel(uint32_t x, uint32_t y) {
  return {static_cast<uint8_t>(0x10u + x * 0x11u), static_cast<uint8_t>(0x20u + y * 0x11u), 0x40,
          0xFF};
}

/// The sentinel every destination texel starts at. Distinct from every source texel, so an
/// untouched destination texel cannot be mistaken for a copied one.
inline std::array<uint8_t, 4> SubRectCopyDestinationFill() {
  return {0x01, 0x02, 0x03, 0xFF};
}

/// True if destination texel (\p x, \p y) lies inside the copied rectangle.
/// @param x Column in the destination texture.
/// @param y Row in the destination texture.
inline bool SubRectCopyCoversDestination(uint32_t x, uint32_t y) {
  return x >= kSubRectCopyDestinationX && x < kSubRectCopyDestinationX + kSubRectCopyWidth &&
         y >= kSubRectCopyDestinationY && y < kSubRectCopyDestinationY + kSubRectCopyHeight;
}

/// Expected destination texel at (\p x, \p y) after the copy.
/// @param x Column in the destination texture.
/// @param y Row in the destination texture.
inline std::array<uint8_t, 4> SubRectCopyExpectedTexel(uint32_t x, uint32_t y) {
  if (!SubRectCopyCoversDestination(x, y)) {
    return SubRectCopyDestinationFill();
  }
  return SubRectCopySourceTexel(x - kSubRectCopyDestinationX + kSubRectCopySourceX,
                                y - kSubRectCopyDestinationY + kSubRectCopySourceY);
}

/// The source pattern packed into rows of \ref kSubRectCopyBytesPerRow, ready for
/// `Device::writeTexture`.
inline std::vector<uint8_t> SubRectCopySourceUpload() {
  std::vector<uint8_t> rows(size_t{kSubRectCopyBytesPerRow} * kSubRectCopyExtent, 0);
  for (uint32_t y = 0; y < kSubRectCopyExtent; ++y) {
    for (uint32_t x = 0; x < kSubRectCopyExtent; ++x) {
      const std::array<uint8_t, 4> texel = SubRectCopySourceTexel(x, y);
      const size_t offset = size_t{y} * kSubRectCopyBytesPerRow + size_t{x} * 4u;
      rows[offset + 0] = texel[0];
      rows[offset + 1] = texel[1];
      rows[offset + 2] = texel[2];
      rows[offset + 3] = texel[3];
    }
  }
  return rows;
}

/// The destination's starting fill packed into rows of \ref kSubRectCopyBytesPerRow, ready for
/// `Device::writeTexture`.
inline std::vector<uint8_t> SubRectCopyDestinationUpload() {
  std::vector<uint8_t> rows(size_t{kSubRectCopyBytesPerRow} * kSubRectCopyExtent, 0);
  const std::array<uint8_t, 4> fill = SubRectCopyDestinationFill();
  for (uint32_t y = 0; y < kSubRectCopyExtent; ++y) {
    for (uint32_t x = 0; x < kSubRectCopyExtent; ++x) {
      const size_t offset = size_t{y} * kSubRectCopyBytesPerRow + size_t{x} * 4u;
      rows[offset + 0] = fill[0];
      rows[offset + 1] = fill[1];
      rows[offset + 2] = fill[2];
      rows[offset + 3] = fill[3];
    }
  }
  return rows;
}

}  // namespace donner::gpu::tests
