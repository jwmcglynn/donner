/// @file
#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/TerminalImageViewer.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"

namespace donner::svg {

/**
 * @brief Which renderer output a parameterized image-comparison run compares.
 *
 * The geode-enabled build runs each test under multiple modes (the matrix from
 * docs/design_docs/0017 §Phase 4b); the pure-CPU build runs `TinyGolden` only.
 */
enum class ComparisonMode : uint8_t {
  /// tiny-skia render vs the committed resvg golden (ground truth; every build).
  TinyGolden,
  /// geode render vs the committed golden (today's geode behavior; geode build).
  GeodeGolden,
  /// geode render vs an in-process tiny-skia render, golden ignored.
  GeodeTinyParity,
};

/**
 * @brief The renderer backend a comparison mode renders with.
 *
 * @param mode The comparison mode.
 * @return The backend used to produce the "actual" image for the mode.
 */
constexpr RendererBackend BackendForMode(ComparisonMode mode) {
  return mode == ComparisonMode::TinyGolden ? RendererBackend::TinySkia : RendererBackend::Geode;
}

/**
 * @brief Short suffix appended to a test name to disambiguate modes.
 *
 * @param mode The comparison mode.
 * @return Suffix string (e.g. "TinyGolden").
 */
std::string_view ComparisonModeName(ComparisonMode mode);

/**
 * @brief The comparison modes active for the current build.
 *
 * Pure-CPU build: `{ TinyGolden }`. Geode-enabled build:
 * `{ TinyGolden, GeodeGolden, GeodeTinyParity }`.
 *
 * @return The active modes, in run order.
 */
const std::vector<ComparisonMode>& ActiveComparisonModes();

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
static constexpr float kDefaultThreshold = 0.02f;

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
  /// Override for maxMismatchedPixels when running without HarfBuzz text shaping
  /// (DONNER_TEXT_FULL not defined). -1 means use maxMismatchedPixels for all configs.
  int simpleTextMaxMismatchedPixels = -1;
  /// If true, skip this test when running without HarfBuzz (simple text).
  bool skipSimpleText = false;
  /// If true, count anti-aliased pixels as mismatches instead of suppressing them.
  bool includeAntiAliasing = false;
  /// If true, skip this test case.
  bool skip = false;
  /// If true, allow updating golden images via an environment variable.
  bool updateGoldenFromEnv = false;
  /// If true, emit a terminal preview grid when comparisons fail.
  bool showTerminalPreview = true;
  /// Optional canvas size override, which determines the size of the rendered image.
  std::optional<Vector2i> canvasSize;
  /// Optional filename to use for the golden image, overriding the default.
  std::string_view overrideGoldenFilename;
  /// Optional Geode-specific golden image, used ONLY for the `GeodeGolden`
  /// comparison mode. Captures Geode's (correct) output when it legitimately
  /// differs from the shared resvg/tiny golden by a genuine sub-pixel analytic
  /// difference that no structural fix can close (e.g. amplified 8-bit
  /// filter-intermediate precision, or GPU bilinear resampling of a feImage
  /// raster). The TinyGolden mode still compares against the shared golden.
  std::string_view geodeOverrideGoldenFilename;
  /// If false, skip the test when the active backend is TinySkia.
  bool allowTinySkia = true;
  /// If false, skip the test when the active backend is Geode.
  bool allowGeode = true;
  /// Bitmask of required backend features, built from \ref RendererBackendFeatureMask.
  uint32_t requiredFeatures = 0;
  /// Human-readable reason used when backend restrictions cause a skip.
  std::string_view backendRequirementReason;
  /// If true, render but skip the pixel comparison. Used for tests where the output
  /// is implementation-defined or UB, but we still want to verify rendering stability.
  bool renderOnly = false;
  /// Human-readable reason attached to Skip / RenderOnly / WithThreshold overrides.
  /// Surfaced in test skip messages and failure output — prefer this over trailing
  /// `// comments` so the reason is discoverable from test logs.
  std::string_view reason;

