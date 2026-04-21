#pragma once
/// @file

#include <cstdint>
#include <vector>

namespace donner::svg {

/// Whether RGB channels of an RGBA pixel are stored with alpha already
/// multiplied in (premultiplied) or kept independent (straight). Mirrors
/// `donner::svg::AlphaType` in `RendererInterface.h`; we avoid pulling the
/// full renderer header in here to keep `ImageResource` a leaf dependency.
enum class ImageAlphaType : std::uint8_t {
  /// Default: RGB is the raw color, `drawImage` callers must multiply by
  /// alpha before the source-over blend. Matches `<image>` decode output
  /// and tiny-skia's `takeSnapshot` contract.
  Unpremultiplied = 0,
  /// RGB has already been multiplied by alpha. Used internally by the
  /// compositor to skip the `takeSnapshot`-unpremultiply /
  /// `drawImage`-remultiply round-trip on cached layer bitmaps — each
  /// of those conversions drops ~1 channel unit of precision at 8-bit
  /// and artifacts accumulate across nested compose passes.
  Premultiplied = 1,
};

/// Contains a decoded image resource in RGBA format.
struct ImageResource {
  /// Pixel data in RGBA format.
  std::vector<uint8_t> data;

  /// Width of the image, in pixels.
  int width;

  /// Height of the image, in pixels.
  int height;

  /// Alpha-storage convention for `data`. Defaults to `Unpremultiplied`
  /// because the historical callers (SVG `<image>` decode, font glyph
  /// bitmaps, pattern tile images) all produce straight-alpha bytes.
  ImageAlphaType alphaType = ImageAlphaType::Unpremultiplied;
};

}  // namespace donner::svg
