#pragma once
/// @file
///
/// Shared RGBA8 pixel-format conversion helpers used across Donner's
/// renderer backends and the compositor. Split out from
/// `FilterGraphExecutor.h` and `CompositorController.cc` so that callers
/// which don't otherwise depend on the filter graph or the compositor
/// can still premul / unpremul without pulling those in.
///
/// Byte layout: RGBA, 4 bytes/pixel. Conversion helpers without an
/// explicit `rowBytes` parameter expect tightly-packed data; row helpers
/// copy from the caller-provided stride into a tightly-packed output.
/// Unaligned tails are ignored (matches the pre-split behavior of every
/// caller that had its own local copy).
///
/// Rounding convention: integer `round-half-up` in both directions, so
/// a round-trip premul → unpremul is bit-identical for every
/// well-formed premul input (i.e., `r ≤ a, g ≤ a, b ≤ a`). If a caller
/// has out-of-range premul input, that is a bug upstream; these helpers
/// will clamp to 255 without corrupting alpha.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace donner::svg {

/// Convert tightly-packed straight-alpha RGBA8 to premultiplied RGBA8.
///
/// @param rgbaPixels Straight-alpha RGBA bytes (size must be a multiple of 4).
/// @return Newly-allocated premul RGBA buffer of the same size.
[[nodiscard]] std::vector<std::uint8_t> PremultiplyRgba(std::span<const std::uint8_t> rgbaPixels);

/// Copy row-strided RGBA8 bytes into a tightly-packed RGBA8 buffer.
///
/// @param rgbaPixels Source RGBA bytes.
/// @param width Pixel width.
/// @param height Pixel height.
/// @param rowBytes Bytes between source rows.
/// @return A tightly-packed RGBA buffer, or an empty vector when dimensions
/// or row storage are invalid.
[[nodiscard]] std::vector<std::uint8_t> CopyTightRgbaRows(std::span<const std::uint8_t> rgbaPixels,
                                                          int width, int height,
                                                          std::size_t rowBytes);

/// Convert row-strided straight-alpha RGBA8 to tightly-packed premultiplied RGBA8.
///
/// @param rgbaPixels Straight-alpha source RGBA bytes.
/// @param width Pixel width.
/// @param height Pixel height.
/// @param rowBytes Bytes between source rows.
/// @return A tightly-packed premul RGBA buffer, or an empty vector when
/// dimensions or row storage are invalid.
[[nodiscard]] std::vector<std::uint8_t> PremultiplyRgbaRows(
    std::span<const std::uint8_t> rgbaPixels, int width, int height, std::size_t rowBytes);

/// Convert tightly-packed premultiplied RGBA8 to straight-alpha RGBA8,
/// in place. Fully-opaque pixels (alpha == 255) are unchanged;
/// fully-transparent pixels (alpha == 0) become `(0, 0, 0, 0)`.
///
/// @param rgba Premultiplied RGBA bytes (size must be a multiple of 4).
void UnpremultiplyRgbaInPlace(std::vector<std::uint8_t>& rgba);

/// Non-mutating variant of `UnpremultiplyRgbaInPlace` that allocates.
/// Prefer the in-place form on hot paths; use this when the caller
/// needs to preserve the input.
///
/// @param rgbaPixels Premultiplied RGBA bytes (size must be a multiple of 4).
[[nodiscard]] std::vector<std::uint8_t> UnpremultiplyRgba(std::span<const std::uint8_t> rgbaPixels);

/// Convert row-strided premultiplied RGBA8 to tightly-packed straight-alpha RGBA8.
///
/// @param rgbaPixels Premultiplied source RGBA bytes.
/// @param width Pixel width.
/// @param height Pixel height.
/// @param rowBytes Bytes between source rows.
/// @return A tightly-packed straight-alpha RGBA buffer, or an empty vector when
/// dimensions or row storage are invalid.
[[nodiscard]] std::vector<std::uint8_t> UnpremultiplyRgbaRows(
    std::span<const std::uint8_t> rgbaPixels, int width, int height, std::size_t rowBytes);

}  // namespace donner::svg
