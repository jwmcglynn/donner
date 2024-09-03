#pragma once

#include <gtest/gtest.h>

#include <filesystem>

#include "donner/svg/SVGDocument.h"

namespace donner::svg {

// Circle rendering is slightly different since Donner uses four custom curves instead of arcTo.
// Allow a small number of mismatched pixels to accomodate.
static constexpr int kDefaultMismatchedPixels = 100;

// For most tests, a threshold of 0.01 is sufficient, but some specific tests have slightly
// different anti-aliasing artifacts, so a larger threshold is required:
// - a_transform_007 - 0.05 to pass
// - e_line_001 - 0.02 to pass
static constexpr float kDefaultThreshold = 0.01f;

struct ImageComparisonParams {
  float threshold = kDefaultThreshold;
  int maxMismatchedPixels = kDefaultMismatchedPixels;
  bool skip = false;
  bool saveDebugSkpOnFailure = true;

  static ImageComparisonParams Skip() {
    ImageComparisonParams result;
    result.skip = true;
    return result;
  }

  static ImageComparisonParams WithThreshold(float threshold,
                                             int maxMismatchedPixels = kDefaultMismatchedPixels) {
    ImageComparisonParams result;
    result.threshold = threshold;
    result.maxMismatchedPixels = maxMismatchedPixels;
    return result;
  }

  ImageComparisonParams& disableDebugSkpOnFailure() {
    saveDebugSkpOnFailure = false;
    return *this;
  }
};

struct ImageComparisonTestcase {
  std::filesystem::path svgFilename;
  ImageComparisonParams params;

  friend bool operator<(const ImageComparisonTestcase& lhs, const ImageComparisonTestcase& rhs) {
    return lhs.svgFilename < rhs.svgFilename;
  }

  friend std::ostream& operator<<(std::ostream& os, const ImageComparisonTestcase& rhs) {
    return os << rhs.svgFilename.string();
  }
};

std::string TestNameFromFilename(const testing::TestParamInfo<ImageComparisonTestcase>& info);

class ImageComparisonTestFixture : public testing::TestWithParam<ImageComparisonTestcase> {
protected:
  SVGDocument loadSVG(const char* filename,
                      const std::optional<std::filesystem::path>& resourceDir = std::nullopt);

  void renderAndCompare(SVGDocument& document, const std::filesystem::path& svgFilename,
                        const char* goldenImageFilename);
};

}  // namespace donner::svg
