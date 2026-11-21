#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"

#include <gmock/gmock.h>
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/FileUtils.h"
#include "donner/base/tests/EnvironmentCapabilityGate.h"
#include "donner/base/tests/ScopedEnvironmentVariable.h"
#include "donner/base/tests/TestTempDir.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/tests/RendererImageTestUtils.h"

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

/// Writes a solid blue PNG of \p width x \p height to \p path.
testing::AssertionResult WriteBluePng(const std::filesystem::path& path, int width, int height) {
  std::vector<uint8_t> pixels;
  for (int i = 0; i < width * height; ++i) {
    pixels.insert(pixels.end(), {0, 0, 255, 255});
  }
  if (!RendererImageIO::writeRgbaPixelsToPngFile(path.string().c_str(), pixels, width, height)) {
    return testing::AssertionFailure() << "cannot write " << path;
  }
  return testing::AssertionSuccess();
}

/// Writes \p bytes to \p path.
testing::AssertionResult WriteBytes(const std::filesystem::path& path, std::string_view bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!file.good()) {
    return testing::AssertionFailure() << "cannot write " << path;
  }
  return testing::AssertionSuccess();
}

/// Writes a 2:1 document drawing a blue rect over its whole viewport to \p path.
testing::AssertionResult WriteWideBlueSvg(const std::filesystem::path& path) {
  return WriteBytes(
      path, R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="2" height="1" viewBox="0 0 2 1">
  <rect width="2" height="1" fill="blue"/></svg>)svg");
}

/// The messages of the failures a scoped reporter captured.
std::vector<std::string> FailureMessages(const testing::TestPartResultArray& failures) {
  std::vector<std::string> messages;
  for (int i = 0; i < failures.size(); ++i) {
    messages.emplace_back(failures.GetTestPartResult(i).message());
  }
  return messages;
}

/// The types of the failures a scoped reporter captured.
std::vector<testing::TestPartResult::Type> FailureTypes(
    const testing::TestPartResultArray& failures) {
  std::vector<testing::TestPartResult::Type> types;
  for (int i = 0; i < failures.size(); ++i) {
    types.push_back(failures.GetTestPartResult(i).type());
  }
  return types;
}

