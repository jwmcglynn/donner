#pragma once
/// @file

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <source_location>
#include <string>

namespace donner::svg::test {

/// Render an RGBA pixel as "{r, g, b, a}" for matcher messages.
inline std::string FormatRgba(const std::array<uint8_t, 4>& px) {
  return testing::PrintToString(std::array<int, 4>{px[0], px[1], px[2], px[3]});
}

/**
 * The four bytes of the pixel at (\p x, \p y) of a snapshot bitmap whose rows are \c rowBytes
 * apart, or tightly packed when that is zero (a \c RendererBitmap or anything shaped like one).
 *
 * A read outside the bitmap fails the calling test at the caller's line, naming the bitmap's
 * extent, and an empty bitmap is named as one: a renderer that could not read its frame back
 * returns an empty snapshot, and a read that quietly answered zeros there reported the missing
 * readback as a transparent pixel, which looks like a rendering defect. The read still answers
 * zeros so the caller's own assertion runs and prints what it expected.
 *
 * @param bitmap Bitmap to read.
 * @param x Column. @param y Row.
 * @param caller Where the read was made; defaults to the call site.
 */
template <typename Bitmap>
std::array<uint8_t, 4> PixelAt(const Bitmap& bitmap, int x, int y,
                               std::source_location caller = std::source_location::current()) {
  const bool inside = x >= 0 && y >= 0 && x < bitmap.dimensions.x && y < bitmap.dimensions.y;
  const size_t rowBytes = bitmap.rowBytes != 0 ? static_cast<size_t>(bitmap.rowBytes)
                                               : static_cast<size_t>(bitmap.dimensions.x) * 4u;
  const size_t offset = static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x) * 4u;
  if (!inside || bitmap.pixels.size() < offset + 4u) {
    if (bitmap.pixels.empty()) {
      ADD_FAILURE_AT(caller.file_name(), caller.line())
          << "read pixel (" << x << ", " << y << ") of an empty snapshot: the renderer returned no "
          << "pixels, so the frame was not read back";
    } else if (!inside) {
      ADD_FAILURE_AT(caller.file_name(), caller.line())
          << "read pixel (" << x << ", " << y << ") outside a " << bitmap.dimensions.x << "x"
          << bitmap.dimensions.y << " snapshot";
    } else {
      ADD_FAILURE_AT(caller.file_name(), caller.line())
          << "read pixel (" << x << ", " << y << ") of a " << bitmap.dimensions.x << "x"
          << bitmap.dimensions.y << " snapshot whose " << bitmap.pixels.size()
          << " bytes end before it";
    }
    return {0, 0, 0, 0};
  }
  return {bitmap.pixels[offset], bitmap.pixels[offset + 1], bitmap.pixels[offset + 2],
          bitmap.pixels[offset + 3]};
}

/**
 * Counts the pixels of \p bitmap whose RGBA satisfies \p predicate, reading each through
 * \ref PixelAt.
 *
 * @param bitmap Snapshot to count.
 * @param predicate Called with each pixel's RGBA; true counts the pixel.
 * @param caller Where the count was made; defaults to the call site.
 * @return Pixels satisfying \p predicate.
 */
template <typename Bitmap, typename Predicate>
size_t CountPixelsWhere(const Bitmap& bitmap, const Predicate& predicate,
                        std::source_location caller = std::source_location::current()) {
  size_t count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      if (predicate(PixelAt(bitmap, x, y, caller))) {
        ++count;
      }
    }
  }
  return count;
}

/**
 * Pixels of \p bitmap with a non-zero alpha: the liveness signal that something rendered.
 *
 * @param bitmap Snapshot to count.
 * @param caller Where the count was made; defaults to the call site.
 * @return Pixels whose alpha is not zero.
 */
template <typename Bitmap>
size_t CountNonTransparentPixels(const Bitmap& bitmap,
                                 std::source_location caller = std::source_location::current()) {
  return CountPixelsWhere(
      bitmap, [](const std::array<uint8_t, 4>& pixel) { return pixel[3] != 0; }, caller);
}

/// Matches a pixel whose channels each satisfy their own sub-matcher.
///
/// Prefer this over four separate channel expectations: a mismatch on any channel prints all four
/// channels at once and names the failing channel.
MATCHER_P4(Rgba, rMatcher, gMatcher, bMatcher, aMatcher,
           std::string("pixel RGBA matches {R=") + testing::DescribeMatcher<int>(rMatcher) +
               ", G=" + testing::DescribeMatcher<int>(gMatcher) +
               ", B=" + testing::DescribeMatcher<int>(bMatcher) +
               ", A=" + testing::DescribeMatcher<int>(aMatcher) + "}") {
  const std::array<uint8_t, 4> px = {arg[0], arg[1], arg[2], arg[3]};
  const testing::Matcher<int> channelMatchers[4] = {
      testing::SafeMatcherCast<int>(rMatcher), testing::SafeMatcherCast<int>(gMatcher),
      testing::SafeMatcherCast<int>(bMatcher), testing::SafeMatcherCast<int>(aMatcher)};
  static constexpr const char* kNames[4] = {"R", "G", "B", "A"};
  bool ok = true;
  for (int c = 0; c < 4; ++c) {
    if (!channelMatchers[c].Matches(px[c])) {
      ok = false;
    }
  }

  *result_listener << "actual RGBA=" << FormatRgba(px);
  if (!ok) {
    for (int c = 0; c < 4; ++c) {
      if (!channelMatchers[c].Matches(px[c])) {
        *result_listener << "; " << kNames[c] << "=" << static_cast<int>(px[c]) << " fails ("
                         << testing::DescribeMatcher<int>(channelMatchers[c]) << ")";
      }
    }
  }

  return ok;
}

/// Matches a pixel whose channels exactly equal the given RGBA values.
inline auto RgbaEq(int r, int g, int b, int a) {
  using testing::Eq;
  return Rgba(Eq(r), Eq(g), Eq(b), Eq(a));
}

/// Matches an integer channel within `tol` of `expected`.
inline testing::Matcher<int> Near(int expected, int tol) {
  return testing::AllOf(testing::Ge(expected - tol), testing::Le(expected + tol));
}

/// Matches a pixel whose alpha channel satisfies `alphaMatcher`, ignoring RGB.
MATCHER_P(Alpha, alphaMatcher, "alpha " + testing::DescribeMatcher<int>(alphaMatcher)) {
  const std::array<uint8_t, 4> px = {arg[0], arg[1], arg[2], arg[3]};
  *result_listener << "actual RGBA=" << FormatRgba(px);
  return testing::SafeMatcherCast<int>(alphaMatcher).Matches(static_cast<int>(px[3]));
}

/// Matches a fully transparent pixel (alpha == 0).
inline testing::Matcher<std::array<uint8_t, 4>> IsTransparent() {
  return Alpha(testing::Eq(0));
}

}  // namespace donner::svg::test
