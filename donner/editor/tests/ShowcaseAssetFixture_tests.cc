/// @file
/// Fixture tests for the v0.8 showcase assets.
///
/// Two assets are validated:
///
///   - `donner_splash_v0_8_editable.svg`: the editable intermediate.
///     This test (`EditableSplashParsesWithNonEmptyViewBox`) is the original
///     "fails loudly if the planned source asset is missing or invalid" gate.
///   - `donner_splash_v0_8.svg`: the FINAL outlined splash, produced
///     by `//donner/editor/tools:generate_showcase_asset` from the editable
///     intermediate (add `<text>SVG</text>` → Convert Text to Outlines → Export
///     Viewport as SVG with overlay enabled). These tests assert the
///     final-asset invariants: it parses + renders in Donner, contains
///     outlined `<path>` geometry (the `showcase_svg_label_outlines` group with
///     the `data-donner-converted-from="text"` marker), contains NO live `<text>`
///     for the `SVG` letters, and carries the exported `id="donner-editor-overlay"`
///     chrome (the showcase overlay variant).

#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include "donner/base/Box.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/parser/SVGParser.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::svg {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

constexpr const char* kEditableSplashPath = "donner_splash_v0_8_editable.svg";
constexpr const char* kFinalSplashPath = "donner_splash_v0_8.svg";

std::string ReadRunfile(const char* path) {
  std::ifstream stream(path);
  if (!stream.is_open()) {
    return {};
  }
  std::ostringstream buf;
  buf << stream.rdbuf();
  return buf.str();
}

TEST(ShowcaseAssetFixture, EditableSplashParsesWithNonEmptyViewBox) {
  const std::string source = ReadRunfile(kEditableSplashPath);
  ASSERT_FALSE(source.empty())
      << kEditableSplashPath
      << " not found in runfiles or empty. The showcase requires the editable "
         "intermediate to ship alongside the provenance notes.";

  ParseWarningSink warningSink = ParseWarningSink::Disabled();
  auto result = parser::SVGParser::ParseSVG(source, warningSink);
  ASSERT_FALSE(result.hasError()) << "SVGParser rejected " << kEditableSplashPath << ": "
                                  << result.error();

  SVGDocument document = std::move(result).result();
  const std::optional<Box2d> viewBox = document.svgElement().viewBox();
  ASSERT_TRUE(viewBox.has_value())
      << "Root <svg> in " << kEditableSplashPath << " is missing a viewBox attribute.";
  EXPECT_GT(viewBox->width(), 0.0) << "viewBox width must be positive: " << viewBox->width();
  EXPECT_GT(viewBox->height(), 0.0) << "viewBox height must be positive: " << viewBox->height();
}

// The final v0.8 showcase SVG parses in Donner and exposes a non-empty viewBox.
TEST(ShowcaseAssetFixture, FinalSplashParsesWithNonEmptyViewBox) {
  const std::string source = ReadRunfile(kFinalSplashPath);
  ASSERT_FALSE(source.empty()) << kFinalSplashPath
                               << " not found in runfiles or empty. Milestone 8 requires the final "
                                  "outlined showcase asset to be checked in. Regenerate it with "
                                  "//donner/editor/tools:generate_showcase_asset.";

  ParseWarningSink warningSink = ParseWarningSink::Disabled();
  auto result = parser::SVGParser::ParseSVG(source, warningSink);
  ASSERT_FALSE(result.hasError()) << "SVGParser rejected " << kFinalSplashPath << ": "
                                  << result.error();

  SVGDocument document = std::move(result).result();
  const std::optional<Box2d> viewBox = document.svgElement().viewBox();
  ASSERT_TRUE(viewBox.has_value())
      << "Root <svg> in " << kFinalSplashPath << " is missing a viewBox attribute.";
  EXPECT_GT(viewBox->width(), 0.0) << "viewBox width must be positive: " << viewBox->width();
  EXPECT_GT(viewBox->height(), 0.0) << "viewBox height must be positive: " << viewBox->height();
}

// The final showcase contains the outlined `SVG` lettering (the converted
// outline group with `<path>` glyph geometry) and NO live `<text>` element.
TEST(ShowcaseAssetFixture, FinalSplashHasOutlinedLettersAndNoLiveText) {
  const std::string source = ReadRunfile(kFinalSplashPath);
  ASSERT_FALSE(source.empty()) << kFinalSplashPath << " not found in runfiles or empty.";

  // Convert Text to Outlines markers from the generator's conversion step.
  EXPECT_THAT(source, HasSubstr("id=\"showcase_svg_label_outlines\""))
      << "final asset must contain the converted outline group";
  EXPECT_THAT(source, HasSubstr("data-donner-converted-from=\"text\""))
      << "final asset must carry the text-to-outline conversion marker";
  EXPECT_THAT(source, HasSubstr("<path")) << "final asset must contain outline <path> geometry";

  // No live <text> remains anywhere: the editable splash had no text of its own,
  // and the inserted `SVG` label was converted to outlines.
  EXPECT_THAT(source, Not(HasSubstr("<text")))
      << "final asset must not contain any live <text> element";

  // Parsed DOM confirms: outline group present, no <text> element.
  ParseWarningSink warningSink = ParseWarningSink::Disabled();
  auto result = parser::SVGParser::ParseSVG(source, warningSink);
  ASSERT_FALSE(result.hasError()) << result.error();
  SVGDocument document = std::move(result).result();
  EXPECT_TRUE(document.querySelector("#showcase_svg_label_outlines").has_value());
  EXPECT_FALSE(document.querySelector("text").has_value())
      << "final asset must contain no live <text> in the parsed DOM";
}

// The exported overlay group is present (the showcase overlay variant).
TEST(ShowcaseAssetFixture, FinalSplashHasExportedOverlayGroup) {
  const std::string source = ReadRunfile(kFinalSplashPath);
  ASSERT_FALSE(source.empty()) << kFinalSplashPath << " not found in runfiles or empty.";

  EXPECT_THAT(source, HasSubstr("id=\"donner-editor-overlay\""))
      << "final asset must carry the exported editor overlay chrome group";

  ParseWarningSink warningSink = ParseWarningSink::Disabled();
  auto result = parser::SVGParser::ParseSVG(source, warningSink);
  ASSERT_FALSE(result.hasError()) << result.error();
  SVGDocument document = std::move(result).result();
  EXPECT_TRUE(document.querySelector("#donner-editor-overlay").has_value())
      << "overlay group must be a resolvable element in the parsed DOM";
}

}  // namespace
}  // namespace donner::svg
