/// @file
/// Byte-identical render checks for the retained path-data parse in `ShapeSystem`.
///
/// The shape pass keeps the spline it parsed from a `<path>`'s resolved path-data string and
/// skips the parse while that string is unchanged. Every way the resolved string can change has
/// to produce exactly the pixels that a document parsed from scratch with the new value
/// produces. Each test renders a live document once so the parse is retained, mutates it, and
/// compares its next frame against a separately parsed document: the retained parse lives on the
/// mutated document, so only a document that never saw the mutation can show what the new value
/// is supposed to look like.
///
/// Every comparison is paired with a check that the expected pixels actually differ from the
/// unmutated ones, so a mutation that silently did nothing cannot pass by both sides agreeing on
/// the old picture.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "donner/base/Path.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

SVGDocument Parse(std::string_view fragment) {
  return instantiateSubtree(fragment, parser::SVGParser::Options(), Vector2i(16, 16));
}

SVGDocument ParseAnimated(std::string_view fragment) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;
  return instantiateSubtree(fragment, options, Vector2i(16, 16));
}

std::string Render(SVGDocument document) {
  return RendererTestUtils::renderToAsciiImage(document).generated;
}

/// The pixels a document parsed from scratch produces.
std::string RenderFresh(std::string_view fragment) {
  return Render(Parse(fragment));
}

/// Look up `#p`, which every fragment here names as the path under test.
SVGPathElement TargetPath(SVGDocument& document) {
  std::optional<SVGElement> element = document.querySelector("#p");
  EXPECT_TRUE(element.has_value());
  return element->cast<SVGPathElement>();
}

constexpr std::string_view kInitialPath =
    R"(<path id="p" d="M2 2 L14 2 L14 6 L2 6 Z" fill="black" />)";

}  // namespace

TEST(ShapeParseCache, RepeatedRenderOfUnchangedDocumentIsStable) {
  SVGDocument document = Parse(kInitialPath);

  const std::string first = Render(document);
  EXPECT_EQ(Render(document), first);
  EXPECT_EQ(Render(document), first);
}

TEST(ShapeParseCache, SetDMatchesFreshDocument) {
  constexpr std::string_view kAfter =
      R"(<path id="p" d="M2 8 L14 8 L14 14 L2 14 Z" fill="black" />)";

  SVGDocument document = Parse(kInitialPath);
  const std::string before = Render(document);
  const std::string expected = RenderFresh(kAfter);
  ASSERT_NE(expected, before);

  TargetPath(document).setD(RcString("M2 8 L14 8 L14 14 L2 14 Z"));

  EXPECT_EQ(Render(document), expected);
}

TEST(ShapeParseCache, SetAttributeDMatchesFreshDocument) {
  constexpr std::string_view kAfter =
      R"(<path id="p" d="M6 2 L10 2 L10 14 L6 14 Z" fill="black" />)";

  SVGDocument document = Parse(kInitialPath);
  const std::string before = Render(document);
  const std::string expected = RenderFresh(kAfter);
  ASSERT_NE(expected, before);

  TargetPath(document).setAttribute("d", "M6 2 L10 2 L10 14 L6 14 Z");

  EXPECT_EQ(Render(document), expected);
}

TEST(ShapeParseCache, RemoveAttributeDMatchesFreshDocument) {
  constexpr std::string_view kAfter = R"(<path id="p" fill="black" />)";

  SVGDocument document = Parse(kInitialPath);
  const std::string before = Render(document);
  const std::string expected = RenderFresh(kAfter);
  ASSERT_NE(expected, before);

  TargetPath(document).removeAttribute("d");

  EXPECT_EQ(Render(document), expected);
}

