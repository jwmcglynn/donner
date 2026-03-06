#pragma once

/// @file GoldenTestHelper.h
/// Test helper for comparing C++ rendered output against Rust golden PNG images.
/// Uses pixelmatch-cpp17 for pixel-level comparison.

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <pixelmatch/pixelmatch.h>

#include "PngDecoder.h"
#include "tiny_skia/Color.h"
#include "tiny_skia/Pixmap.h"

namespace tiny_skia::test_utils {

/// Returns the base path for golden test images.
/// Resolved relative to the Bazel runfiles directory.
inline std::string goldenImagePath(const std::string& relativePath) {
  // In Bazel tests, data deps are accessible relative to the test working dir
  // via the runfiles tree.  The golden images live under
  // third_party/tiny-skia/tests/images/.
  return "third_party/tiny-skia/tests/images/" + relativePath;
}

/// Premultiplies straight-alpha RGBA8 data in-place.
///   channel = (channel * alpha + 128 + ((channel * alpha + 128) >> 8)) >> 8
/// Equivalent to: round(channel * alpha / 255).
inline void premultiplyRgba(std::vector<std::uint8_t>& data) {
  for (std::size_t i = 0; i + 3 < data.size(); i += 4) {
    std::uint8_t a = data[i + 3];
    if (a == 0) {
      data[i + 0] = 0;
      data[i + 1] = 0;
      data[i + 2] = 0;
    } else if (a != 255) {
      for (int c = 0; c < 3; ++c) {
        std::uint32_t v = static_cast<std::uint32_t>(data[i + c]) * a;
        // Exact integer rounding: (v + 128 + ((v + 128) >> 8)) >> 8
        v = (v + 128 + ((v + 128) >> 8)) >> 8;
        data[i + c] = static_cast<std::uint8_t>(v);
      }
    }
  }
}

/// Loads a golden PNG image, premultiplies its RGBA data, and compares it
/// against the rendered Pixmap.  Returns the number of mismatched pixels.
/// A return of 0 means bit-exact parity.
inline int compareWithGolden(const Pixmap& rendered,
                             const std::string& goldenRelativePath) {
  auto golden = decodePng(goldenImagePath(goldenRelativePath));
  if (!golden.has_value()) {
    ADD_FAILURE() << "Failed to load golden PNG: " << goldenRelativePath;
    return -1;
  }

  if (golden->width != rendered.width() ||
      golden->height != rendered.height()) {
    ADD_FAILURE() << "Size mismatch: rendered=" << rendered.width() << "x"
                  << rendered.height()
                  << " golden=" << golden->width << "x" << golden->height
                  << " for " << goldenRelativePath;
    return -1;
  }

  // Premultiply the golden PNG data to match the Pixmap's premultiplied format.
  premultiplyRgba(golden->data);

  // Copy rendered data into a vector for pixelmatch API.
  auto renderedSpan = rendered.data();
  std::vector<std::uint8_t> renderedVec(renderedSpan.begin(), renderedSpan.end());

  // Diff output buffer (optional, but required by the API).
  std::vector<std::uint8_t> diffOutput(renderedVec.size());

  pixelmatch::Options opts;
  opts.threshold = 0.0f;  // Exact match.
  opts.includeAA = true;  // Don't skip anti-aliased pixels.

  return pixelmatch::pixelmatch(
      renderedVec, golden->data,
      diffOutput,
      rendered.width(), rendered.height(),
      rendered.width(),  // stride in pixels
      opts);
}

/// GTest assertion macro: asserts that the rendered Pixmap is pixel-identical
/// to the golden PNG.
#define EXPECT_GOLDEN_MATCH(pixmap, goldenPath)                              \
  do {                                                                       \
    int _diff = ::tiny_skia::test_utils::compareWithGolden(pixmap,           \
                                                           goldenPath);      \
    EXPECT_EQ(_diff, 0) << "Pixel mismatch with golden image: "             \
                        << goldenPath << " (" << _diff << " pixels differ)"; \
  } while (0)

#define ASSERT_GOLDEN_MATCH(pixmap, goldenPath)                              \
  do {                                                                       \
    int _diff = ::tiny_skia::test_utils::compareWithGolden(pixmap,           \
                                                           goldenPath);      \
    ASSERT_EQ(_diff, 0) << "Pixel mismatch with golden image: "             \
                        << goldenPath << " (" << _diff << " pixels differ)"; \
  } while (0)

}  // namespace tiny_skia::test_utils
