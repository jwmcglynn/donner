/// @file
#pragma once

#include <gtest/gtest.h>

#include <filesystem>

#include "donner/svg/SVGDocument.h"

namespace donner::svg {

/**
 * @brief Default maximum number of mismatched pixels allowed in image comparisons.
 *
 * Circle rendering is slightly different since Donner uses four custom curves instead of arcTo.
 * This constant allows a small number of mismatched pixels to accommodate these differences.
 */
static constexpr int kDefaultMismatchedPixels = 100;

/**
 * @brief Default threshold for pixel differences in image comparisons.
 *
 * For most tests, a threshold of 0.01 (1%) is sufficient. Some specific tests might require a
 * larger threshold due to subtle anti-aliasing differences. For example:
 * - `a_transform_007` might need up to 0.05.
 * - `e_line_001` might need up to 0.02.
 */
static constexpr float kDefaultThreshold = 0.01f;

/**
 * @brief Parameters for controlling image comparison tests.
 *
 * This struct allows customization of various aspects of the image comparison process,
 * such as error thresholds, skipping tests, and overriding golden image filenames.
 */
struct ImageComparisonParams {
  /// Maximum allowed difference per pixel (0.0 to 1.0).
  float threshold = kDefaultThreshold;
  /// Maximum number of pixels that can exceed the threshold.
  int maxMismatchedPixels = kDefaultMismatchedPixels;
  /// If true, skip this test case.
  bool skip = false;
  /// If true, save a .skp file for debugging when a test fails.
  bool saveDebugSkpOnFailure = true;
  /// If true, allow updating golden images via an environment variable.
  bool updateGoldenFromEnv = false;
  /// Optional canvas size override, which determines the size of the rendered image.
  std::optional<Vector2i> canvasSize;
  /// Optional filename to use for the golden image, overriding the default.
  std::string_view overrideGoldenFilename;

  /**
   * @brief Creates parameters to skip a test.
   * @return ImageComparisonParams configured to skip.
   */
  static ImageComparisonParams Skip() {
    ImageComparisonParams result;
    result.skip = true;
    return result;
  }

  /**
   * @brief Creates parameters with a specific threshold and maximum mismatched pixels.
   *
   * @param threshold The per-pixel difference threshold.
   * @param maxMismatchedPixels The maximum number of pixels allowed to mismatch.
   * @return ImageComparisonParams configured with the specified thresholds.
   */
  static ImageComparisonParams WithThreshold(float threshold,
                                             int maxMismatchedPixels = kDefaultMismatchedPixels) {
    ImageComparisonParams result;
    result.threshold = threshold;
    result.maxMismatchedPixels = maxMismatchedPixels;
    return result;
  }

  /**
   * @brief Creates parameters with an overridden golden image filename.
   *
   * @param filename The filename to use for the golden image.
   * @return ImageComparisonParams configured with the golden override.
   */
  static ImageComparisonParams WithGoldenOverride(std::string_view filename) {
    ImageComparisonParams result;
    result.overrideGoldenFilename = filename;
    return result;
  }

  /**
   * @brief Disables saving of .skp files on test failure.
   * @return Reference to this ImageComparisonParams object.
   */
  ImageComparisonParams& disableDebugSkpOnFailure() {
    saveDebugSkpOnFailure = false;
    return *this;
  }

  /**
   * @brief Enables updating golden images based on an environment variable.
   * @return Reference to this ImageComparisonParams object.
   */
  ImageComparisonParams& enableGoldenUpdateFromEnv() {
    updateGoldenFromEnv = true;
    return *this;
  }

  /**
   * @brief Sets a custom canvas size for rendering.
   *
   * @param width The width of the canvas.
   * @param height The height of the canvas.
   * @return Reference to this ImageComparisonParams object.
   */
  ImageComparisonParams& setCanvasSize(int width, int height) {
    canvasSize = Vector2i(width, height);
    return *this;
  }
};

/**
 * @brief Represents a single test case for image comparison.
 *
 * Contains the path to the SVG file to be tested and the parameters for the comparison.
 */
struct ImageComparisonTestcase {
  std::filesystem::path svgFilename;  //!< Path to the SVG file for this test case.
  ImageComparisonParams params;       //!< Parameters for this specific test case.

  /**
   * @brief Comparison operator for sorting test cases by filename.
   */
  friend bool operator<(const ImageComparisonTestcase& lhs, const ImageComparisonTestcase& rhs) {
    return lhs.svgFilename < rhs.svgFilename;
  }

  /**
   * @brief Stream insertion operator for printing the test case (prints the SVG filename).
   */
  friend std::ostream& operator<<(std::ostream& os, const ImageComparisonTestcase& rhs) {
    return os << rhs.svgFilename.string();
  }
};

/**
 * @brief Generates a test name from the SVG filename in the test parameter info.
 *
 * This is used by GTest to create human-readable test names for parameterized tests.
 *
 * @param info Test parameter information containing the \ref ImageComparisonTestcase.
 * @return A string suitable for use as a test name.
 */
std::string TestNameFromFilename(const testing::TestParamInfo<ImageComparisonTestcase>& info);

/**
 * @brief A Google Test fixture for tests that compare rendered SVG output against golden images.
 *
 * This fixture is parameterized with \ref ImageComparisonTestcase, allowing multiple SVG files
 * to be tested with different comparison parameters. It provides helper methods for loading
 * SVG documents and performing the rendering and comparison.
 */
class ImageComparisonTestFixture : public testing::TestWithParam<ImageComparisonTestcase> {
protected:
  /**
   * @brief Loads an SVG document from the given filename.
   *
   * @param filename The path to the SVG file.
   * @param resourceDir Optional path to a directory containing resources (e.g., external images)
   *                    referenced by the SVG.
   * @return The loaded \ref SVGDocument.
   */
  SVGDocument loadSVG(const char* filename,
                      const std::optional<std::filesystem::path>& resourceDir = std::nullopt);

  /**
   * @brief Renders the given SVG document and compares it against a golden image.
   *
   * Uses default comparison parameters.
   *
   * @param document The \ref SVGDocument to render.
   * @param svgFilename The original path of the SVG file (used for naming output files).
   * @param goldenImageFilename The path to the golden image file.
   */
  void renderAndCompare(SVGDocument& document, const std::filesystem::path& svgFilename,
                        const char* goldenImageFilename);

  /**
   * @brief Renders the given SVG document and compares it against a golden image using specified
   * parameters.
   *
   * @param document The \ref SVGDocument to render.
   * @param svgFilename The original path of the SVG file (used for naming output files).
   * @param goldenImageFilename The path to the golden image file.
   * @param params The \ref ImageComparisonParams to use for the comparison.
   */
  void renderAndCompare(SVGDocument& document, const std::filesystem::path& svgFilename,
                        const char* goldenImageFilename, const ImageComparisonParams& params);
};

}  // namespace donner::svg
