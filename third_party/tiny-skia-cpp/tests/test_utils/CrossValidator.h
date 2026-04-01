#pragma once

/// @file CrossValidator.h
/// Test helpers for comparing C++ rendered output against Rust rendered output
/// at runtime (live cross-validation).
///
/// Uses pixelmatch-cpp17 for pixel-level comparison – same library and
/// threshold settings as the golden-image comparisons.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <pixelmatch/pixelmatch.h>

#include "RustReference.h"
#include "tiny_skia/Pixmap.h"

namespace tiny_skia::test_utils {

/// Compares a C++ Pixmap against a Rust-reference Pixmap.
/// Both must contain premultiplied RGBA8 data of the same dimensions.
/// Returns the number of mismatched pixels (0 = bit-exact parity).
inline int comparePixmaps(const Pixmap& cpp, const rustRef::Pixmap& rust) {
  if (cpp.width() != rust.width() || cpp.height() != rust.height()) {
    ADD_FAILURE() << "Size mismatch: C++=" << cpp.width() << "x"
                  << cpp.height() << " Rust=" << rust.width() << "x"
                  << rust.height();
    return -1;
  }

  auto cppData = cpp.data();
  auto rustData = rust.data();

  std::vector<std::uint8_t> cppVec(cppData.begin(), cppData.end());
  std::vector<std::uint8_t> rustVec(rustData.begin(), rustData.end());
  std::vector<std::uint8_t> diffOutput(cppVec.size());

  pixelmatch::Options opts;
  opts.threshold = 0.05f;  // 5% tolerance for AA rounding differences.
  opts.includeAA = false;  // Skip anti-aliased boundary pixels.

  return pixelmatch::pixelmatch(cppVec, rustVec, diffOutput, cpp.width(),
                                cpp.height(), cpp.width(), opts);
}

/// Compares two raw premultiplied RGBA8 buffers of the same dimensions.
inline int compareRawPixels(std::span<const std::uint8_t> cppData,
                            std::span<const std::uint8_t> rustData,
                            std::uint32_t width, std::uint32_t height) {
  if (cppData.size() != rustData.size()) {
    ADD_FAILURE() << "Data size mismatch: C++=" << cppData.size()
                  << " Rust=" << rustData.size();
    return -1;
  }

  std::vector<std::uint8_t> cppVec(cppData.begin(), cppData.end());
  std::vector<std::uint8_t> rustVec(rustData.begin(), rustData.end());
  std::vector<std::uint8_t> diffOutput(cppVec.size());

  pixelmatch::Options opts;
  opts.threshold = 0.05f;
  opts.includeAA = false;

  return pixelmatch::pixelmatch(cppVec, rustVec, diffOutput, width, height,
                                width, opts);
}

}  // namespace tiny_skia::test_utils

// ---------------------------------------------------------------------------
// Assertion macros
// ---------------------------------------------------------------------------

/// Expects that the C++ Pixmap matches the Rust-reference Pixmap exactly.
#define EXPECT_CROSS_MATCH(cppPixmap, rustPixmap)                             \
  do {                                                                        \
    int _diff =                                                               \
        ::tiny_skia::test_utils::comparePixmaps(cppPixmap, rustPixmap);       \
    EXPECT_EQ(_diff, 0) << "C++ vs Rust pixel mismatch: " << _diff           \
                        << " pixels differ";                                  \
  } while (0)

/// Asserts (fatal) that the C++ Pixmap matches the Rust-reference Pixmap.
#define ASSERT_CROSS_MATCH(cppPixmap, rustPixmap)                             \
  do {                                                                        \
    int _diff =                                                               \
        ::tiny_skia::test_utils::comparePixmaps(cppPixmap, rustPixmap);       \
    ASSERT_EQ(_diff, 0) << "C++ vs Rust pixel mismatch: " << _diff           \
                        << " pixels differ";                                  \
  } while (0)
