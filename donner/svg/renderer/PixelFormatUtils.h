#pragma once
/// @file
///
/// Shared RGBA8 pixel-format conversion helpers used across Donner's
/// renderer backends and the compositor. Split out from
/// `FilterGraphExecutor.h` and `CompositorController.cc` so that callers
/// which don't otherwise depend on the filter graph or the compositor
/// can still premul / unpremul without pulling those in.
///
/// Byte layout: tightly packed RGBA, 4 bytes/pixel. `rgba.size() % 4`
/// must be 0; unaligned tails are ignored (matches the pre-split
/// behavior of every caller that had its own local copy).
///
/// Rounding convention: integer `round-half-up` in both directions, so
/// a round-trip premul → unpremul is bit-identical for every
/// well-formed premul input (i.e., `r ≤ a, g ≤ a, b ≤ a`). If a caller
/// has out-of-range premul input, that is a bug upstream; these helpers
/// will clamp to 255 without corrupting alpha.

#include <cstdint>
#include <span>
#include <vector>

namespace donner::svg {

/// Convert tightly-packed straight-alpha RGBA8 to premultiplied RGBA8.
///
/// @param rgbaPixels Straight-alpha RGBA bytes (size must be a multiple of 4).
/// @return Newly-allocated premul RGBA buffer of the same size.
[[nodiscard]] std::vector<std::uint8_t> PremultiplyRgba(
    std::span<const std::uint8_t> rgbaPixels);

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
[[nodiscard]] std::vector<std::uint8_t> UnpremultiplyRgba(
    std::span<const std::uint8_t> rgbaPixels);

}  // namespace donner::svg
