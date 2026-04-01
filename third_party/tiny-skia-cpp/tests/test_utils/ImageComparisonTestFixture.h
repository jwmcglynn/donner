#pragma once

/// @file ImageComparisonTestFixture.h
/// Image comparison test fixture for tiny-skia-cpp integration tests.
/// Ported from donner's ImageComparisonTestFixture, adapted for direct golden
/// image comparison with configurable per-pixel threshold.
///
/// On failure, saves actual/expected/diff PNGs to /tmp for debugging.

#include <gtest/gtest.h>
#include <pixelmatch/pixelmatch.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "PngDecoder.h"
#include "tiny_skia/Color.h"
#include "tiny_skia/Pixmap.h"

namespace tiny_skia::test_utils {

/// Default per-pixel color threshold (0.0–1.0).  5% tolerance absorbs AA
/// rounding differences between analytic AA and supersampled AA without
/// masking real bugs.
inline constexpr float kDefaultThreshold = 0.05f;

/// Parameters controlling image comparison tolerance.
struct ImageComparisonParams {
  /// Per-pixel color distance threshold (0.0 = exact, 1.0 = any difference ok).
  /// pixelmatch counts every pixel whose YIQ delta exceeds this value.
  /// The test passes only if that count is zero.
  float threshold = kDefaultThreshold;

  /// Whether to count anti-aliased pixel differences.  When false, pixelmatch
  /// ignores AA boundary pixels (useful when the AA algorithm changed but
  /// interior rendering is identical).
  bool includeAntiAliasing = false;

  // -- Factory helpers --------------------------------------------------------

  /// Construct with custom threshold.
  static ImageComparisonParams WithThreshold(float threshold) {
    ImageComparisonParams p;
    p.threshold = threshold;
    return p;
  }
};

/// Short alias used in test files.
using Params = ImageComparisonParams;

// ---------------------------------------------------------------------------
// PNG writing (for diagnostic output)
// ---------------------------------------------------------------------------

namespace detail {

/// Writes RGBA8 pixel data as a PNG file to the given path.
/// Data must be premultiplied or straight — caller decides.
inline bool writePng(const std::string& path, const std::uint8_t* rgba, std::uint32_t width,
                     std::uint32_t height) {
  const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
  const std::size_t rawSize = static_cast<std::size_t>(height) * (1 + rowBytes);
  std::vector<std::uint8_t> raw(rawSize);

  for (std::uint32_t y = 0; y < height; ++y) {
    auto* dst = raw.data() + static_cast<std::size_t>(y) * (1 + rowBytes);
    dst[0] = 0;  // Filter: None
    std::memcpy(dst + 1, rgba + static_cast<std::size_t>(y) * rowBytes, rowBytes);
  }

  uLongf compBound = compressBound(static_cast<uLong>(rawSize));
  std::vector<std::uint8_t> compressed(compBound);
  if (compress2(compressed.data(), &compBound, raw.data(), static_cast<uLong>(rawSize),
                Z_DEFAULT_COMPRESSION) != Z_OK) {
    return false;
  }
  compressed.resize(compBound);

  auto writeU32BE = [](std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
  };

  auto writeChunk = [&](std::ofstream& out, const char* type, const std::uint8_t* data,
                        std::uint32_t len) {
    std::uint8_t header[8];
    writeU32BE(header, len);
    std::memcpy(header + 4, type, 4);
    out.write(reinterpret_cast<const char*>(header), 8);
    auto crc = crc32(0L, reinterpret_cast<const Bytef*>(type), 4);
    if (len > 0) {
      out.write(reinterpret_cast<const char*>(data), len);
      crc = crc32(crc, data, len);
    }
    std::uint8_t crcBuf[4];
    writeU32BE(crcBuf, static_cast<std::uint32_t>(crc));
    out.write(reinterpret_cast<const char*>(crcBuf), 4);
  };

  std::ofstream file(path, std::ios::binary);
  if (!file) return false;

  constexpr std::uint8_t sig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  file.write(reinterpret_cast<const char*>(sig), 8);

  std::uint8_t ihdr[13];
  writeU32BE(ihdr, width);
  writeU32BE(ihdr + 4, height);
  ihdr[8] = 8;
  ihdr[9] = 6;
  ihdr[10] = 0;
  ihdr[11] = 0;
  ihdr[12] = 0;
  writeChunk(file, "IHDR", ihdr, 13);
  writeChunk(file, "IDAT", compressed.data(), static_cast<std::uint32_t>(compressed.size()));
  writeChunk(file, "IEND", nullptr, 0);
  return file.good();
}

/// Escape a golden path into a flat filename (replace / with _).
inline std::string escapeFilename(const std::string& goldenPath) {
  std::string result = goldenPath;
  for (char& c : result) {
    if (c == '/') c = '_';
  }
  return result;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Core comparison function
// ---------------------------------------------------------------------------

/// Returns the base path for golden test images (Bazel runfiles).
inline std::string goldenImagePath(const std::string& relativePath) {
  return "third_party/tiny-skia/tests/images/" + relativePath;
}

/// Premultiplies straight-alpha RGBA8 data in-place.
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
        v = (v + 128 + ((v + 128) >> 8)) >> 8;
        data[i + c] = static_cast<std::uint8_t>(v);
      }
    }
  }
}