/// The file names in \p directory.
std::vector<std::string> FileNames(const std::filesystem::path& directory) {
  std::vector<std::string> names;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

TEST(ImageComparisonTestFixtureTests, ReferenceCanvasIsTheSquareOfItsLongerSide) {
  const std::filesystem::path root = TestTempDir() / "reference-canvas";
  std::filesystem::create_directories(root);
  ASSERT_TRUE(WriteBluePng(root / "wide.png", 4, 2));
  ASSERT_TRUE(WriteBluePng(root / "tall.png", 2, 3));

  const CanvasSizeResult wide = CanvasSizeFromReferenceImage(root / "wide.png");
  EXPECT_THAT(wide.size, testing::Optional(Vector2i(4, 4)));
  EXPECT_THAT(wide.error, testing::IsEmpty());
  EXPECT_THAT(CanvasSizeFromReferenceImage(root / "tall.png").size,
              testing::Optional(Vector2i(3, 3)));
}

TEST(ImageComparisonTestFixtureTests, UnreadableReferenceGivesNoCanvas) {
  using namespace std::literals;
  const std::filesystem::path root = TestTempDir() / "reference-canvas-errors";
  std::filesystem::create_directories(root);
  const std::string signature("\x89PNG\r\n\x1a\n"sv);
  const std::string dimensions("\0\0\0\x04\0\0\0\x03"sv);
  const std::string header = signature + "\0\0\0\x0dIHDR"s;
  ASSERT_TRUE(WriteBytes(root / "text.png", "not a png, but long enough to hold a header"));
  ASSERT_TRUE(WriteBytes(root / "truncated.png", header));
  ASSERT_TRUE(WriteBytes(root / "ihdr-not-first.png", signature + "\0\0\0\x0dtEXt"s + dimensions));
  ASSERT_TRUE(WriteBytes(root / "ihdr-length.png", signature + "\0\0\0\x0cIHDR"s + dimensions));
  ASSERT_TRUE(WriteBytes(root / "zero.png", header + "\0\0\0\0\0\0\0\x03"s));
  ASSERT_TRUE(WriteBytes(root / "huge.png", header + "\0\0\x40\x01\0\0\0\x03"s));

  const std::vector<std::pair<std::string, std::string>> cases = {
      {"missing.png", "is missing or cannot be opened"},
      {"text.png", "does not start with the PNG signature and a 13-byte IHDR chunk"},
      {"truncated.png", "is 16 bytes, shorter than a PNG signature and IHDR header"},
      {"ihdr-not-first.png", "does not start with the PNG signature and a 13-byte IHDR chunk"},
      {"ihdr-length.png", "does not start with the PNG signature and a 13-byte IHDR chunk"},
      {"zero.png", "has invalid dimensions 0x3"},
      {"huge.png", "has invalid dimensions 16385x3"},
  };
  for (const auto& [name, message] : cases) {
    SCOPED_TRACE(name);
    const CanvasSizeResult result = CanvasSizeFromReferenceImage(root / name);
    EXPECT_THAT(result.size, testing::Eq(std::nullopt));
    EXPECT_THAT(result.error, testing::AllOf(testing::HasSubstr((root / name).string()),
                                             testing::HasSubstr(message)));
  }
}

TEST(ImageComparisonTestFixtureTests, ExplicitCanvasWinsOverTheReference) {
  const std::filesystem::path missing = TestTempDir() / "no-such-reference.png";

  EXPECT_THAT(ResolveComparisonCanvasSize(ImageComparisonParams(), missing).size,
              testing::Eq(std::nullopt));
  EXPECT_THAT(
      ResolveComparisonCanvasSize(ImageComparisonParams().setCanvasSize(7, 5), missing).size,
      testing::Optional(Vector2i(7, 5)));
  EXPECT_THAT(
      ResolveComparisonCanvasSize(
          ImageComparisonParams().enableCanvasSizeFromReference().setCanvasSize(7, 5), missing)
          .size,
      testing::Optional(Vector2i(7, 5)));

  const CanvasSizeResult derived =
      ResolveComparisonCanvasSize(ImageComparisonParams().enableCanvasSizeFromReference(), missing);
  EXPECT_THAT(derived.size, testing::Eq(std::nullopt));
  EXPECT_THAT(derived.error, testing::AllOf(testing::HasSubstr("is missing or cannot be opened"),
                                            testing::HasSubstr("WithGoldenOverride"),
                                            testing::HasSubstr("Bazel data dependency"),
                                            testing::HasSubstr("UPDATE_GOLDEN_IMAGES_DIR"),
                                            testing::HasSubstr("setCanvasSize()")));
}

TEST(ImageComparisonTestFixtureTests, OnlyAMissingReferenceUsesTheFallbackCanvas) {
  const std::filesystem::path root = TestTempDir() / "reference-canvas-fallback";
  std::filesystem::create_directories(root);
  ASSERT_TRUE(WriteBluePng(root / "golden.png", 3, 3));
  ASSERT_TRUE(WriteBluePng(root / "vendored.png", 4, 2));
  const ImageComparisonParams params = ImageComparisonParams().enableCanvasSizeFromReference();

  EXPECT_THAT(ResolveComparisonCanvasSize(params, root / "golden.png", root / "vendored.png").size,
              testing::Optional(Vector2i(3, 3)));
  EXPECT_THAT(ResolveComparisonCanvasSize(params, root / "missing.png", root / "vendored.png").size,
              testing::Optional(Vector2i(4, 4)));

  const CanvasSizeResult bothMissing =
      ResolveComparisonCanvasSize(params, root / "missing.png", root / "no-vendored.png");
  EXPECT_THAT(bothMissing.size, testing::Eq(std::nullopt));
  EXPECT_THAT(bothMissing.error,
              testing::AllOf(testing::HasSubstr((root / "missing.png").string() + " is missing"),
                             testing::HasSubstr((root / "no-vendored.png").string() +
                                                " is missing or cannot be opened")));
}

TEST_F(GoldenArtifactTest, ComparisonRendersOnTheReferenceCanvas) {
  const std::filesystem::path root = TestTempDir() / "reference-canvas-render";
  std::filesystem::create_directories(root);
  const std::filesystem::path svgPath = root / "wide.svg";
  ASSERT_TRUE(WriteWideBlueSvg(svgPath));
  // A 2:1 document fitted into the 8x8 canvas the 8x4 reference implies renders at 8x4; at the
  // document's own size it would render at 2x1 and fail the dimension check.
  ASSERT_TRUE(WriteBluePng(root / "wide.png", 8, 4));
  ScopedEnvironmentVariable noGoldenUpdates("UPDATE_GOLDEN_IMAGES_DIR", nullptr);
  SVGDocument document = loadSVG(svgPath.string().c_str());
  testing::TestPartResultArray failures;
  {
    testing::ScopedFakeTestPartResultReporter capture(
        testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD, &failures);
    renderAndCompare(document, svgPath, (root / "wide.png").string().c_str(),
                     ImageComparisonParams::WithThreshold(0.0f, 0).enableCanvasSizeFromReference(),
                     ComparisonMode::TinyGolden);
  }
  EXPECT_THAT(FailureMessages(failures), testing::IsEmpty());
}

TEST_F(GoldenArtifactTest, MissingGoldenFailsOutsideUpdateMode) {
  const std::filesystem::path root = TestTempDir() / "reference-canvas-missing";
  std::filesystem::create_directories(root);
  const std::filesystem::path svgPath = root / "wide.svg";
  ASSERT_TRUE(WriteWideBlueSvg(svgPath));
  ASSERT_TRUE(WriteBluePng(root / "wide.png", 8, 4));
  ScopedEnvironmentVariable noGoldenUpdates("UPDATE_GOLDEN_IMAGES_DIR", nullptr);
  SVGDocument document = loadSVG(svgPath.string().c_str());
  testing::TestPartResultArray failures;
  {
    testing::ScopedFakeTestPartResultReporter capture(
        testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD, &failures);
    renderAndCompare(document, svgPath, (root / "golden.png").string().c_str(),
                     ImageComparisonParams().enableCanvasSizeFromReference(),
                     ComparisonMode::TinyGolden);
  }
  EXPECT_THAT(FailureTypes(failures), testing::ElementsAre(testing::TestPartResult::kFatalFailure));
  EXPECT_THAT(FailureMessages(failures),
              testing::ElementsAre(testing::HasSubstr((root / "golden.png").string() +
                                                      " is missing or cannot be opened")));
  EXPECT_THAT(FileNames(root), testing::ElementsAre("wide.png", "wide.svg"));
}

TEST_F(GoldenArtifactTest, UpdateModeWritesAMissingGoldenOnTheVendoredCanvas) {
  const std::filesystem::path root = TestTempDir() / "reference-canvas-update";
  const std::filesystem::path updates = root / "updates";
  std::filesystem::create_directories(updates);
  const std::filesystem::path svgPath = root / "wide.svg";
  ASSERT_TRUE(WriteWideBlueSvg(svgPath));
  ASSERT_TRUE(WriteBluePng(root / "wide.png", 8, 4));
  // An absolute golden path is written in place; updates/ stays empty.
  const std::filesystem::path golden = root / "golden.png";
  ScopedEnvironmentVariable goldenUpdates("UPDATE_GOLDEN_IMAGES_DIR", updates.string().c_str());
  SVGDocument document = loadSVG(svgPath.string().c_str());
  testing::TestPartResultArray failures;
  {
    testing::ScopedFakeTestPartResultReporter capture(
        testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD, &failures);
    renderAndCompare(document, svgPath, golden.string().c_str(),
                     ImageComparisonParams().enableCanvasSizeFromReference(),
                     ComparisonMode::TinyGolden);
  }
  EXPECT_THAT(FailureMessages(failures), testing::IsEmpty());
  const std::optional<Image> written =
      RendererImageTestUtils::readRgbaImageFromPngFile(golden.string().c_str());
  EXPECT_THAT(written, testing::Optional(testing::AllOf(testing::Field(&Image::width, 8),
                                                        testing::Field(&Image::height, 4))));
}

TEST_F(GoldenArtifactTest, UpdateModeFailsWhenTheVendoredPngIsMissingToo) {
  const std::filesystem::path root = TestTempDir() / "reference-canvas-update-missing";
  const std::filesystem::path updates = root / "updates";
  std::filesystem::create_directories(updates);
  const std::filesystem::path svgPath = root / "wide.svg";
  ASSERT_TRUE(WriteWideBlueSvg(svgPath));
  ScopedEnvironmentVariable goldenUpdates("UPDATE_GOLDEN_IMAGES_DIR", updates.string().c_str());
  SVGDocument document = loadSVG(svgPath.string().c_str());
  testing::TestPartResultArray failures;
  {
    testing::ScopedFakeTestPartResultReporter capture(
        testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD, &failures);
    renderAndCompare(document, svgPath, (root / "golden.png").string().c_str(),
                     ImageComparisonParams().enableCanvasSizeFromReference(),
                     ComparisonMode::TinyGolden);
  }
  EXPECT_THAT(FailureTypes(failures), testing::ElementsAre(testing::TestPartResult::kFatalFailure));
  EXPECT_THAT(
      FailureMessages(failures),
      testing::ElementsAre(testing::AllOf(
          testing::HasSubstr((root / "golden.png").string() + " is missing"),
          testing::HasSubstr((root / "wide.png").string() + " is missing or cannot be opened"))));
  EXPECT_THAT(FileNames(updates), testing::IsEmpty());
  EXPECT_THAT(FileNames(root), testing::ElementsAre("updates", "wide.svg"));
}

TEST(ImageComparisonTestFixtureTests, ResvgHttpsLoaderMapsOnlyExactSnapshots) {
  const auto root = TestTempDir() / "resvg-https-mapping";
  const auto local = root / "local";
  const auto snapshots = root / "snapshots";
  std::filesystem::create_directories(local);
  std::filesystem::create_directories(snapshots);
  { std::ofstream(snapshots / "PNG_transparency_demonstration_1.png") << "png"; }
  { std::ofstream(snapshots / "SVG_Logo.svg") << "svg"; }
  { std::ofstream(local / "relative.svg") << "local"; }
  { std::ofstream(root / "outside.svg") << "outside"; }
  auto loader = CreateResvgResourceLoaderForTesting(local, local / "document.svg", snapshots);
  EXPECT_THAT(loader->fetchExternalResource("https://upload.wikimedia.org/wikipedia/commons/4/47/"
                                            "PNG_transparency_demonstration_1.png"),
              testing::VariantWith<std::vector<uint8_t>>(testing::ElementsAre('p', 'n', 'g')));
  EXPECT_THAT(loader->fetchExternalResource(
                  "https://upload.wikimedia.org/wikipedia/commons/4/4f/SVG_Logo.svg"),
              testing::VariantWith<std::vector<uint8_t>>(testing::ElementsAre('s', 'v', 'g')));
  EXPECT_THAT(
      loader->fetchExternalResource("relative.svg"),
      testing::VariantWith<std::vector<uint8_t>>(testing::ElementsAre('l', 'o', 'c', 'a', 'l')));
  for (const std::string_view url :
       {"https://example.invalid/SVG_Logo.svg",
        "http://upload.wikimedia.org/wikipedia/commons/4/4f/SVG_Logo.svg",
        "https://upload.wikimedia.org/wikipedia/commons/4/4f/SVG_Logo.svg?different=1"}) {
    SCOPED_TRACE(url);
    EXPECT_THAT(loader->fetchExternalResource(url),
                testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::NotFound));
  }
  EXPECT_THAT(loader->fetchExternalResource("../outside.svg"),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::SandboxViolation));
  auto missing =
      CreateResvgResourceLoaderForTesting(local, local / "document.svg", root / "missing");
  EXPECT_THAT(missing->fetchExternalResource(
                  "https://upload.wikimedia.org/wikipedia/commons/4/4f/SVG_Logo.svg"),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::NotFound));
  {
    std::ofstream(snapshots / "SVG_Logo.svg", std::ios::binary | std::ios::trunc)
        << std::string(512 * 1024 + 1, 'x');
  }
  EXPECT_THAT(loader->fetchExternalResource(
                  "https://upload.wikimedia.org/wikipedia/commons/4/4f/SVG_Logo.svg"),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::TooLarge));
}

