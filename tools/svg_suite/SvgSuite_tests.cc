#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "tools/svg_suite/StaticSuite.h"

namespace donner::svg_suite {
namespace {
using testing::Eq;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::Not;

Image RedImage() {
  return {1, 1, 1, {255, 0, 0, 255}};
}

ImageView View(const Image& image) {
  return {image.width, image.height, image.strideInPixels, image.data};
}

Image WithRowPadding(const Image& image, size_t stride, uint8_t padding) {
  Image result{image.width, image.height, stride,
               std::vector<uint8_t>(stride * image.height * 4, padding)};
  for (int row = 0; row < image.height; ++row) {
    std::copy_n(image.data.begin() + row * image.strideInPixels * 4, image.width * 4,
                result.data.begin() + row * stride * 4);
  }
  return result;
}

Image VerticalEdgeImage() {
  Image image{3, 3, 3, {}};
  for (int row = 0; row < 3; ++row) {
    image.data.insert(image.data.end(), {0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255});
  }
  return image;
}

class SvgSuiteTest : public testing::Test {
protected:
  void SetUp() override {
    static std::atomic<unsigned> serial{0};
    const char* base = std::getenv("TEST_TMPDIR");
    directory_ = (base ? std::filesystem::path(base) : std::filesystem::temp_directory_path()) /
                 ("svg-suite-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                  "-" + std::to_string(serial++));
    std::filesystem::create_directories(directory_ / "corpus");
  }

  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

  void addCase(std::string_view name) {
    const auto path = directory_ / "corpus" / name;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream svg(path);
    svg << "<svg xmlns='http://www.w3.org/2000/svg' width='1' height='1'>"
           "<rect width='1' height='1' fill='red'/></svg>";
    svg.close();
    auto png = path;
    png.replace_extension(".png");
    const Image image = RedImage();
    ASSERT_THAT(WritePng(png, View(image)), IsEmpty());
  }

  DiscoveryResult discover() const { return DiscoverCases({{"extension", directory_ / "corpus"}}); }

  std::filesystem::path directory_;
};

TEST_F(SvgSuiteTest, InvalidUtf8PathValueIsRejectedBeforeFilesystemAccess) {
  const auto path = directory_ / std::string("invalid-").append(1, char(0xFF));
  const auto result = DiscoverCases({{"fixture", path}});
  EXPECT_TRUE(result.cases.empty());
  EXPECT_THAT(result.error, HasSubstr("UTF-8"));
}

TEST_F(SvgSuiteTest, ManualCaseInvalidPathNeverInvokesCallback) {
  addCase("valid.svg");
  auto discovery = discover();
  ASSERT_TRUE(discovery.error.empty());
  auto test = discovery.cases.front();
  test.expectedPath = directory_ / std::string("bad-").append(1, char(0xFF));
  bool called = false;
  const auto result = RunCase(
      test,
      [&called](const StaticCase&, int, int) {
        called = true;
        return ImageResult{RedImage(), {}};
      },
      directory_ / "output");
  EXPECT_FALSE(called);
  EXPECT_FALSE(result.passed);
  EXPECT_THAT(result.error, HasSubstr("UTF-8"));
  EXPECT_FALSE(result.mismatchedPixels);
}

TEST_F(SvgSuiteTest, UnicodeDiscoveryKeepsDistinctByteIdentities) {
  addCase("unicode-\xC3\xA9.svg");
  addCase("replacement-\xEF\xBF\xBD.svg");
  const auto discovery = discover();
  ASSERT_THAT(discovery.error, IsEmpty());
  ASSERT_EQ(discovery.cases.size(), 2u);
  EXPECT_NE(discovery.cases[0].id, discovery.cases[1].id);
  for (const auto& test : discovery.cases) {
    const auto result = RunCase(
        test, [](const StaticCase&, int, int) { return ImageResult{RedImage(), {}}; },
        directory_ / "output");
    EXPECT_TRUE(result.passed) << result.error;
  }
}

#if defined(__linux__)
TEST_F(SvgSuiteTest, InvalidNativeDirectoryAndSymlinkTargetAreRejected) {
  addCase("valid.svg");
  const auto invalid = directory_ / "corpus" / std::string("bad-").append(1, char(0xFF));
  std::filesystem::create_directory(invalid);
  EXPECT_THAT(discover().error, HasSubstr("UTF-8"));
  std::filesystem::remove(invalid);
  const auto target = directory_ / std::string("target-").append(1, char(0xFF));
  std::ofstream(target) << "data";
  std::filesystem::create_symlink(target, directory_ / "corpus/valid-link");
  EXPECT_THAT(discover().error, HasSubstr("UTF-8"));
}
#endif

TEST(ImageCompareTest, StrictComparisonDetectsWrongColor) {
  const Image expected = RedImage();
  Image actual = expected;
  actual.data = {0, 0, 255, 255};
  const ComparisonResult result = CompareImages(View(expected), View(actual));
  EXPECT_THAT(result.error, IsEmpty());
  EXPECT_THAT(result.mismatchedPixels, Eq(std::optional<int>(1)));
  EXPECT_THAT(result.passed, Eq(false));
  ASSERT_THAT(result.diff.has_value(), Eq(true));
  EXPECT_THAT(result.diff->data.size(), Eq(4u));
}

TEST(ImageCompareTest, IdentityPassesAndRetainsDiff) {
  const Image image = RedImage();
  const auto result = CompareImages(View(image), View(image));
  EXPECT_THAT(result.error, IsEmpty());
  EXPECT_THAT(result.mismatchedPixels, Eq(std::optional<int>(0)));
  EXPECT_THAT(result.passed, Eq(true));
  EXPECT_THAT(result.diff.has_value(), Eq(true));
}

TEST(ImageCompareTest, IndependentRowStridesIgnorePoisonedPadding) {
  const Image tight = VerticalEdgeImage();
  for (const auto strides : {std::array<size_t, 2>{3, 5}, {5, 3}, {4, 6}, {6, 4}, {5, 5}}) {
    SCOPED_TRACE(testing::Message() << strides[0] << ", " << strides[1]);
    const Image expected = WithRowPadding(tight, strides[0], 0xA5);
    const Image actual = WithRowPadding(tight, strides[1], 0x5A);
    const auto result = CompareImages(View(expected), View(actual));
    ASSERT_THAT(result.error, IsEmpty());
    EXPECT_TRUE(result.passed);
    EXPECT_EQ(result.mismatchedPixels, 0);
    ASSERT_TRUE(result.diff);
    EXPECT_THAT(ValidateImage(View(*result.diff)), IsEmpty());
  }
}

TEST(ImageCompareTest, TightEdgeControlUsesNeighborRows) {
  const Image expected = VerticalEdgeImage();
  Image actual = expected;
  std::fill_n(actual.data.begin() + 16, 3, 128);
  EXPECT_EQ(CompareImages(View(expected), View(actual), {0.0, 0, true}).mismatchedPixels, 1);
  EXPECT_EQ(CompareImages(View(expected), View(actual), {0.0, 0, false}).mismatchedPixels, 0);
}

TEST(ImageCompareTest, IndependentStridesPreserveNeighborRowComparison) {
  const Image tightExpected = VerticalEdgeImage();
  Image tightActual = tightExpected;
  std::fill_n(tightActual.data.begin() + 16, 3, 128);
  for (const bool includeEdges : {true, false}) {
    const ComparisonOptions options{0.0, 0, includeEdges};
    const auto control = CompareImages(View(tightExpected), View(tightActual), options);
    ASSERT_THAT(control.error, IsEmpty());
    ASSERT_TRUE(control.diff);
    EXPECT_EQ(control.mismatchedPixels, includeEdges ? 1 : 0);
    for (const auto strides : {std::array<size_t, 2>{3, 5}, {5, 3}, {4, 6}, {6, 4}}) {
      SCOPED_TRACE(testing::Message() << includeEdges << ": " << strides[0] << ", " << strides[1]);
      const Image expected = WithRowPadding(tightExpected, strides[0], 0xA5);
      const Image actual = WithRowPadding(tightActual, strides[1], 0x5A);
      const auto result = CompareImages(View(expected), View(actual), options);
      ASSERT_THAT(result.error, IsEmpty());
      EXPECT_EQ(result.passed, control.passed);
      EXPECT_EQ(result.mismatchedPixels, control.mismatchedPixels);
      ASSERT_TRUE(result.diff);
      const Image tightDiff = WithRowPadding(*result.diff, 3, 0);
      EXPECT_EQ(tightDiff.data, control.diff->data);
    }
  }
}

TEST_F(SvgSuiteTest, CallbackWithPaddedRowsMatchesTightPngReference) {
  addCase("padded.svg");
  const auto discovery = discover();
  ASSERT_THAT(discovery.error, IsEmpty());
  ASSERT_EQ(discovery.cases.size(), 1u);
  const auto result = RunCase(
      discovery.cases.front(),
      [](const StaticCase&, int, int) {
        return ImageResult{WithRowPadding(RedImage(), 4, 0xA5), {}};
      },
      directory_ / "padded-output");
  EXPECT_TRUE(result.passed) << result.error;
  EXPECT_THAT(result.error, IsEmpty());
  EXPECT_EQ(result.mismatchedPixels, 0);
}

TEST(ImageCompareTest, InvalidBuffersNeverPassWithGenerousTolerance) {
  const Image empty;
  const ComparisonResult result = CompareImages(View(empty), View(empty), {1.0, 100, false});
  EXPECT_THAT(result.passed, Eq(false));
  EXPECT_THAT(result.error, Not(IsEmpty()));
  EXPECT_THAT(result.mismatchedPixels.has_value(), Eq(false));
}

TEST(ImageCompareTest, InvalidPaddedBuffersRemainRejected) {
  const Image expected = VerticalEdgeImage();
  Image actual = WithRowPadding(expected, 5, 0xA5);
  actual.data.pop_back();
  const auto truncated = CompareImages(View(expected), View(actual));
  EXPECT_THAT(truncated.error, HasSubstr("byte length"));
  EXPECT_FALSE(truncated.passed);
  EXPECT_FALSE(truncated.mismatchedPixels);
  EXPECT_FALSE(truncated.diff);

  actual.strideInPixels = std::numeric_limits<size_t>::max();
  const auto oversized = CompareImages(View(expected), View(actual));
  EXPECT_THAT(oversized.error, HasSubstr("dimensions/stride"));
  EXPECT_FALSE(oversized.passed);
  EXPECT_FALSE(oversized.mismatchedPixels);
  EXPECT_FALSE(oversized.diff);
}

TEST(ImageCompareTest, RejectsNegativeBudgetAndNonfiniteThreshold) {
  const Image image = RedImage();
  EXPECT_THAT(CompareImages(View(image), View(image), {0.0, -1, true}).error, HasSubstr("options"));
  EXPECT_THAT(
      CompareImages(View(image), View(image), {std::numeric_limits<double>::quiet_NaN(), 0, true})
          .error,
      HasSubstr("options"));
}

TEST(ImageCompareTest, DimensionMismatchHasNoInventedPixelCount) {
  const Image expected = RedImage();
  const Image actual{2, 1, 2, {255, 0, 0, 255, 255, 0, 0, 255}};
  const auto result = CompareImages(View(expected), View(actual));
  EXPECT_THAT(result.error, Eq("dimension-mismatch"));
  EXPECT_THAT(result.mismatchedPixels.has_value(), Eq(false));
  EXPECT_THAT(result.passed, Eq(false));
}

/// The counts \ref CompareImages reports for one comparison, printable as a whole on failure.
struct EdgeCoverageOutcome {
  std::optional<int> mismatchedPixels;
  int edgeCoveragePixels = 0;
  int maxEdgeCoverageAlphaDelta = 0;
  std::string error;