  /// If true, skip only the `GeodeTinyParity` instance for this test.
  bool disableGeodeTinyParity = false;

  /**
   * @brief Creates parameters to skip a test.
   * @param reason Human-readable explanation, shown in the skip message.
   * @return ImageComparisonParams configured to skip.
   */
  static ImageComparisonParams Skip(std::string_view reason = std::string_view()) {
    ImageComparisonParams result;
    result.skip = true;
    result.reason = reason;
    return result;
  }

  /**
   * @brief Creates parameters that render the test but skip pixel comparison.
   *
   * Used for tests where the output is implementation-defined or has expected
   * variance, but we want to verify that rendering completes without crashing.
   * @param reason Human-readable explanation, shown in the test log.
   * @return ImageComparisonParams configured for render-only mode.
   */
  static ImageComparisonParams RenderOnly(std::string_view reason = std::string_view()) {
    ImageComparisonParams result;
    result.renderOnly = true;
    result.reason = reason;
    return result;
  }

  /**
   * @brief Creates parameters with a specific threshold and maximum mismatched pixels.
   *
   * @param threshold The per-pixel difference threshold.
   * @param maxMismatchedPixels The maximum number of pixels allowed to mismatch.
   * @param reason Human-readable explanation for why the threshold is raised.
   * @return ImageComparisonParams configured with the specified thresholds.
   */
  static ImageComparisonParams WithThreshold(float threshold,
                                             int maxMismatchedPixels = kDefaultMismatchedPixels,
                                             std::string_view reason = std::string_view()) {
    ImageComparisonParams result;
    result.threshold = threshold;
    result.maxMismatchedPixels = maxMismatchedPixels;
    result.reason = reason;
    return result;
  }

  /**
   * @brief Creates parameters with an overridden golden image filename.
   *
   * @param filename The filename to use for the golden image.
   * @param threshold Optional per-pixel difference threshold to use with the override.
   * @param reason Human-readable explanation for why the override is needed.
   * @return ImageComparisonParams configured with the golden override.
   */
  static ImageComparisonParams WithGoldenOverride(std::string_view filename,
                                                  float threshold = kDefaultThreshold,
                                                  std::string_view reason = std::string_view()) {
    ImageComparisonParams result;
    result.overrideGoldenFilename = filename;
    result.threshold = threshold;
    result.reason = reason;
    return result;
  }

  /**
   * @brief Attach a human-readable reason to a chained Params() expression.
   *
   * Use when you're starting from `Params()` (or any other builder) and want
   * to record why the override exists. Surfaces in skip/failure messages.
   *
   * @param text Reason text.
   * @return Reference to this ImageComparisonParams object.
   */
  ImageComparisonParams& withReason(std::string_view text) {
    reason = text;
    return *this;
  }

  /**
   * @brief Use a Geode-specific golden for the `GeodeGolden` comparison mode.
   *
   * The shared golden still gates `TinyGolden`. Use only when Geode's output is
   * verified correct and differs from the shared reference by a genuine
   * sub-pixel analytic difference (never to absorb a structural bug).
   *
   * @param filename Path (under the workspace) to the Geode golden PNG.
   * @param text Justification for the per-backend golden.
   * @return Reference to this ImageComparisonParams object.
   */
  ImageComparisonParams& withGeodeGoldenOverride(std::string_view filename,
                                                 std::string_view text = std::string_view()) {
    geodeOverrideGoldenFilename = filename;
    if (!text.empty()) {
      reason = text;
    }
    return *this;
  }

