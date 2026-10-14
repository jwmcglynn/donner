#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <limits>

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

TEST(ImageCompareTest, InvalidBuffersNeverPassWithGenerousTolerance) {
  const Image empty;
  const ComparisonResult result = CompareImages(View(empty), View(empty), {1.0, 100, false});
  EXPECT_THAT(result.passed, Eq(false));
  EXPECT_THAT(result.error, Not(IsEmpty()));
  EXPECT_THAT(result.mismatchedPixels.has_value(), Eq(false));
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
