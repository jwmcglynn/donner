/// @file
/// Milestone 1 fixture test for the v0.8 showcase (design doc:
/// `docs/design_docs/0047-v0_8_showcase.md`).
///
/// Asserts that the editable showcase intermediate
/// `donner_splash_v0_8_editable.svg` is present in runfiles, parses through
/// `donner::svg::parser::SVGParser` without errors, and exposes a non-empty
/// root viewBox. This is the "fails loudly if the planned source asset is
/// missing or invalid" gate from the milestone's checklist, and stays in place
/// once the later milestones produce the final outlined `donner_splash_v0_8.svg`.

#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include "donner/base/Box.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/parser/SVGParser.h"
#include "gtest/gtest.h"

namespace donner::svg {
namespace {

constexpr const char* kEditableSplashPath = "donner_splash_v0_8_editable.svg";

std::string ReadEditableSplash() {
  std::ifstream stream(kEditableSplashPath);
  if (!stream.is_open()) {
    return {};
  }
  std::ostringstream buf;
  buf << stream.rdbuf();
  return buf.str();
}

TEST(ShowcaseAssetFixture, EditableSplashParsesWithNonEmptyViewBox) {
  const std::string source = ReadEditableSplash();
  ASSERT_FALSE(source.empty())
      << kEditableSplashPath
      << " not found in runfiles or empty. Milestone 1 requires the editable "
         "intermediate to ship alongside the design doc and provenance notes.";

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

}  // namespace
}  // namespace donner::svg