testing::TestParamInfo<ImageComparisonTestParam> MakeParamInfo(ImageComparisonParams params) {
  ImageComparisonTestcase testcase;
  testcase.svgFilename = "embedded-png.svg";
  testcase.params = params;

  return testing::TestParamInfo<ImageComparisonTestParam>(
      ImageComparisonTestParam{testcase, ComparisonMode::TinyGolden}, 0);
}

TEST(ImageComparisonTestFixtureTests, SimpleTextGeodeGateKeepsTheFullTextComparison) {
  // Whether each comparison runs: TinySkia in the simple and full text tiers, then Geode in both.
  const auto allowed = [](const ImageComparisonParams& params) {
    return std::vector<bool>{params.allowsBackend(RendererBackend::TinySkia, false),
                             params.allowsBackend(RendererBackend::TinySkia, true),
                             params.allowsBackend(RendererBackend::Geode, false),
                             params.allowsBackend(RendererBackend::Geode, true)};
  };

  EXPECT_THAT(allowed(ImageComparisonParams().disableGeodeForSimpleText("simple tier only")),
              testing::ElementsAre(true, true, false, true));
  EXPECT_THAT(allowed(ImageComparisonParams().disableBackend(RendererBackend::Geode, "both")),
              testing::ElementsAre(true, true, false, false));
  EXPECT_THAT(allowed(ImageComparisonParams()), testing::ElementsAre(true, true, true, true));
}