TEST(ShapeParseCache, StyleAttributeDMatchesFreshDocument) {
  constexpr std::string_view kAfter =
      R"(<path id="p" d="M2 2 L14 2 L14 6 L2 6 Z" fill="black"
                style="d: 'M2 8 L14 8 L14 14 L2 14 Z'" />)";

  SVGDocument document = Parse(kInitialPath);
  const std::string before = Render(document);
  const std::string expected = RenderFresh(kAfter);
  ASSERT_NE(expected, before);

  // A CSS `d` declaration does not drop the computed path, so this is the mutation class the
  // retained parse has to notice on its own rather than through an invalidation.
  TargetPath(document).setStyle("d: 'M2 8 L14 8 L14 14 L2 14 Z'");

  EXPECT_EQ(Render(document), expected);
}

TEST(ShapeParseCache, SetSplineThenSetDMatchesFreshDocument) {
  constexpr std::string_view kOverridden =
      R"(<path id="p" d="M2 8 L14 8 L14 14 L2 14 Z" fill="black" />)";

  SVGDocument document = Parse(kInitialPath);
  const std::string before = Render(document);
  const std::string overridden = RenderFresh(kOverridden);
  ASSERT_NE(overridden, before);

  TargetPath(document).setSpline(PathBuilder()
                                     .moveTo(Vector2d(2.0, 8.0))
                                     .lineTo(Vector2d(14.0, 8.0))
                                     .lineTo(Vector2d(14.0, 14.0))
                                     .lineTo(Vector2d(2.0, 14.0))
                                     .closePath()
                                     .build());
  EXPECT_EQ(Render(document), overridden);

  // The retained spline was supplied directly rather than parsed, so returning to the original
  // path data must parse it again instead of matching it against a spline it never produced.
  TargetPath(document).setD(RcString("M2 2 L14 2 L14 6 L2 6 Z"));
  EXPECT_EQ(Render(document), before);
}

TEST(ShapeParseCache, UseShadowInstanceFollowsMutation) {
  constexpr std::string_view kBefore = R"svg(
    <path id="p" d="M2 2 L6 2 L6 6 L2 6 Z" fill="black" />
    <use href="#p" transform="translate(8, 8)" />
  )svg";
  constexpr std::string_view kAfter = R"svg(
    <path id="p" d="M2 2 L6 2 L6 10 L2 10 Z" fill="black" />
    <use href="#p" transform="translate(8, 8)" />
  )svg";

  SVGDocument document = Parse(kBefore);
  const std::string before = Render(document);
  const std::string expected = RenderFresh(kAfter);
  ASSERT_NE(expected, before);

  // A `<use>` instance renders the referenced element's computed geometry, so a stale retained
  // parse would show up on the original and on every instance of it.
  TargetPath(document).setD(RcString("M2 2 L6 2 L6 10 L2 10 Z"));

  EXPECT_EQ(Render(document), expected);
}

TEST(ShapeParseCache, AnimatedPathDataMatchesFreshDocumentAtEachTime) {
  constexpr std::string_view kFragment = R"svg(
    <path id="p" d="M2 2 L14 2 L14 6 L2 6 Z" fill="black">
      <set attributeName="d" to="M2 8 L14 8 L14 14 L2 14 Z" begin="1s" dur="1s" />
    </path>
  )svg";

  SVGDocument document = ParseAnimated(kFragment);
  document.setTime(0.0);
  const std::string atStart = Render(document);

  // Before the animation begins, while it is applied, and after it reverts. Each sample of the
  // live document has to match a document that was parsed fresh and sampled at the same time.
  bool sawDifferentFrame = false;
  for (const double time : {0.0, 1.5, 3.0}) {
    document.setTime(time);

    SVGDocument fresh = ParseAnimated(kFragment);
    fresh.setTime(time);

    const std::string expected = Render(fresh);
    EXPECT_EQ(Render(document), expected) << "at time " << time;
    sawDifferentFrame = sawDifferentFrame || expected != atStart;
  }

  // The animation has to actually move the path at one of the sampled times, otherwise the
  // comparisons above would hold for a document that ignored it entirely.
  EXPECT_TRUE(sawDifferentFrame);
}

}  // namespace donner::svg