  /**
   * @brief Counts anti-aliased pixel differences as mismatches.
   * @return Reference to this ImageComparisonParams object.
   */
  ImageComparisonParams& includeAntiAliasingDifferences() {
    includeAntiAliasing = true;
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

  /**
   * @brief Disables one backend for this test case.
   *
   * @param backend The backend to disable.
   * @param reason Optional human-readable skip reason.
   * @return Reference to this ImageComparisonParams object.
   */
  ImageComparisonParams& disableBackend(RendererBackend backend,
                                        std::string_view reason = std::string_view()) {
    switch (backend) {
      case RendererBackend::TinySkia: allowTinySkia = false; break;
      case RendererBackend::Geode: allowGeode = false; break;
    }

    if (!reason.empty()) {
      backendRequirementReason = reason;
    }

    return *this;
  }

  /**
   * @brief Requires a renderer feature for this test case.
   *
   * @param feature The feature to require.
   * @param reason Optional human-readable skip reason.
   * @return Reference to this ImageComparisonParams object.
   */
  ImageComparisonParams& requireFeature(RendererBackendFeature feature,
                                        std::string_view reason = std::string_view()) {
    requiredFeatures |= RendererBackendFeatureMask(feature);
    if (!reason.empty()) {
      backendRequirementReason = reason;
    }

    return *this;
  }

  /**
   * @brief Sets a separate maxMismatchedPixels for simple text (without HarfBuzz shaping).
   *
   * When DONNER_TEXT_FULL is not defined, this value is used instead of maxMismatchedPixels.
   * This allows tests that require text shaping (combining marks, ligatures, etc.) to have
   * a looser threshold for simple text while keeping a strict threshold for HarfBuzz.
   *
   * @param pixels The max mismatched pixels for simple text.
   * @return Reference to this ImageComparisonParams object.
   */
  ImageComparisonParams& withSimpleTextMaxPixels(int pixels) {
    simpleTextMaxMismatchedPixels = pixels;
    return *this;
  }

  /**
   * @brief Sets the max mismatched pixels for this test case.
   *
   * @param pixels The max mismatched pixels.
   * @return Reference to this ImageComparisonParams object.
   */
  ImageComparisonParams& withMaxPixelsDifferent(int pixels) {
    maxMismatchedPixels = pixels;
    return *this;
  }

  /**
   * @brief Skips only the `GeodeTinyParity` instance for this test.
   *
   * Use sparingly for a known geode-vs-tiny divergence that should not disable
   * the normal golden comparisons.
   *
   * @param reason Short tracking reason.
   * @return Reference to this ImageComparisonParams object.
   */
  ImageComparisonParams& disableGeodeParity(std::string_view reason = std::string_view()) {
    disableGeodeTinyParity = true;
    if (!reason.empty()) {
      this->reason = reason;
    }
    return *this;
  }

  /**
   * @brief Skip this test entirely when running without HarfBuzz (simple text).
   *
   * Use this for tests that require text-full features (e.g., color emoji fonts that
   * can't be loaded by stb_truetype at all).
   *
   * @return Reference to this ImageComparisonParams object.
   */
  ImageComparisonParams& onlyTextFull() {
    skipSimpleText = true;
    return *this;
  }

  /**
   * @brief Returns the effective maxMismatchedPixels for the active text config.
   */
  int effectiveMaxMismatchedPixels() const {
#ifdef DONNER_TEXT_FULL
    return maxMismatchedPixels;
#else
    return simpleTextMaxMismatchedPixels >= 0 ? simpleTextMaxMismatchedPixels : maxMismatchedPixels;
#endif
  }

  /**
   * @brief Returns true if this test should be skipped for the active text config.
   */
  bool shouldSkip() const {
    if (skip) {
      return true;
    }
#ifndef DONNER_TEXT_FULL
    if (skipSimpleText) {
      return true;
    }
#endif
    return false;
  }
};

/**
 * @brief Represents a single test case for image comparison.
 *
 * Contains the path to the SVG file to be tested and the parameters for the comparison.
 */
struct ImageComparisonTestcase {
  std::filesystem::path svgFilename;  //!< Path to the SVG file for this test case.
  ImageComparisonParams params;       //!< Parameters for this test case.

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
 * @brief The parameterized fixture's GTest parameter: a test case + its mode.
 *
 * `INSTANTIATE_TEST_SUITE_P` sites build this via
 * `Combine(ValuesIn(getTestsInCategory(...)), ValuesIn(ActiveComparisonModes()))`.
 */
using ImageComparisonTestParam = std::tuple<ImageComparisonTestcase, ComparisonMode>;

/**
 * @brief Generates a test name from the SVG filename (and mode when >1 active).
 *
 * The mode suffix is emitted only when the build runs more than one comparison
 * mode, so single-mode (CPU) builds keep the exact historical test names that
 * `--test_filter` patterns and golden tooling rely on.
 *
 * @param info Test parameter information containing the test case + mode.
 * @return A string suitable for use as a test name.
 */
std::string TestNameFromFilename(const testing::TestParamInfo<ImageComparisonTestParam>& info);

/**
 * @brief Asserts two live renderer bitmaps are pixel-for-pixel identical.
 *
 * Strict identity pixelmatch (threshold 0, AA included, 0 mismatches allowed) —
 * the renderer suite's shared bitmap-to-bitmap comparator (so tests don't roll a
 * private one; mirrors the editor's `CompareBitmapToBitmap`). On mismatch, adds a
 * gtest failure and writes `actual_/expected_/diff_<label>.png` to
 * `$TEST_UNDECLARED_OUTPUTS_DIR` (else the temp dir). Use when the ground truth
 * is another render (e.g. a re-draw idempotency check), not a committed golden.
 *
 * @param actual The bitmap under test.
 * @param expected The reference bitmap.
 * @param label Short identifier for log output and dumped PNG names.
 */
void ExpectBitmapsIdentical(const RendererBitmap& actual, const RendererBitmap& expected,
                            std::string_view label);

/**
 * @brief Terminal preview configuration derived from the environment.
 */
struct TerminalPreviewConfig {
  TerminalPixelMode pixelMode = TerminalPixelMode::kQuarterPixel;  //!< Pixel mode to use.
  int terminalWidth = 120;                                         //!< Maximum terminal width.
};

/**
 * @brief Reads preview configuration and gating flags from the environment.
 */
std::optional<TerminalPreviewConfig> PreviewConfigFromEnv(const ImageComparisonParams& params);

/**
 * @brief Render a 2x2 terminal preview grid for testing.
 */
std::string RenderTerminalComparisonGridForTesting(
    const TerminalImageView& actual, const TerminalImageView& expected,
    const TerminalImageView& diff, int maxTerminalWidth, TerminalPixelMode pixelMode,
    const TerminalImageViewerConfig& viewerConfig = {});

/**
 * @brief A Google Test fixture for tests that compare rendered SVG output against golden images.
 *
 * This fixture is parameterized with \ref ImageComparisonTestcase, allowing multiple SVG files
 * to be tested with different comparison parameters. It provides helper methods for loading
 * SVG documents and performing the rendering and comparison.
 */
class ImageComparisonTestFixture : public testing::TestWithParam<ImageComparisonTestParam> {
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
   * Uses the test case's effective parameters and comparison mode from the GTest
   * parameter. Only valid for parameterized (`TEST_P`) tests.
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
   * The `mode` selects which backend renders the "actual" image; it defaults to
   * the build's primary backend (TinySkia on CPU, Geode on the geode build) so
   * non-parameterized `TEST_F` callers keep today's behavior.
   *
   * @param document The \ref SVGDocument to render.
   * @param svgFilename The original path of the SVG file (used for naming output files).
   * @param goldenImageFilename The path to the golden image file.
   * @param params The \ref ImageComparisonParams to use for the comparison.
   * @param mode The comparison mode (backend) to render with.
   */
  void renderAndCompare(SVGDocument& document, const std::filesystem::path& svgFilename,
                        const char* goldenImageFilename, const ImageComparisonParams& params,
                        ComparisonMode mode = ActiveRendererBackend() == RendererBackend::Geode
                                                  ? ComparisonMode::GeodeGolden
                                                  : ComparisonMode::TinyGolden);
};

}  // namespace donner::svg