TEST(ImageComparisonTestFixtureTests, ExplicitSkipsUseDisabledGtestName) {
  const std::string name =
      TestNameFromFilename(MakeParamInfo(ImageComparisonParams::Skip("triaged gap")));

  EXPECT_THAT(name, testing::StartsWith("DISABLED_"));
  EXPECT_THAT(name, testing::HasSubstr("embedded_png"));
}

// Render-only state is private, so only the named factories can skip a case's comparison.
template <typename T>
concept HasPublicRenderOnlyField =
    requires(T params) { params.renderOnly; } || requires(T params) { params.renderOnly_; };
static_assert(!HasPublicRenderOnlyField<ImageComparisonParams>);

TEST(ImageComparisonTestFixtureTests, OnlyNamedFactoriesRenderWithoutComparing) {
  EXPECT_FALSE(ImageComparisonParams().isRenderOnly());
  EXPECT_FALSE(ImageComparisonParams::Skip("bug").isRenderOnly());
  EXPECT_FALSE(ImageComparisonParams::WithThreshold(0.1f).isRenderOnly());

  const ImageComparisonParams undefinedBehavior =
      ImageComparisonParams::UndefinedBehavior("ub").withReason("chained");
  EXPECT_TRUE(undefinedBehavior.isRenderOnly());
  EXPECT_EQ(undefinedBehavior.renderOnlyBucket(), "undefined behavior");

  const ImageComparisonParams deprecated = ImageComparisonParams::DeprecatedInSVG2("<tref>");
  EXPECT_TRUE(deprecated.isRenderOnly());
  EXPECT_EQ(deprecated.renderOnlyBucket(), "deprecated in SVG 2");
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

TEST(ImageComparisonTestFixtureTests, TextFullOnlyRunsUseDisabledGtestNameInSimpleTextBuild) {
  if (IsFullTextBuiltIn()) {
    GTEST_SKIP() << "Full text is built in; text-full-only cases run in this configuration";
  }

  ImageComparisonParams params;
  params.onlyTextFull();

  const std::string name = TestNameFromFilename(MakeParamInfo(params));

  EXPECT_THAT(name, testing::StartsWith("DISABLED_"));
}

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