/// Compares a rendered Pixmap against a golden PNG using the given params.
/// Returns the number of mismatched pixels (pixels exceeding the threshold).
/// Returns -1 on load/size errors (and adds a GTest failure).
///
/// On mismatch (return > 0), saves diagnostic PNGs:
///   /tmp/<escaped-golden>.png          — actual rendered output
///   /tmp/expected_<escaped-golden>.png — golden reference
///   /tmp/diff_<escaped-golden>.png     — pixelmatch diff visualization
inline int compareWithGolden(const Pixmap& rendered, const std::string& goldenRelativePath,
                             const ImageComparisonParams& params) {
  auto golden = decodePng(goldenImagePath(goldenRelativePath));
  if (!golden.has_value()) {
    ADD_FAILURE() << "Failed to load golden PNG: " << goldenRelativePath;
    return -1;
  }

  if (golden->width != rendered.width() || golden->height != rendered.height()) {
    ADD_FAILURE() << "Size mismatch: rendered=" << rendered.width() << "x" << rendered.height()
                  << " golden=" << golden->width << "x" << golden->height << " for "
                  << goldenRelativePath;
    return -1;
  }

  premultiplyRgba(golden->data);

  auto renderedSpan = rendered.data();
  std::vector<std::uint8_t> renderedVec(renderedSpan.begin(), renderedSpan.end());
  std::vector<std::uint8_t> diffOutput(renderedVec.size());

  pixelmatch::Options opts;
  opts.threshold = params.threshold;

  int mismatchedPixels =
      pixelmatch::pixelmatch(renderedVec, golden->data, diffOutput, rendered.width(),
                             rendered.height(), rendered.width(), opts);

  // On any mismatch, dump diagnostic images.
  if (mismatchedPixels > 0) {
    std::string escaped = detail::escapeFilename(goldenRelativePath);
    std::string actualPath = "/tmp/" + escaped;
    std::string expectedPath = "/tmp/expected_" + escaped;
    std::string diffPath = "/tmp/diff_" + escaped;

    detail::writePng(actualPath, renderedVec.data(), rendered.width(), rendered.height());
    detail::writePng(expectedPath, golden->data.data(), golden->width, golden->height);
    detail::writePng(diffPath, diffOutput.data(), rendered.width(), rendered.height());

    std::cerr << "  Actual:   " << actualPath << "\n"
              << "  Expected: " << expectedPath << "\n"
              << "  Diff:     " << diffPath << "\n";
  }

  return mismatchedPixels;
}

// ---------------------------------------------------------------------------
// GTest assertion macros
// ---------------------------------------------------------------------------

/// Assert golden match using given params.  Passes only if zero pixels exceed
/// the per-pixel threshold.
#define EXPECT_GOLDEN_MATCH_WITH_PARAMS(pixmap, goldenPath, params)                            \
  do {                                                                                         \
    const auto& _params = (params);                                                            \
    int _diff = ::tiny_skia::test_utils::compareWithGolden(pixmap, goldenPath, _params);       \
    EXPECT_EQ(_diff, 0) << "Pixel mismatch with golden image: " << goldenPath << " (" << _diff \
                        << " pixels differ, threshold=" << _params.threshold << ")";           \
  } while (0)

/// Assert golden match using default params (kDefaultThreshold).
#define EXPECT_GOLDEN_MATCH(pixmap, goldenPath)       \
  EXPECT_GOLDEN_MATCH_WITH_PARAMS(pixmap, goldenPath, \
                                  ::tiny_skia::test_utils::ImageComparisonParams{})

/// Hard-fail variants.
#define ASSERT_GOLDEN_MATCH_WITH_PARAMS(pixmap, goldenPath, params)                            \
  do {                                                                                         \
    const auto& _params = (params);                                                            \
    int _diff = ::tiny_skia::test_utils::compareWithGolden(pixmap, goldenPath, _params);       \
    ASSERT_EQ(_diff, 0) << "Pixel mismatch with golden image: " << goldenPath << " (" << _diff \
                        << " pixels differ, threshold=" << _params.threshold << ")";           \
  } while (0)

#define ASSERT_GOLDEN_MATCH(pixmap, goldenPath)       \
  ASSERT_GOLDEN_MATCH_WITH_PARAMS(pixmap, goldenPath, \
                                  ::tiny_skia::test_utils::ImageComparisonParams{})

}  // namespace tiny_skia::test_utils
