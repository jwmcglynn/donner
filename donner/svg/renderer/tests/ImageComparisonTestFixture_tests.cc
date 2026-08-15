#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "donner/base/tests/TestTempDir.h"

namespace donner::svg {
namespace {

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
  EXPECT_EQ(params.effectiveMaxMismatchedPixels(ComparisonMode::GeodeTinyParity), 500);
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
  if (error) {
    GTEST_SKIP() << "Could not create test symlink: " << error.message();
  }

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

}  // namespace
}  // namespace donner::svg