  friend bool operator==(const EdgeCoverageOutcome&, const EdgeCoverageOutcome&) = default;

  friend void PrintTo(const EdgeCoverageOutcome& outcome, std::ostream* os) {
    *os << "{mismatched=" << (outcome.mismatchedPixels ? *outcome.mismatchedPixels : -1)
        << ", edgeCoverage=" << outcome.edgeCoveragePixels
        << ", maxAlphaDelta=" << outcome.maxEdgeCoverageAlphaDelta << ", error=\"" << outcome.error
        << "\"}";
  }
};

using Rgba = std::array<uint8_t, 4>;

constexpr Rgba kEmpty = {0, 0, 0, 0};
constexpr Rgba kOpaqueBlack = {0, 0, 0, 255};

/// A 5-pixel wide image whose rows are each one color, top to bottom.
Image RowsImage(const std::vector<Rgba>& rows) {
  Image image{5, static_cast<int>(rows.size()), 5, {}};
  for (const Rgba& row : rows) {
    for (int i = 0; i < 5; ++i) {
      image.data.insert(image.data.end(), row.begin(), row.end());
    }
  }
  return image;
}

/// A 3x3 image of one color. Every pixel has eight identical neighbors or touches the border with
/// identical neighbors, so pixelmatch never classifies a difference in it as anti-aliasing.
Image UniformImage(Rgba pixel) {
  Image image{3, 3, 3, {}};
  for (int i = 0; i < 9; ++i) {
    image.data.insert(image.data.end(), pixel.begin(), pixel.end());
  }
  return image;
}

ComparisonOptions EdgeCoverageOptions(double threshold = 0.0) {
  return {.threshold = threshold,
          .maxMismatchedPixels = 0,
          .includeAntiAliasing = false,
          .acceptQuarterPixelEdgeCoverage = true};
}

EdgeCoverageOutcome Compare(const Image& expected, const Image& actual,
                            const ComparisonOptions& options) {
  const ComparisonResult result = CompareImages(View(expected), View(actual), options);
  return {result.mismatchedPixels, result.edgeCoveragePixels, result.maxEdgeCoverageAlphaDelta,
          result.error};
}

/// Compares two 5x5 images that draw a one-pixel line, \p expected and \p actual, across row 2
/// over \p surround. The line is darker or lighter than every pixel around it in each image, so
/// pixelmatch does not classify its 5 pixels as anti-aliasing, and in the expected image it is the
/// partially covered row of a horizontal edge whenever \p surround is empty or fully covered.
EdgeCoverageOutcome CompareLine(Rgba expected, Rgba actual, const ComparisonOptions& options,
                                Rgba surround = kEmpty) {
  return Compare(RowsImage({surround, surround, expected, surround, surround}),
                 RowsImage({surround, surround, actual, surround, surround}), options);
}

EdgeCoverageOutcome CompareUniform(Rgba expected, Rgba actual, const ComparisonOptions& options) {
  return Compare(UniformImage(expected), UniformImage(actual), options);
}

TEST(ImageCompareEdgeCoverageTest, IsOffByDefault) {
  const ComparisonOptions defaults{
      .threshold = 0.0, .maxMismatchedPixels = 0, .includeAntiAliasing = false};
  EXPECT_THAT(defaults.acceptQuarterPixelEdgeCoverage, Eq(false));
  EXPECT_THAT(CompareLine({0, 0, 0, 128}, {0, 0, 0, 159}, defaults),
              Eq(EdgeCoverageOutcome{5, 0, 0, ""}));
}

TEST(ImageCompareEdgeCoverageTest, AcceptsEdgeCoverageDifferencesUpToTheBound) {
  for (const int delta : {1, 32, kQuarterPixelEdgeCoverageMaxAlphaDelta}) {
    SCOPED_TRACE(testing::Message() << "alpha delta " << delta);
    const auto actualAlpha = static_cast<uint8_t>(128 + delta);
    EXPECT_THAT(CompareLine({0, 0, 0, 128}, {0, 0, 0, actualAlpha}, EdgeCoverageOptions()),
                Eq(EdgeCoverageOutcome{0, 5, delta, ""}));
    EXPECT_THAT(CompareLine({0, 0, 0, actualAlpha}, {0, 0, 0, 128}, EdgeCoverageOptions()),
                Eq(EdgeCoverageOutcome{0, 5, delta, ""}));
  }
  // A colored edge near both ends of the alpha range.
  EXPECT_THAT(CompareLine({40, 90, 200, 1}, {40, 90, 200, 34}, EdgeCoverageOptions()),
              Eq(EdgeCoverageOutcome{0, 5, 33, ""}));
  EXPECT_THAT(CompareLine({40, 90, 200, 254}, {40, 90, 200, 221}, EdgeCoverageOptions()),
              Eq(EdgeCoverageOutcome{0, 5, 33, ""}));
  // At the suite's default threshold pixelmatch counts only some of these pixels, depending on
  // their checkerboard background; the rule accepts every one it counts.
  const EdgeCoverageOutcome atDefaultThreshold =
      CompareLine({40, 90, 200, 254}, {40, 90, 200, 222}, EdgeCoverageOptions(0.02));
  EXPECT_THAT(atDefaultThreshold.mismatchedPixels, Eq(std::optional<int>(0)));
  EXPECT_THAT(atDefaultThreshold.edgeCoveragePixels, testing::Gt(0));
}

// The references' 1.25 px crosshair: its two edge rows store two and three quarter rows, 128 and
// 192, where exact coverage is 0.625 on each, 159. Pixelmatch classifies the upper row as
// anti-aliasing and counts the lower one, which lies above the empty background.
TEST(ImageCompareEdgeCoverageTest, AcceptsTheCrosshairEdgeRow) {
  const Image expected =
      RowsImage({kEmpty, kEmpty, {0, 0, 0, 128}, {0, 0, 0, 192}, kEmpty, kEmpty});
  const Image actual = RowsImage({kEmpty, kEmpty, {0, 0, 0, 159}, {0, 0, 0, 159}, kEmpty, kEmpty});
  EXPECT_THAT(Compare(expected, actual, EdgeCoverageOptions()),
              Eq(EdgeCoverageOutcome{0, 5, 33, ""}));
}

TEST(ImageCompareEdgeCoverageTest, AcceptsAnEdgeBesideAFullyCoveredRow) {
  // A gap in an opaque fill: the line row is less covered than the rows around it.
  EXPECT_THAT(CompareLine({0, 0, 0, 192}, {0, 0, 0, 159}, EdgeCoverageOptions(), kOpaqueBlack),
              Eq(EdgeCoverageOutcome{0, 5, 33, ""}));
  // The empty or fully covered neighbor may be either the pixel above or the one below.
  const Image expected = RowsImage({kEmpty, kEmpty, {0, 0, 0, 192}, {0, 0, 0, 128}, {0, 0, 0, 64}});
  const Image actual = RowsImage({kEmpty, kEmpty, {0, 0, 0, 159}, {0, 0, 0, 128}, {0, 0, 0, 64}});
  EXPECT_THAT(Compare(expected, actual, EdgeCoverageOptions()),
              Eq(EdgeCoverageOutcome{0, 5, 33, ""}));
}

// A coverage or opacity difference across a uniform partially covered region is not edge
// quantization: every pixel's neighbors share its alpha.
TEST(ImageCompareEdgeCoverageTest, CountsDifferencesAcrossAUniformPartiallyCoveredRegion) {
  for (const auto& [expected, actual] : std::vector<std::pair<Rgba, Rgba>>{
           {{0, 0, 0, 128}, {0, 0, 0, 159}},
           {{0, 0, 0, 159}, {0, 0, 0, 128}},
           {{0, 0, 0, 192}, {0, 0, 0, 159}},
           {{40, 90, 200, 254}, {40, 90, 200, 221}},
       }) {
    SCOPED_TRACE(testing::Message() << "alphas " << static_cast<int>(expected[3]) << ", "
                                    << static_cast<int>(actual[3]));
    EXPECT_THAT(CompareUniform(expected, actual, EdgeCoverageOptions()),
                Eq(EdgeCoverageOutcome{9, 0, 0, ""}));
  }
}

TEST(ImageCompareEdgeCoverageTest, CountsPixelsWithoutAnEmptyOrFullyCoveredRowAboveOrBelow) {
  // The same line over a partially covered surround.
  EXPECT_THAT(CompareLine({0, 0, 0, 192}, {0, 0, 0, 159}, EdgeCoverageOptions(), {0, 0, 0, 32}),
              Eq(EdgeCoverageOutcome{5, 0, 0, ""}));
  // Only the image border is beyond the pixel: outside the image does not count as empty.
  const Image expected = RowsImage({{0, 0, 0, 32}, {0, 0, 0, 32}, {0, 0, 0, 128}});
  const Image actual = RowsImage({{0, 0, 0, 32}, {0, 0, 0, 32}, {0, 0, 0, 159}});
  EXPECT_THAT(Compare(expected, actual, EdgeCoverageOptions()),
              Eq(EdgeCoverageOutcome{5, 0, 0, ""}));
  // The same pixels are accepted when the row above them is empty.
  EXPECT_THAT(Compare(RowsImage({kEmpty, kEmpty, {0, 0, 0, 128}}),
                      RowsImage({kEmpty, kEmpty, {0, 0, 0, 159}}), EdgeCoverageOptions()),
              Eq(EdgeCoverageOutcome{0, 5, 31, ""}));
}

TEST(ImageCompareEdgeCoverageTest, CountsPixelsWhoseRowAboveOrBelowSharesTheirAlpha) {
  // A two-row band of one alpha over the empty background: each row is beside an empty row, but
  // also beside a row of its own alpha, so it is a uniform region rather than an edge row.
  const Image expected = RowsImage({kEmpty, {0, 0, 0, 192}, {0, 0, 0, 192}, kEmpty, kEmpty});
  const Image actual = RowsImage({kEmpty, {0, 0, 0, 159}, {0, 0, 0, 159}, kEmpty, kEmpty});
  EXPECT_THAT(Compare(expected, actual, EdgeCoverageOptions()),
              Eq(EdgeCoverageOutcome{10, 0, 0, ""}));
}

TEST(ImageCompareEdgeCoverageTest, CountsCoverageDifferencesAboveTheBound) {
  EXPECT_THAT(CompareLine({0, 0, 0, 128}, {0, 0, 0, 162}, EdgeCoverageOptions()),
              Eq(EdgeCoverageOutcome{5, 0, 0, ""}));
  EXPECT_THAT(CompareLine({0, 0, 0, 200}, {0, 0, 0, 100}, EdgeCoverageOptions()),
              Eq(EdgeCoverageOutcome{5, 0, 0, ""}));
}

TEST(ImageCompareEdgeCoverageTest, CountsPixelsFullyCoveredOrEmptyInEitherImage) {
  for (const auto& [expected, actual] : std::vector<std::pair<Rgba, Rgba>>{
           {{0, 0, 0, 255}, {0, 0, 0, 230}},
           {{0, 0, 0, 230}, {0, 0, 0, 255}},
           {{0, 0, 0, 0}, {0, 0, 0, 20}},
           {{0, 0, 0, 20}, {0, 0, 0, 0}},
       }) {
    SCOPED_TRACE(testing::Message() << "alphas " << static_cast<int>(expected[3]) << ", "
                                    << static_cast<int>(actual[3]));
    EXPECT_THAT(CompareLine(expected, actual, EdgeCoverageOptions()),
                Eq(EdgeCoverageOutcome{5, 0, 0, ""}));
  }
}

TEST(ImageCompareEdgeCoverageTest, CountsColorDifferencesAtEdgePixels) {
  // Same coverage, different color.
  EXPECT_THAT(CompareLine({0, 0, 0, 128}, {255, 0, 0, 128}, EdgeCoverageOptions(0.02)),
              Eq(EdgeCoverageOutcome{5, 0, 0, ""}));
  // A coverage difference within the bound that also carries a color difference.
  EXPECT_THAT(CompareLine({0, 0, 0, 128}, {0, 0, 255, 150}, EdgeCoverageOptions(0.02)),
              Eq(EdgeCoverageOutcome{5, 0, 0, ""}));
  EXPECT_THAT(CompareLine({0, 0, 0, 128}, {70, 70, 70, 150}, EdgeCoverageOptions(0.02)),
              Eq(EdgeCoverageOutcome{5, 0, 0, ""}));
}

// Deliberate: the color condition runs at the smaller alpha, at pixelmatch's sensitivity for that
// coverage, so a black-to-white difference at alpha 2 is accepted while the same colors at alpha
// 100 are counted.
TEST(ImageCompareEdgeCoverageTest, ComparesColorAtTheSmallerAlpha) {
  EXPECT_THAT(CompareLine({0, 0, 0, 2}, {255, 255, 255, 30}, EdgeCoverageOptions(0.02)),
              Eq(EdgeCoverageOutcome{0, 5, 28, ""}));
  EXPECT_THAT(CompareLine({0, 0, 0, 100}, {255, 255, 255, 128}, EdgeCoverageOptions(0.02)),
              Eq(EdgeCoverageOutcome{5, 0, 0, ""}));
}

TEST(ImageCompareEdgeCoverageTest, AcceptsOnlyQualifyingPixelsAndMarksThemInTheDiff) {
  // Two lines over the empty background: the upper one differs within the bound, the lower one by
  // more than it.
  const Image expected = RowsImage({kEmpty, {0, 0, 0, 128}, kEmpty, {0, 0, 0, 128}, kEmpty});
  const Image actual = RowsImage({kEmpty, {0, 0, 0, 150}, kEmpty, {0, 0, 0, 200}, kEmpty});
  const ComparisonResult result =
      CompareImages(View(expected), View(actual), EdgeCoverageOptions());
  EXPECT_THAT((EdgeCoverageOutcome{result.mismatchedPixels, result.edgeCoveragePixels,
                                   result.maxEdgeCoverageAlphaDelta, result.error}),
              Eq(EdgeCoverageOutcome{5, 5, 22, ""}));
  EXPECT_THAT(result.passed, Eq(false));
  ASSERT_TRUE(result.diff.has_value());
  const auto diffRow = [&](int row) {
    std::vector<Rgba> pixels;
    for (size_t pos = static_cast<size_t>(row) * 5 * 4; pos < static_cast<size_t>(row + 1) * 5 * 4;
         pos += 4) {
      pixels.push_back({result.diff->data[pos], result.diff->data[pos + 1],
                        result.diff->data[pos + 2], result.diff->data[pos + 3]});
    }
    return pixels;
  };
  EXPECT_THAT(diffRow(1), Eq(std::vector<Rgba>(5, kEdgeCoverageDiffColor)));
  EXPECT_THAT(diffRow(3), Eq(std::vector<Rgba>(5, Rgba{255, 0, 0, 255})));
}

TEST(ImageCompareEdgeCoverageTest, RequiresAntiAliasingDetection) {
  ComparisonOptions options = EdgeCoverageOptions();
  options.includeAntiAliasing = true;
  EXPECT_THAT(CompareLine({0, 0, 0, 128}, {0, 0, 0, 150}, options).error, HasSubstr("options"));
}

TEST_F(SvgSuiteTest, PngRoundTripAndMalformedInput) {
  const Image image = RedImage();
  ASSERT_THAT(WritePng(directory_ / "image.png", View(image)), IsEmpty());
  const ImageResult decoded = ReadPng(directory_ / "image.png");
  ASSERT_THAT(decoded.error, IsEmpty());
  ASSERT_THAT(decoded.image.has_value(), Eq(true));
  EXPECT_THAT(decoded.image->data, Eq(image.data));
  std::ofstream(directory_ / "invalid.png") << "not a png file";
  EXPECT_THAT(ReadPng(directory_ / "invalid.png").error, Eq("input is not PNG"));
}

TEST_F(SvgSuiteTest, OversizedPngRejectedBeforeDecode) {
  const auto path = directory_ / "large.png";
  std::ofstream(path) << "x";
  std::filesystem::resize_file(path, 32u * 1024u * 1024u + 1u);
  EXPECT_THAT(ReadPng(path).error, HasSubstr("oversized"));
}

TEST_F(SvgSuiteTest, TinyPngCannotExpandBeyondDecoderAllocationBudget) {
  // A valid 1x1 IHDR followed by 128 KiB of zero-valued inflated IDAT data.
  constexpr uint8_t kOversizedInflatePng[] = {
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
      0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f,
      0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x95, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0xed, 0xc1,
      0x31, 0x01, 0x00, 0x00, 0x00, 0xc2, 0xa0, 0xf5, 0x4f, 0xed, 0x61, 0x0d, 0xa0, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x6e, 0x00, 0x1e, 0x00, 0x01, 0x97, 0xe0, 0xa7, 0xe7, 0x00,
      0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
  };
  const auto path = directory_ / "oversized-inflate.png";
  std::ofstream input(path, std::ios::binary);
  input.write(reinterpret_cast<const char*>(kOversizedInflatePng), sizeof(kOversizedInflatePng));
  input.close();
  const ImageResult oversized = ReadPng(path, 64u * 1024u);
  EXPECT_THAT(oversized.error, Eq("PNG decoder allocation limit exceeded"));
  EXPECT_THAT(oversized.image.has_value(), Eq(false));

  const Image image = RedImage();
  ASSERT_THAT(WritePng(directory_ / "ordinary.png", View(image)), IsEmpty());
  const ImageResult ordinary = ReadPng(directory_ / "ordinary.png", 64u * 1024u);
  ASSERT_THAT(ordinary.error, IsEmpty());
  ASSERT_THAT(ordinary.image.has_value(), Eq(true));
  EXPECT_THAT(ordinary.image->data, Eq(image.data));
  EXPECT_THAT(ReadPng(path, 256u * 1024u * 1024u + 1u).error,
              Eq("invalid PNG decoder allocation budget"));
  EXPECT_THAT(ReadPng(path, 0).error, Eq("invalid PNG decoder allocation budget"));
}

TEST_F(SvgSuiteTest, DiscoveryIsSortedRecursiveAndCollisionFree) {
  addCase("z.svg");
  addCase("a-b.svg");
  addCase("a_b.svg");
  addCase("nested/a.svg");
  const auto result = discover();
  ASSERT_THAT(result.error, IsEmpty());
  ASSERT_THAT(result.cases.size(), Eq(4u));
  for (size_t index = 1; index < result.cases.size(); ++index) {
    EXPECT_LT(result.cases[index - 1].id, result.cases[index].id);
  }
  const auto second = discover();
  EXPECT_THAT(second.cases.front().id, Eq(result.cases.front().id));
}

TEST_F(SvgSuiteTest, DiscoveryRejectsEmptyMissingReferenceAndDuplicateRoots) {
  EXPECT_THAT(discover().error, HasSubstr("empty corpus"));
  std::ofstream(directory_ / "corpus" / "missing.svg") << "<svg/>";
  EXPECT_THAT(discover().error, HasSubstr("missing"));
  EXPECT_THAT(DiscoverCases({}).error, HasSubstr("no corpus"));
  addCase("missing.svg");
  EXPECT_THAT(
      DiscoverCases({{"same", directory_ / "corpus"}, {"same", directory_ / "corpus"}}).error,
      HasSubstr("duplicate"));
}

TEST_F(SvgSuiteTest, DiscoveryRejectsSymlinkEscape) {
  std::ofstream(directory_ / "outside.svg") << "<svg/>";
  std::filesystem::create_symlink(directory_ / "outside.svg", directory_ / "corpus" / "escape.svg");
  EXPECT_THAT(discover().error, HasSubstr("escapes"));
}

TEST_F(SvgSuiteTest, CallbackGetsReferenceDimensionsAndEvidenceIsPreserved) {
  addCase("red.svg");
  const auto discovery = discover();
  ASSERT_THAT(discovery.error, IsEmpty());
  int callbacks = 0;
  const Renderer renderer = [&](const StaticCase& test, int width, int height) -> ImageResult {
    ++callbacks;
    EXPECT_THAT(test.svgPath.filename().string(), Eq("red.svg"));
    EXPECT_THAT(width, Eq(1));
    EXPECT_THAT(height, Eq(1));
    return {RedImage(), {}};
  };
  const auto result = RunCase(discovery.cases.front(), renderer, directory_ / "artifacts");
  EXPECT_THAT(result.error, IsEmpty());
  EXPECT_THAT(result.passed, Eq(true));
  EXPECT_THAT(callbacks, Eq(1));
  for (const char* filename : {"expected.png", "actual.png", "diff.png", "metrics.json"}) {
    EXPECT_THAT(std::filesystem::is_regular_file(result.artifactDirectory / filename), Eq(true))
        << filename;
  }
  const auto repeat = RunCase(discovery.cases.front(), renderer, directory_ / "artifacts");
  EXPECT_THAT(repeat.error, HasSubstr("already exists"));
  EXPECT_THAT(callbacks, Eq(1));
}

TEST_F(SvgSuiteTest, WrongRendererOutputAndRendererErrorsFail) {
  addCase("red.svg");
  const auto discovery = discover();
  ASSERT_THAT(discovery.error, IsEmpty());
  const Renderer wrong = [](const StaticCase&, int, int) -> ImageResult {
    return {Image{1, 1, 1, {0, 0, 255, 255}}, {}};
  };
  const auto mismatch = RunCase(discovery.cases.front(), wrong, directory_ / "mismatch");
  EXPECT_THAT(mismatch.passed, Eq(false));
  EXPECT_THAT(mismatch.mismatchedPixels, Eq(std::optional<int>(1)));
  EXPECT_THAT(mismatch.error, IsEmpty());
  const Renderer broken = [](const StaticCase&, int, int) -> ImageResult {
    return {{}, "renderer crashed"};
  };
  const auto failure = RunCase(discovery.cases.front(), broken, directory_ / "failure");
  EXPECT_THAT(failure.passed, Eq(false));
  EXPECT_THAT(failure.error, Eq("renderer crashed"));
  EXPECT_THAT(std::filesystem::is_regular_file(failure.artifactDirectory / "metrics.json"),
              Eq(true));
}

TEST_F(SvgSuiteTest, RejectsArtifactSymlinkAndCorpusOutput) {
  addCase("red.svg");
  const auto discovery = discover();
  ASSERT_THAT(discovery.error, IsEmpty());
  const Renderer renderer = [](const StaticCase&, int, int) -> ImageResult {
    return {RedImage(), {}};
  };
  EXPECT_THAT(RunCase(discovery.cases.front(), renderer, directory_ / "corpus" / "out").error,
              HasSubstr("outside the corpus"));
  std::filesystem::create_directories(directory_ / "output");
  std::filesystem::create_directory_symlink(directory_ / "corpus",
                                            directory_ / "output" / discovery.cases.front().id);
  EXPECT_THAT(RunCase(discovery.cases.front(), renderer, directory_ / "output").error,
              HasSubstr("symlink"));
}
}  // namespace
}  // namespace donner::svg_suite
