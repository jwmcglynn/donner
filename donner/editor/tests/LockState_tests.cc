#include "donner/editor/LockState.h"

#include <gtest/gtest.h>

#include <optional>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::editor {
namespace {

svg::SVGDocument ParseSvg(std::string_view source) {
  ParseWarningSink sink;
  // The lock marker is a user-defined `data-*` attribute, which the parser
  // drops by default; keep it so `getAttribute` can read it back (mirrors how
  // the editor loads documents).
  svg::parser::SVGParser::Options options;
  options.disableUserAttributes = false;
  auto result = svg::parser::SVGParser::ParseSVG(source, sink, options);
  EXPECT_FALSE(result.hasError());
  return std::move(result).result();
}

svg::SVGElement ElementById(svg::SVGDocument& document, std::string_view id) {
  auto element = document.querySelector(id);
  EXPECT_TRUE(element.has_value()) << "no element matching " << id;
  return *element;
}

TEST(LockStateTest, UnlockedElementHasNoLockedAncestor) {
  auto document = ParseSvg(R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="r" x="0" y="0" width="10" height="10"/>
  </svg>)");
  const svg::SVGElement rect = ElementById(document, "#r");

  EXPECT_FALSE(IsLocked(rect));
  EXPECT_FALSE(LockedAncestor(rect).has_value());
}

TEST(LockStateTest, DirectlyLockedElementIsItsOwnLockedAncestor) {
  auto document = ParseSvg(R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="r" data-donner-locked="true" x="0" y="0" width="10" height="10"/>
  </svg>)");
  const svg::SVGElement rect = ElementById(document, "#r");

  EXPECT_TRUE(IsLocked(rect));
  const std::optional<svg::SVGElement> ancestor = LockedAncestor(rect);
  ASSERT_TRUE(ancestor.has_value());
  EXPECT_EQ(ancestor->id(), "r");
}

TEST(LockStateTest, LockedAncestorResolvesToTheMarkedGroup) {
  // `<rect>` inside an unmarked `<g id="inner">` inside a marked `<g id="grp">`:
  // the locked ancestor is the marked group, skipping the unmarked one.
  auto document = ParseSvg(R"(<svg xmlns="http://www.w3.org/2000/svg">
    <g id="grp" data-donner-locked="true">
      <g id="inner">
        <rect id="r" x="0" y="0" width="10" height="10"/>
      </g>
    </g>
  </svg>)");
  const svg::SVGElement rect = ElementById(document, "#r");

  EXPECT_TRUE(IsLocked(rect));
  const std::optional<svg::SVGElement> ancestor = LockedAncestor(rect);
  ASSERT_TRUE(ancestor.has_value());
  EXPECT_EQ(ancestor->id(), "grp") << "must walk to the nearest ancestor carrying the lock marker";
}

TEST(LockStateTest, FalseMarkerValueDoesNotLock) {
  auto document = ParseSvg(R"(<svg xmlns="http://www.w3.org/2000/svg">
    <g id="grp" data-donner-locked="false">
      <rect id="r" x="0" y="0" width="10" height="10"/>
    </g>
  </svg>)");
  const svg::SVGElement rect = ElementById(document, "#r");

  EXPECT_FALSE(IsLocked(rect));
  EXPECT_FALSE(LockedAncestor(rect).has_value());
}

}  // namespace
}  // namespace donner::editor
