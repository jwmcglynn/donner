/// @file
/// End-to-end tests for the on-demand v0.8 showcase generator.
///
/// The repository keeps only `donner_splash.svg`. These tests generate the
/// derived showcase in memory through the same text-to-outlines and viewport
/// export paths as the editor, then assert the generated-output invariants.

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "donner/base/Box.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/editor/tools/GenerateShowcaseAsset.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::svg {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

constexpr const char* kSplashPath = "donner_splash.svg";
constexpr const char* kShowcaseGoldenPath =
    "donner/editor/tests/testdata/showcase_asset_tiny_skia.png";

std::string ReadRunfile(const char* path) {
  std::ifstream stream(path);
  if (!stream.is_open()) {
    return {};
  }
  std::ostringstream buf;
  buf << stream.rdbuf();
  return buf.str();
}

TEST(ShowcaseAssetFixture, CanonicalSplashParsesWithNonEmptyViewBox) {
  const std::string source = ReadRunfile(kSplashPath);
  ASSERT_FALSE(source.empty())
      << kSplashPath
      << " not found in runfiles or empty. The showcase generator requires the "
         "canonical splash as its input.";

  ParseWarningSink warningSink = ParseWarningSink::Disabled();
  auto result = parser::SVGParser::ParseSVG(source, warningSink);
  ASSERT_FALSE(result.hasError()) << "SVGParser rejected " << kSplashPath << ": " << result.error();

  SVGDocument document = std::move(result).result();
  const std::optional<Box2d> viewBox = document.svgElement().viewBox();
  ASSERT_TRUE(viewBox.has_value())
      << "Root <svg> in " << kSplashPath << " is missing a viewBox attribute.";
  EXPECT_GT(viewBox->width(), 0.0) << "viewBox width must be positive: " << viewBox->width();
  EXPECT_GT(viewBox->height(), 0.0) << "viewBox height must be positive: " << viewBox->height();
}

TEST(ShowcaseAssetFixture, GeneratesOutlinedOverlayShowcaseOnDemand) {
  const std::string baseSource = ReadRunfile(kSplashPath);
  ASSERT_FALSE(baseSource.empty()) << kSplashPath << " not found in runfiles or empty.";

  const auto generated = editor::GenerateShowcaseAsset(baseSource);
  ASSERT_TRUE(generated.ok()) << generated.error;
  const std::string& source = generated.value;

  ParseWarningSink warningSink = ParseWarningSink::Disabled();
  auto result = parser::SVGParser::ParseSVG(source, warningSink);
  ASSERT_FALSE(result.hasError()) << "SVGParser rejected the generated showcase: "
                                  << result.error();

  SVGDocument document = std::move(result).result();
  const std::optional<Box2d> viewBox = document.svgElement().viewBox();
  ASSERT_TRUE(viewBox.has_value()) << "Generated root <svg> is missing a viewBox attribute.";
  EXPECT_GT(viewBox->width(), 0.0) << "viewBox width must be positive: " << viewBox->width();
  EXPECT_GT(viewBox->height(), 0.0) << "viewBox height must be positive: " << viewBox->height();
  // Convert Text to Outlines markers from the generator's conversion step.
  EXPECT_THAT(source, HasSubstr("id=\"showcase_svg_label_outlines\""))
      << "generated showcase must contain the converted outline group";
  EXPECT_THAT(source, HasSubstr("data-donner-converted-from=\"text\""))
      << "generated showcase must carry the text-to-outline conversion marker";
  EXPECT_THAT(source, HasSubstr("id=\"showcase_svg_label_outlines_0\""))
      << "generated showcase must contain a path inside the outline group";

  // No live <text> remains anywhere: the canonical splash has no text of its own,
  // and the inserted `SVG` label was converted to outlines.
  EXPECT_THAT(source, Not(HasSubstr("<text")))
      << "generated showcase must not contain any live <text> element";

  // Parsed DOM confirms the outline and overlay groups are present, with no
  // live <text> element.
  EXPECT_TRUE(document.querySelector("#showcase_svg_label_outlines").has_value());
  EXPECT_FALSE(document.querySelector("text").has_value())
      << "generated showcase must contain no live <text> in the parsed DOM";
  EXPECT_THAT(source, HasSubstr("id=\"donner-editor-overlay\""))
      << "generated showcase must carry the exported editor overlay chrome group";
  EXPECT_TRUE(document.querySelector("#donner-editor-overlay").has_value())
      << "overlay group must be a resolvable element in the parsed DOM";

  // Render through the deterministic CPU facade as the final end-to-end gate.
  constexpr Vector2i kRenderSize(223, 128);
  document.setCanvasSize(kRenderSize.x, kRenderSize.y);
  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap bitmap = renderer.takeSnapshot();
  ASSERT_FALSE(bitmap.empty()) << "generated showcase produced no renderer snapshot";
  EXPECT_EQ(bitmap.dimensions.x, kRenderSize.x);
  EXPECT_EQ(bitmap.dimensions.y, kRenderSize.y);
  ASSERT_GE(bitmap.rowBytes, static_cast<std::size_t>(bitmap.dimensions.x) * 4u);
  editor::tests::CompareBitmapToGolden(bitmap, kShowcaseGoldenPath, "generated_showcase_tiny_skia",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(ShowcaseAssetFixture, RejectsInputWithLiveText) {
  constexpr std::string_view kInput =
      R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
  <text x="10" y="20">existing</text>
</svg>)";

  const auto generated = editor::GenerateShowcaseAsset(kInput);
  ASSERT_FALSE(generated.ok());
  EXPECT_THAT(generated.error, HasSubstr("must not contain live <text>"));
}

TEST(ShowcaseAssetFixture, RejectsReservedGeneratorIds) {
  constexpr std::string_view kInput =
      R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
  <g id="showcase_svg_label_outlines"/>
</svg>)";

  const auto generated = editor::GenerateShowcaseAsset(kInput);
  ASSERT_FALSE(generated.ok());
  EXPECT_THAT(generated.error, HasSubstr("reserved id #showcase_svg_label_outlines"));
}

TEST(ShowcaseAssetFixture, RejectsGeneratedOutlinePathIdCollision) {
  constexpr std::string_view kInput =
      R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
  <path id="showcase_svg_label_outlines_0" d="M0 0h1v1z"/>
</svg>)";

  const auto generated = editor::GenerateShowcaseAsset(kInput);
  ASSERT_FALSE(generated.ok());
  EXPECT_THAT(generated.error, HasSubstr("reserved id #showcase_svg_label_outlines_0"));
}

TEST(ShowcaseAssetFixture, RejectsNonFiniteViewBox) {
  constexpr std::string_view kInput =
      R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="1e308 0 1e308 100"/>)";

  const auto generated = editor::GenerateShowcaseAsset(kInput);
  ASSERT_FALSE(generated.ok());
  EXPECT_THAT(generated.error, HasSubstr("finite coordinates and dimensions"));
}

TEST(ShowcaseAssetFixture, RejectsViewBoxLargerThanViewportIntegerRange) {
  constexpr std::string_view kInput =
      R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 2147483648 100"/>)";

  const auto generated = editor::GenerateShowcaseAsset(kInput);
  ASSERT_FALSE(generated.ok());
  EXPECT_THAT(generated.error, HasSubstr("supported viewport dimension range"));
}

}  // namespace
}  // namespace donner::svg
