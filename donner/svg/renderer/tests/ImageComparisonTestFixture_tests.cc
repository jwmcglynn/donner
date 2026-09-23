#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"

#include <gmock/gmock.h>
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "donner/base/FileUtils.h"
#include "donner/base/tests/EnvironmentCapabilityGate.h"
#include "donner/base/tests/ScopedEnvironmentVariable.h"
#include "donner/base/tests/TestTempDir.h"
#include "donner/svg/renderer/RendererImageIO.h"

namespace donner::svg {
namespace {

class GoldenArtifactTest : public ImageComparisonTestFixture {};

TEST_F(GoldenArtifactTest, MismatchExportsActualExpectedAndDiff) {
  const std::filesystem::path root = TestTempDir() / "golden-mismatch-triplet";
  const std::filesystem::path output = root / "artifacts";
  std::filesystem::create_directories(output);
  const std::filesystem::path svgPath = root / "input.svg";
  const std::filesystem::path goldenPath = root / "blue.png";
  {
    std::ofstream svg(svgPath);
    svg << R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="2" height="2">
      <rect width="2" height="2" fill="red"/></svg>)svg";
    ASSERT_THAT(svg.good(), testing::IsTrue());
  }
  const std::vector<uint8_t> blue{0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255};
  ASSERT_THAT(RendererImageIO::writeRgbaPixelsToPngFile(goldenPath.string().c_str(), blue, 2, 2),
              testing::IsTrue());
  ScopedEnvironmentVariable artifactDirectory("TEST_UNDECLARED_OUTPUTS_DIR",
                                              output.string().c_str());
  ScopedEnvironmentVariable noGoldenUpdates("UPDATE_GOLDEN_IMAGES_DIR", nullptr);
  SVGDocument document = loadSVG(svgPath.string().c_str());
  testing::TestPartResultArray failures;
  {
    testing::ScopedFakeTestPartResultReporter capture(
        testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD, &failures);
    renderAndCompare(document, svgPath, goldenPath.string().c_str(),
                     ImageComparisonParams::WithThreshold(0.0f, 0).includeAntiAliasingDifferences(),
                     ComparisonMode::TinyGolden);
  }
  ASSERT_THAT(failures.size(), testing::Eq(1));
  std::vector<std::string> prefixes;
  std::filesystem::path expectedArtifact;
  for (const auto& entry : std::filesystem::directory_iterator(output)) {
    const std::string filename = entry.path().filename().string();
    prefixes.push_back(filename.substr(0, filename.find('_')));
    EXPECT_THAT(entry.file_size(), testing::Gt(0));
    if (filename.starts_with("expected_")) {
      expectedArtifact = entry.path();
    }
  }
  std::sort(prefixes.begin(), prefixes.end());
  ASSERT_THAT(prefixes, testing::ElementsAre("actual", "diff", "expected"));
  const FileReadResult golden = ReadFileBounded(goldenPath, 4096);
  ASSERT_THAT(golden, testing::VariantWith<std::string>(testing::Not(testing::IsEmpty())));
  EXPECT_THAT(ReadFileBounded(expectedArtifact, 4096),
              testing::VariantWith<std::string>(testing::Eq(std::get<std::string>(golden))));
}

testing::TestParamInfo<ImageComparisonTestParam> MakeParamInfo(ImageComparisonParams params) {
  ImageComparisonTestcase testcase;
  testcase.svgFilename = "embedded-png.svg";
  testcase.params = params;

  return testing::TestParamInfo<ImageComparisonTestParam>(
      ImageComparisonTestParam{testcase, ComparisonMode::TinyGolden}, 0);
}

TEST(ImageComparisonTestFixtureTests, ExplicitSkipsUseDisabledGtestName) {
  const std::string name =
      TestNameFromFilename(MakeParamInfo(ImageComparisonParams::Skip("triaged gap")));

  EXPECT_THAT(name, testing::StartsWith("DISABLED_"));
  EXPECT_THAT(name, testing::HasSubstr("embedded_png"));
}

TEST(ImageComparisonTestFixtureTests, GeodeMaxPixelsOnlyAppliesToGeodeModes) {
  ImageComparisonParams params;
  params.withMaxPixelsDifferent(150).withGeodeMaxPixelsDifferent(500);

  EXPECT_EQ(params.effectiveMaxMismatchedPixels(ComparisonMode::TinyGolden), 150);
  EXPECT_EQ(params.effectiveMaxMismatchedPixels(ComparisonMode::GeodeGolden), 500);
}

TEST(ImageComparisonTestFixtureTests, ResolvesSymlinkedRunfilesRootToCanonicalDocumentTree) {
  const std::filesystem::path testRoot = TestTempDir() / "runfiles-resource-root";
  const std::filesystem::path canonicalRoot = testRoot / "canonical";
  const std::filesystem::path runfilesRoot = testRoot / "runfiles";
  const std::filesystem::path canonicalDocument = canonicalRoot / "document.svg";
  std::filesystem::create_directories(canonicalRoot);
  {
    std::ofstream file(canonicalDocument);
    ASSERT_TRUE(file.good());
    file << "<svg/>";
  }

  std::error_code error;
  std::filesystem::create_directory_symlink(canonicalRoot, runfilesRoot, error);
  DONNER_REQUIRE_ENVIRONMENT_CAPABILITY(error ? error.message() : std::string(),
                                        "the ability to create filesystem symlinks");

  EXPECT_EQ(ResolveRunfilesResourceRootForTesting(runfilesRoot, runfilesRoot / "document.svg"),
            canonicalRoot);
  EXPECT_EQ(ResolveRunfilesResourceRootForTesting(runfilesRoot, canonicalDocument), runfilesRoot);
}

#ifndef DONNER_TEXT_FULL
TEST(ImageComparisonTestFixtureTests, TextFullOnlyRunsUseDisabledGtestNameInSimpleTextBuild) {
  ImageComparisonParams params;
  params.onlyTextFull();

  const std::string name = TestNameFromFilename(MakeParamInfo(params));

  EXPECT_THAT(name, testing::StartsWith("DISABLED_"));
}
#endif

/// A 2x2 opaque red bitmap: the side of the comparisons below that has pixels.
RendererBitmap OpaqueRedBitmap() {
  RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(2, 2);
  bitmap.rowBytes = 8;
  bitmap.pixels = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
  return bitmap;
}

/// A renderer that could not read its frame back returns an empty bitmap. That render has no
/// pixels to compare, so it is never identical to another render, not even to another empty one:
/// an equivalence test whose two renders both failed must not pass.
TEST(ImageComparisonTestFixtureTests, AnEmptyBitmapIsNeverIdentical) {
  EXPECT_NONFATAL_FAILURE(ExpectBitmapsIdentical(OpaqueRedBitmap(), RendererBitmap{}, "e"),
                          "e: the expected bitmap is empty");
  EXPECT_NONFATAL_FAILURE(ExpectBitmapsIdentical(RendererBitmap{}, OpaqueRedBitmap(), "a"),
                          "a: the actual bitmap is empty");
  EXPECT_NONFATAL_FAILURE(ExpectBitmapsIdentical(RendererBitmap{}, RendererBitmap{}, "both"),
                          "both: the actual and expected bitmaps are empty");
}

/// A check that a change alters output must not pass because a render was not read back: the
/// empty side fails on its own, once, rather than inside the comparison, where its failure would
/// read as a difference.
TEST(ImageComparisonTestFixtureTests, AnEmptyBitmapNeverDiffers) {
  EXPECT_NONFATAL_FAILURE(ExpectBitmapsDiffer(OpaqueRedBitmap(), RendererBitmap{}, "e"),
                          "empty 0x0 snapshot");
  EXPECT_NONFATAL_FAILURE(ExpectBitmapsDiffer(RendererBitmap{}, RendererBitmap{}, "both"),
                          "empty 0x0 snapshot");
}

TEST(ImageComparisonTestFixtureTests, IdenticalBitmapsFailTheDifferCheck) {
  EXPECT_NONFATAL_FAILURE(ExpectBitmapsDiffer(OpaqueRedBitmap(), OpaqueRedBitmap(), "same"),
                          "same: expected the renders to differ, but they are pixel-identical");
}

TEST(ImageComparisonTestFixtureTests, OnePixelOfDifferencePassesTheDifferCheck) {
  RendererBitmap oneBluePixel = OpaqueRedBitmap();
  oneBluePixel.pixels[0] = 0;
  oneBluePixel.pixels[2] = 255;

  ExpectBitmapsDiffer(oneBluePixel, OpaqueRedBitmap(), "one_blue_pixel");
}

}  // namespace
}  // namespace donner::svg
