#pragma once
/// @file
/// Renderer-independent, bounded PNG IO and the shared pixelmatch comparison.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace donner::svg_suite {

/// Borrowed RGBA pixels, valid only for the duration of an operation accepting the view.
struct ImageView {
  int width = 0;
  int height = 0;
  size_t strideInPixels = 0;
  std::span<const uint8_t> data;
};

/// Owned, straight-alpha sRGB RGBA image. Padding is permitted after each row.
struct Image {
  int width = 0;
  int height = 0;
  size_t strideInPixels = 0;
  std::vector<uint8_t> data;
};

/// Explicit load/render outcome; errors never contain a usable image.
struct ImageResult {
  std::optional<Image> image;
  std::string error;
};

/**
 * Largest alpha difference \ref ComparisonOptions::acceptQuarterPixelEdgeCoverage accepts: the
 * 8-bit worst case of quarter-pixel edge quantization against exact coverage.
 *
 * A rasterizer that samples coverage on four sub-scanlines per pixel places each horizontal edge
 * on the nearest quarter-pixel boundary and stores an edge pixel with k of its four quarter rows
 * covered as 64 * k (255 when all four are). An exact-coverage renderer stores round(255 * f) for
 * the true coverage f, which is within 1/8 of k/4. The two differ by at most
 * 255/8 + |64 * k - 255 * k / 4| + 1/2 = 31.875 + 0.25 * k + 0.5 levels, largest at k = 3:
 * 33.125, so 33 in whole levels. An edge row covered 0.625 px, for example, is 192 in the
 * reference and 159 in the exact rendering.
 */
inline constexpr int kQuarterPixelEdgeCoverageMaxAlphaDelta = 33;

/// Diff color, RGBA, of a pixel the quarter-pixel edge coverage rule accepted. Pixelmatch draws a
/// counted pixel red and an anti-aliased pixel yellow.
inline constexpr std::array<uint8_t, 4> kEdgeCoverageDiffColor = {0, 160, 255, 255};

/// Pixelmatch policy. Defaults count every pixelmatch-visible difference, including antialiasing.
struct ComparisonOptions {
  double threshold = 0.0;
  int maxMismatchedPixels = 0;
  bool includeAntiAliasing = true;
  /**
   * Extends pixelmatch's anti-aliasing classification to edge pixels whose coverage differs by no
   * more than a quarter-pixel quantized reference allows. Off by default; requires
   * \ref includeAntiAliasing to be false.
   *
   * A pixel pixelmatch counts is accepted instead when all of these hold:
   * - both images cover it partially (alpha strictly between 0 and 255 in each);
   * - in the expected image it is the partially covered row of a horizontal edge: the pixel
   *   directly above or below it is empty or fully covered (a pixel outside the image does not
   *   count), and neither of those two pixels has its alpha;
   * - the alphas differ by at most \ref kQuarterPixelEdgeCoverageMaxAlphaDelta;
   * - the colors agree at the coverage the two pixels share: pixelmatch, at the same threshold and
   *   background, finds no difference between the two straight colors when both carry the smaller
   *   of the two alphas.
   *
   * The last condition compares color at the smaller of the two alphas, at the sensitivity
   * pixelmatch applies to a pixel of that coverage. Pixelmatch compares a partially covered pixel
   * by blending it over a background, which scales a color difference by the pixel's alpha, so at
   * very low alpha a color difference can fall under the threshold and the pixel is accepted.
   * Blending at the smaller alpha also scales the 8-bit rounding of a straight color stored for a
   * low-alpha pixel down to about one level.
   *
   * The edge condition keeps the rule to the pixels quantization moves: a quarter-pixel quantized
   * edge row sits between the shape's inside and outside, so one of its vertical neighbors is
   * empty or fully covered and differs from it. It rejects a coverage or opacity difference across
   * a uniform partially covered region, where the pixels above and below share the pixel's alpha
   * or are themselves partially covered.
   */
  bool acceptQuarterPixelEdgeCoverage = false;
};

/// Comparison result. An error never passes and has no mismatch count or diff.
struct ComparisonResult {
  bool passed = false;
  std::optional<int> mismatchedPixels;
  std::optional<Image> diff;
  std::string error;
  /// Pixels pixelmatch counted that \ref ComparisonOptions::acceptQuarterPixelEdgeCoverage
  /// accepted. They are not in \ref mismatchedPixels; the diff draws them in
  /// \ref kEdgeCoverageDiffColor.
  int edgeCoveragePixels = 0;
  /// Largest alpha difference among the \ref edgeCoveragePixels; 0 when there are none.
  int maxEdgeCoverageAlphaDelta = 0;
};

/// Compare validated RGBA views through pixelmatch. Visible dimensions must match; row strides
/// may differ. Padding is ignored, with at most one bounded input normalization buffer. With
/// ComparisonOptions::acceptQuarterPixelEdgeCoverage, one more pixelmatch pass over two
/// image-sized scratch buffers decides the color condition of the edge pixels it reclassifies.
/// @param expected Expected pixels; borrowed for this call only.
/// @param actual Actual pixels; borrowed for this call only.
/// @param options Reviewed comparison policy.
ComparisonResult CompareImages(ImageView expected, ImageView actual,
                               const ComparisonOptions& options = {});

/// Read a PNG with a 32 MiB encoded, 256 MiB decoded, and 16384-pixel dimension limit.
/// @param path Input PNG path.
/// @param maxDecoderBytes Maximum live decoder allocation bytes, including inflater scratch and
/// concurrent buffers during growth. May lower the 256 MiB ceiling, but cannot raise it.
ImageResult ReadPng(const std::filesystem::path& path,
                    size_t maxDecoderBytes = 256u * 1024u * 1024u);

/// Write a bounded RGBA image to an existing directory, returning an error string on failure.
/// @param path Output path.
/// @param image Pixels borrowed for this call only.
std::string WritePng(const std::filesystem::path& path, ImageView image);

/// Validate dimensions, stride and byte length under the decoded image bound.
/// @param image Pixels borrowed for this call only.
std::string ValidateImage(ImageView image);

}  // namespace donner::svg_suite
