#include "donner/editor/AttributeWriteback.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>

#include "donner/editor/TextPatch.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::editor {
namespace {

using testing::Eq;

constexpr std::string_view kSimpleSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
  <rect id="r1" x="10" y="20" width="50" height="30" fill="red"/>
</svg>)svg";

constexpr std::string_view kGroupedCircleSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
  <g id="group">
    <circle id="c1" cx="40" cy="40" r="10"/>
  </g>
</svg>)svg";

constexpr std::string_view kGroupedCircleSourceShiftedBeforeCircle =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
  <g id="group" data-note="shifted">
    <circle id="c1" cx="40" cy="40" r="10"/>
  </g>
</svg>)svg";

constexpr std::string_view kParentTransformSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
  <g id="group" transform="translate(10, 0)">
    <circle id="c1" cx="40" cy="40" r="10"/>
  </g>
</svg>)svg";

std::optional<svg::SVGDocument> ParseDocument(std::string_view source) {
  ParseWarningSink sink;
  auto result = svg::parser::SVGParser::ParseSVG(source, sink);
  if (result.hasError()) {
    return std::nullopt;
  }

  return std::move(result.result());
}

svg::SVGElement GetElementById(svg::SVGDocument& document, std::string_view selector) {
  auto element = document.querySelector(selector);
  EXPECT_TRUE(element.has_value()) << "no element matching " << selector;
  return *element;
}

std::size_t CountSubstring(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) {
    return 0;
  }

  std::size_t count = 0;
  std::size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
    ++count;
    pos += needle.size();
  }

  return count;
}

class AttributeWritebackTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto document = ParseDocument(kSimpleSvg);
    ASSERT_TRUE(document.has_value());
    document_ = std::move(*document);

    auto rect = document_.querySelector("#r1");
    ASSERT_TRUE(rect.has_value());
    rect_ = *rect;
  }

  svg::SVGDocument document_;
  std::optional<svg::SVGElement> rect_;  // set in SetUp
};

TEST_F(AttributeWritebackTest, UpdateExistingAttribute) {
  auto patch = buildAttributeWriteback(kSimpleSvg, *rect_, "fill", "blue");
  ASSERT_TRUE(patch.has_value());

  // Apply the patch to a copy of the source and verify the result.
  std::string source(kSimpleSvg);
  auto result = applyPatches(source, {{*patch}});
  EXPECT_EQ(result.applied, 1u);

  // The updated source should have fill="blue" instead of fill="red".
  EXPECT_THAT(source, testing::HasSubstr("fill=\"blue\""));
  EXPECT_THAT(source, testing::Not(testing::HasSubstr("fill=\"red\"")));

  // Other attributes must be preserved.
  EXPECT_THAT(source, testing::HasSubstr("x=\"10\""));
  EXPECT_THAT(source, testing::HasSubstr("id=\"r1\""));
}

TEST_F(AttributeWritebackTest, InsertNewAttribute) {
  auto patch = buildAttributeWriteback(kSimpleSvg, *rect_, "stroke", "green");
  ASSERT_TRUE(patch.has_value());

  std::string source(kSimpleSvg);
  auto result = applyPatches(source, {{*patch}});
  EXPECT_EQ(result.applied, 1u);

  // The new attribute should appear somewhere in the <rect> tag.
  EXPECT_THAT(source, testing::HasSubstr("stroke=\"green\""));
  // Existing attributes must be preserved.
  EXPECT_THAT(source, testing::HasSubstr("fill=\"red\""));
}

TEST_F(AttributeWritebackTest, ValueWithSpecialCharsIsEscaped) {
  auto patch = buildAttributeWriteback(kSimpleSvg, *rect_, "fill", "a<b&c");
  ASSERT_TRUE(patch.has_value());

  std::string source(kSimpleSvg);
  auto result = applyPatches(source, {{*patch}});
  EXPECT_EQ(result.applied, 1u);

  // The special characters must be XML-escaped in the output.
  EXPECT_THAT(source, testing::HasSubstr("fill=\"a&lt;b&amp;c\""));
}

TEST_F(AttributeWritebackTest, UpdatedSourceReparses) {
  // The round-trip: update an attribute, then re-parse the modified source
  // and verify the DOM reflects the new value.
  auto patch = buildAttributeWriteback(kSimpleSvg, *rect_, "fill", "blue");
  ASSERT_TRUE(patch.has_value());

  std::string source(kSimpleSvg);
  applyPatches(source, {{*patch}});

  ParseWarningSink sink;
  auto result = svg::parser::SVGParser::ParseSVG(source, sink);
  ASSERT_TRUE(result.hasResult()) << "Re-parse failed for: " << source;

  auto rect = result.result().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  EXPECT_THAT(rect->getAttribute("fill"), testing::Optional(RcString("blue")));
}

TEST_F(AttributeWritebackTest, NulValueReturnsNullopt) {
  // A value containing NUL can't be XML-escaped - buildAttributeWriteback
  // must return nullopt rather than producing invalid XML.
  auto patch = buildAttributeWriteback(kSimpleSvg, *rect_, "fill", std::string_view("a\0b", 3));
  EXPECT_FALSE(patch.has_value());
}

TEST_F(AttributeWritebackTest, StaleSourceOpeningTagGuardFailuresReturnNullopt) {
  const std::size_t rectStart = kSimpleSvg.find("<rect");
  ASSERT_NE(rectStart, std::string_view::npos);

  std::string tooShort(rectStart + 1, ' ');
  tooShort[rectStart] = '<';
  EXPECT_FALSE(buildAttributeWriteback(tooShort, *rect_, "fill", "blue").has_value());

  std::string incompleteName(kSimpleSvg.substr(0, rectStart + std::string_view("<rect").size()));
  EXPECT_FALSE(buildAttributeWriteback(incompleteName, *rect_, "fill", "blue").has_value());

  std::string wrongName(kSimpleSvg);
  wrongName.replace(rectStart + 1, 4, "path");
  EXPECT_FALSE(buildAttributeWriteback(wrongName, *rect_, "fill", "blue").has_value());

  std::string badTerminator(kSimpleSvg);
  badTerminator[rectStart + std::string_view("<rect").size()] = 'x';
  EXPECT_FALSE(buildAttributeWriteback(badTerminator, *rect_, "fill", "blue").has_value());
}

TEST_F(AttributeWritebackTest, StaleSourceWithExpectedOpenButNoCloseReturnsNullopt) {
  const std::size_t rectStart = kSimpleSvg.find("<rect");
  ASSERT_NE(rectStart, std::string_view::npos);

  std::string truncated(kSimpleSvg.substr(0, rectStart + std::string_view("<rect ").size()));
  EXPECT_FALSE(buildAttributeWriteback(truncated, *rect_, "stroke", "green").has_value());
}

TEST(AttributeWritebackRegressionTest, ElementOverloadAcceptsOpeningTagTerminators) {
  constexpr std::string_view kSources[] = {
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect/></svg>)",
      "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect\tid=\"r1\"/></svg>",
      "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect\nid=\"r1\"/></svg>",
      "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect\rid=\"r1\"/></svg>",
  };

  for (std::string_view testSource : kSources) {
    SCOPED_TRACE(testSource);
    auto document = ParseDocument(testSource);
    ASSERT_TRUE(document.has_value());
    auto rect = document->svgElement().firstChild();
    ASSERT_TRUE(rect.has_value());

    auto patch = buildAttributeWriteback(testSource, *rect, "fill", "blue");
    ASSERT_TRUE(patch.has_value());

    std::string source(testSource);
    const auto result = applyPatches(source, {{*patch}});
    ASSERT_EQ(result.applied, 1u);
    EXPECT_THAT(source, testing::HasSubstr(R"(fill="blue")"));
  }
}

TEST(AttributeWritebackRegressionTest, PrefixedSvgElementWritebackUsesQualifiedTagName) {
  constexpr std::string_view kSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:svg="http://www.w3.org/2000/svg"><svg:rect svg:id="nsRect"/></svg>)";
  auto document = ParseDocument(kSource);
  ASSERT_TRUE(document.has_value());
  auto prefixedRect = document->svgElement().firstChild();
  ASSERT_TRUE(prefixedRect.has_value());
  EXPECT_EQ(prefixedRect->tagName(), (xml::XMLQualifiedNameRef("svg", "rect")));

  auto patch = buildAttributeWriteback(kSource, *prefixedRect, "fill", "blue");
  ASSERT_TRUE(patch.has_value());

  std::string source(kSource);
  const auto result = applyPatches(source, {{*patch}});
  ASSERT_EQ(result.applied, 1u);
  EXPECT_THAT(source, testing::HasSubstr(R"(<svg:rect svg:id="nsRect" fill="blue"/>)"));
}

TEST(AttributeWritebackRegressionTest, TargetCorrectnessUsesCircleNotAncestorGroup) {
  auto document = ParseDocument(kGroupedCircleSvg);
  ASSERT_TRUE(document.has_value());
  svg::SVGElement circle = GetElementById(*document, "#c1");

  auto patch = buildAttributeWriteback(kGroupedCircleSourceShiftedBeforeCircle, circle, "transform",
                                       "translate(5)");
  ASSERT_TRUE(patch.has_value());

  std::string source(kGroupedCircleSourceShiftedBeforeCircle);
  const auto result = applyPatches(source, {{*patch}});
  EXPECT_EQ(result.applied, 1u);

  EXPECT_THAT(source, testing::HasSubstr("<circle id=\"c1\" cx=\"40\" cy=\"40\" r=\"10\" "
                                         "transform=\"translate(5)\""));
  EXPECT_THAT(source, testing::HasSubstr("<g id=\"group\" data-note=\"shifted\">"));
  EXPECT_EQ(CountSubstring(source, "transform="), 1u);
}

TEST(AttributeWritebackRegressionTest, RepeatedWritebackReplacesExistingTransform) {
  auto document = ParseDocument(kGroupedCircleSvg);
  ASSERT_TRUE(document.has_value());
  svg::SVGElement circle = GetElementById(*document, "#c1");

  std::string source(kGroupedCircleSourceShiftedBeforeCircle);

  auto firstPatch = buildAttributeWriteback(source, circle, "transform", "translate(5)");
  ASSERT_TRUE(firstPatch.has_value());
  auto firstResult = applyPatches(source, {{*firstPatch}});
  ASSERT_EQ(firstResult.applied, 1u);

  auto secondPatch = buildAttributeWriteback(source, circle, "transform", "translate(10)");
  ASSERT_TRUE(secondPatch.has_value());
  auto secondResult = applyPatches(source, {{*secondPatch}});
  ASSERT_EQ(secondResult.applied, 1u);

  EXPECT_THAT(source, testing::HasSubstr("<circle id=\"c1\" cx=\"40\" cy=\"40\" r=\"10\" "
                                         "transform=\"translate(10)\""));
  EXPECT_EQ(CountSubstring(source, "transform="), 1u);
}

TEST(AttributeWritebackRegressionTest, SerializedTransformStaysLocalToDraggedCircle) {
  auto document = ParseDocument(kParentTransformSvg);
  ASSERT_TRUE(document.has_value());
  auto circle = GetElementById(*document, "#c1").cast<svg::SVGGraphicsElement>();
  circle.setTransform(Transform2d::Translate(Vector2d(5.0, 0.0)));

  const RcString serialized = toSVGTransformString(circle.transform());
  EXPECT_EQ(std::string_view(serialized), "translate(5)");

  auto patch = buildAttributeWriteback(kParentTransformSvg, circle, "transform",
                                       std::string_view(serialized));
  ASSERT_TRUE(patch.has_value());

  std::string source(kParentTransformSvg);
  const auto result = applyPatches(source, {{*patch}});
  EXPECT_EQ(result.applied, 1u);

  EXPECT_THAT(source, testing::HasSubstr("<g id=\"group\" transform=\"translate(10, 0)\">"));
  EXPECT_THAT(source, testing::HasSubstr("<circle id=\"c1\" cx=\"40\" cy=\"40\" r=\"10\" "
                                         "transform=\"translate(5)\""));
  EXPECT_THAT(source, testing::Not(testing::HasSubstr("translate(15)")));
}

TEST(AttributeWritebackRegressionTest, RemoveTransformAttributeWithoutLeavingWhitespace) {
  constexpr std::string_view kSource =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg"><circle transform="translate(5)"/></svg>)svg";
  auto document = ParseDocument(kSource);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement circle = GetElementById(*document, "circle");
  const auto target = captureAttributeWritebackTarget(circle);
  ASSERT_TRUE(target.has_value());

  auto patch = buildAttributeRemoveWriteback(kSource, *target, "transform");
  ASSERT_TRUE(patch.has_value());

  std::string source(kSource);
  const auto result = applyPatches(source, {{*patch}});
  ASSERT_EQ(result.applied, 1u);
  EXPECT_EQ(source, "<svg xmlns=\"http://www.w3.org/2000/svg\"><circle/></svg>");
}

TEST(AttributeWritebackRegressionTest, RemoveMiddleAttributeKeepsSingleSpacing) {
  constexpr std::string_view kSource =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg"><circle cx="10" transform="translate(5)" r="3"/></svg>)svg";
  auto document = ParseDocument(kSource);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement circle = GetElementById(*document, "circle");
  const auto target = captureAttributeWritebackTarget(circle);
  ASSERT_TRUE(target.has_value());

  auto patch = buildAttributeRemoveWriteback(kSource, *target, "transform");
  ASSERT_TRUE(patch.has_value());

  std::string source(kSource);
  const auto result = applyPatches(source, {{*patch}});
  ASSERT_EQ(result.applied, 1u);
  EXPECT_EQ(source, "<svg xmlns=\"http://www.w3.org/2000/svg\"><circle cx=\"10\" r=\"3\"/></svg>");
}

TEST(AttributeWritebackRegressionTest, RemoveMissingAttributeReturnsNullopt) {
  constexpr std::string_view kSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><circle cx="10"/></svg>)";
  auto document = ParseDocument(kSource);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement circle = GetElementById(*document, "circle");
  const auto target = captureAttributeWritebackTarget(circle);
  ASSERT_TRUE(target.has_value());

  EXPECT_FALSE(buildAttributeRemoveWriteback(kSource, *target, "transform").has_value());
}

TEST(AttributeWritebackRegressionTest, RemoveSelfClosingElementConsumesWholeLine) {
  constexpr std::string_view kSource =
      "<svg xmlns=\"http://www.w3.org/2000/svg\">\n  <circle id=\"a\"/>\n  <circle id=\"b\"/>\n"
      "</svg>\n";
  auto document = ParseDocument(kSource);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement circle = GetElementById(*document, "#a");
  const auto target = captureAttributeWritebackTarget(circle);
  ASSERT_TRUE(target.has_value());

  auto patch = buildElementRemoveWriteback(kSource, *target);
  ASSERT_TRUE(patch.has_value());

  std::string source(kSource);
  const auto result = applyPatches(source, {{*patch}});
  ASSERT_EQ(result.applied, 1u);
  EXPECT_EQ(source, "<svg xmlns=\"http://www.w3.org/2000/svg\">\n  <circle id=\"b\"/>\n</svg>\n");
}

TEST(AttributeWritebackRegressionTest, RemoveSelfClosingElementConsumesCrLfLineEnding) {
  constexpr std::string_view kSource =
      "<svg xmlns=\"http://www.w3.org/2000/svg\">\r\n  <circle id=\"a\"/>\r\n  <circle "
      "id=\"b\"/>\r\n</svg>\r\n";
  auto document = ParseDocument(kSource);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement circle = GetElementById(*document, "#a");
  const auto target = captureAttributeWritebackTarget(circle);
  ASSERT_TRUE(target.has_value());

  auto patch = buildElementRemoveWriteback(kSource, *target);
  ASSERT_TRUE(patch.has_value());

  std::string source(kSource);
  const auto result = applyPatches(source, {{*patch}});
  ASSERT_EQ(result.applied, 1u);
  EXPECT_EQ(source,
            "<svg xmlns=\"http://www.w3.org/2000/svg\">\r\n  <circle id=\"b\"/>\r\n</svg>\r\n");
}

TEST(AttributeWritebackRegressionTest, RemoveSelfClosingElementConsumesBareCrLineEnding) {
  constexpr std::string_view kSource =
      "<svg xmlns=\"http://www.w3.org/2000/svg\">\r  <circle id=\"a\"/>\r  <circle id=\"b\"/>\r"
      "</svg>\r";
  auto document = ParseDocument(kSource);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement circle = GetElementById(*document, "#a");
  const auto target = captureAttributeWritebackTarget(circle);
  ASSERT_TRUE(target.has_value());

  auto patch = buildElementRemoveWriteback(kSource, *target);
  ASSERT_TRUE(patch.has_value());

  std::string source(kSource);
  const auto result = applyPatches(source, {{*patch}});
  ASSERT_EQ(result.applied, 1u);
  EXPECT_EQ(source, "<svg xmlns=\"http://www.w3.org/2000/svg\">\r  <circle id=\"b\"/>\r</svg>\r");
}

TEST(AttributeWritebackRegressionTest, RemoveInlineElementConsumesFollowingSpaceOnly) {
  constexpr std::string_view kSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><circle id="a"/> <circle id="b"/></svg>)";
  auto document = ParseDocument(kSource);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement circle = GetElementById(*document, "#a");
  const auto target = captureAttributeWritebackTarget(circle);
  ASSERT_TRUE(target.has_value());

  auto patch = buildElementRemoveWriteback(kSource, *target);
  ASSERT_TRUE(patch.has_value());

  std::string source(kSource);
  const auto result = applyPatches(source, {{*patch}});
  ASSERT_EQ(result.applied, 1u);
  EXPECT_EQ(source, R"(<svg xmlns="http://www.w3.org/2000/svg"><circle id="b"/></svg>)");
}

TEST(AttributeWritebackRegressionTest, RemoveContainerElementDeletesSubtree) {
  constexpr std::string_view kSource =
      "<svg xmlns=\"http://www.w3.org/2000/svg\">\n  <g id=\"group\">\n    <circle "
      "id=\"child\"/>\n  </g>\n  <rect id=\"after\"/>\n</svg>\n";
  auto document = ParseDocument(kSource);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement group = GetElementById(*document, "#group");
  const auto target = captureAttributeWritebackTarget(group);
  ASSERT_TRUE(target.has_value());

  auto patch = buildElementRemoveWriteback(kSource, *target);
  ASSERT_TRUE(patch.has_value());

  std::string source(kSource);
  const auto result = applyPatches(source, {{*patch}});
  ASSERT_EQ(result.applied, 1u);
  EXPECT_EQ(source, "<svg xmlns=\"http://www.w3.org/2000/svg\">\n  <rect id=\"after\"/>\n</svg>\n");
}

// --- resolveAttributeWritebackTarget ---------------------------------------

TEST_F(AttributeWritebackTest, ResolveTargetRoundTripsCapturedElement) {
  const auto target = captureAttributeWritebackTarget(*rect_);
  ASSERT_TRUE(target.has_value());
  EXPECT_THAT(target->elementId, testing::Optional(RcString("r1")));

  const auto resolved = resolveAttributeWritebackTarget(document_, *target);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->id(), "r1");
  EXPECT_TRUE(*resolved == *rect_);
}

TEST(AttributeWritebackResolveTest, ResolvesNestedElementByIdSearch) {
  auto document = ParseDocument(kGroupedCircleSvg);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement circle = GetElementById(*document, "#c1");
  const auto target = captureAttributeWritebackTarget(circle);
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(target->elementId.has_value());

  const auto resolved = resolveAttributeWritebackTarget(*document, *target);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->id(), "c1");
}

TEST(AttributeWritebackResolveTest, ResolvesNestedElementByPathWhenIdAbsent) {
  // No id on the circle forces the path-walk branch instead of the id search.
  constexpr std::string_view kSource =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg"><g><circle cx="1" cy="2" r="3"/></g></svg>)svg";
  auto document = ParseDocument(kSource);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement circle = GetElementById(*document, "circle");
  const auto target = captureAttributeWritebackTarget(circle);
  ASSERT_TRUE(target.has_value());
  EXPECT_FALSE(target->elementId.has_value());

  const auto resolved = resolveAttributeWritebackTarget(*document, *target);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->tagName().name, "circle");
}

TEST(AttributeWritebackResolveTest, BatchResolutionPreservesTargetOrderAndMisses) {
  auto document = ParseDocument(kGroupedCircleSvg);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement circle = GetElementById(*document, "#c1");
  const auto circleTarget = captureAttributeWritebackTarget(circle);
  ASSERT_TRUE(circleTarget.has_value());

  AttributeWritebackTarget missing = *circleTarget;
  missing.elementId.reset();
  missing.elementPath.back().elementChildIndex = 99;
  const std::array targets{*circleTarget, missing};

  const std::vector<std::optional<svg::SVGElement>> resolved =
      resolveAttributeWritebackTargets(*document, targets);

  ASSERT_EQ(resolved.size(), 2u);
  ASSERT_TRUE(resolved[0].has_value());
  EXPECT_EQ(resolved[0]->id(), "c1");
  EXPECT_FALSE(resolved[1].has_value());
}

TEST(AttributeWritebackResolveTest, EmptyPathResolvesToNullopt) {
  auto document = ParseDocument(kSimpleSvg);
  ASSERT_TRUE(document.has_value());

  AttributeWritebackTarget empty;
  EXPECT_FALSE(resolveAttributeWritebackTarget(*document, empty).has_value());
}

TEST(AttributeWritebackResolveTest, RootSegmentMismatchResolvesToNullopt) {
  auto document = ParseDocument(kSimpleSvg);
  ASSERT_TRUE(document.has_value());

  // Path with a wrong root tag name (path-walk branch, no id) must not resolve.
  AttributeWritebackTarget target;
  target.elementPath.push_back(
      AttributeWritebackPathSegment{0, xml::XMLQualifiedName(RcString(), RcString("notSvg"))});
  EXPECT_FALSE(resolveAttributeWritebackTarget(*document, target).has_value());
}

TEST(AttributeWritebackResolveTest, ChildIndexOutOfRangeResolvesToNullopt) {
  auto document = ParseDocument(kGroupedCircleSvg);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement circle = GetElementById(*document, "#c1");
  auto target = captureAttributeWritebackTarget(circle);
  ASSERT_TRUE(target.has_value());

  // Drop the id so resolution must use the path, then break the leaf child
  // index so the path walk fails to find a matching child.
  target->elementId.reset();
  ASSERT_GE(target->elementPath.size(), 2u);
  target->elementPath.back().elementChildIndex = 99;
  EXPECT_FALSE(resolveAttributeWritebackTarget(*document, *target).has_value());
}

TEST(AttributeWritebackResolveTest, LeafTagMismatchResolvesToNullopt) {
  auto document = ParseDocument(kGroupedCircleSvg);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement circle = GetElementById(*document, "#c1");
  auto target = captureAttributeWritebackTarget(circle);
  ASSERT_TRUE(target.has_value());

  target->elementId.reset();
  ASSERT_GE(target->elementPath.size(), 2u);
  target->elementPath.back().qualifiedName = xml::XMLQualifiedName(RcString("rect"));

  EXPECT_FALSE(resolveAttributeWritebackTarget(*document, *target).has_value());
}

// --- buildAttributeWriteback element overload, stale-source fallback --------

TEST(AttributeWritebackResolveTest, ElementOverloadFallsBackToPathWhenSourceStale) {
  // The element's tracked start offset points at the *original* source, but the
  // patch source has had content inserted before it, so HasExpectedOpeningTagAt
  // fails and we fall back to re-resolving via the captured target.
  auto document = ParseDocument(kGroupedCircleSvg);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement circle = GetElementById(*document, "#c1");

  auto patch =
      buildAttributeWriteback(kGroupedCircleSourceShiftedBeforeCircle, circle, "fill", "purple");
  ASSERT_TRUE(patch.has_value());

  std::string source(kGroupedCircleSourceShiftedBeforeCircle);
  const auto result = applyPatches(source, {{*patch}});
  EXPECT_EQ(result.applied, 1u);
  EXPECT_THAT(source, testing::HasSubstr("fill=\"purple\""));
  EXPECT_THAT(source, testing::HasSubstr("data-note=\"shifted\""));
}

TEST(AttributeWritebackResolveTest, ElementOverloadReturnsNulloptWhenElementMissingFromSource) {
  // The element exists in the document, but the patch source doesn't contain it
  // at all - neither the tracked offset nor the path/id can resolve it.
  auto document = ParseDocument(kSimpleSvg);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement rect = GetElementById(*document, "#r1");

  constexpr std::string_view kSourceWithoutRect =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200"></svg>)svg";
  EXPECT_FALSE(buildAttributeWriteback(kSourceWithoutRect, rect, "fill", "blue").has_value());
}

TEST(AttributeWritebackResolveTest, ElementOverloadRejectsMissingAndNonTagOffsets) {
  auto document = ParseDocument(kSimpleSvg);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement rect = GetElementById(*document, "#r1");

  EXPECT_FALSE(buildAttributeWriteback("", rect, "fill", "blue").has_value());

  std::string spaces(kSimpleSvg);
  const std::size_t rectStart = spaces.find("<rect");
  ASSERT_NE(rectStart, std::string::npos);
  spaces[rectStart] = ' ';
  EXPECT_FALSE(buildAttributeWriteback(spaces, rect, "fill", "blue").has_value());
}

TEST(AttributeWritebackResolveTest, ElementOverloadAcceptsOpeningTagWhitespaceTerminators) {
  struct Case {
    std::string_view source;
    std::string_view selector;
    std::string_view expectedFragment;
  };

  constexpr Case kCases[] = {
      {
          R"(<svg xmlns="http://www.w3.org/2000/svg"><g></g></svg>)",
          "g",
          R"(<g data-state="ready">)",
      },
      {
          "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect\tid=\"r1\"/></svg>",
          "#r1",
          "<rect\tid=\"r1\" data-state=\"ready\"/>",
      },
      {
          "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect\nid=\"r1\"/></svg>",
          "#r1",
          "<rect\nid=\"r1\" data-state=\"ready\"/>",
      },
      {
          "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect\rid=\"r1\"/></svg>",
          "#r1",
          "<rect\rid=\"r1\" data-state=\"ready\"/>",
      },
  };

  for (const Case& testCase : kCases) {
    SCOPED_TRACE(testCase.source);
    auto document = ParseDocument(testCase.source);
    ASSERT_TRUE(document.has_value());
    const svg::SVGElement element = GetElementById(*document, testCase.selector);

    auto patch = buildAttributeWriteback(testCase.source, element, "data-state", "ready");
    ASSERT_TRUE(patch.has_value());

    std::string source(testCase.source);
    const auto result = applyPatches(source, {{*patch}});
    ASSERT_EQ(result.applied, 1u);
    EXPECT_THAT(source, testing::HasSubstr(testCase.expectedFragment));
  }
}

TEST(AttributeWritebackResolveTest, ElementOverloadReturnsNulloptForUnterminatedQuotedTag) {
  auto document = ParseDocument(kSimpleSvg);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement rect = GetElementById(*document, "#r1");

  const std::size_t rectStart = kSimpleSvg.find("<rect");
  ASSERT_NE(rectStart, std::string_view::npos);

  std::string staleSource(kSimpleSvg.substr(0, rectStart));
  staleSource += R"(<rect id="r1" data-label="unterminated)";

  EXPECT_FALSE(buildAttributeWriteback(staleSource, rect, "fill", "blue").has_value());
}

// --- target overload + malformed source ------------------------------------

TEST(AttributeWritebackTargetOverloadTest, MalformedSourceReturnsNullopt) {
  constexpr std::string_view kMalformed = R"(<svg><circle id="c1" )";
  AttributeWritebackTarget target;
  target.elementId = RcString("c1");
  target.elementPath.push_back(
      AttributeWritebackPathSegment{0, xml::XMLQualifiedName(RcString(), RcString("circle"))});

  EXPECT_FALSE(buildAttributeWriteback(kMalformed, target, "fill", "blue").has_value());
  EXPECT_FALSE(buildAttributeRemoveWriteback(kMalformed, target, "fill").has_value());
  EXPECT_FALSE(buildElementRemoveWriteback(kMalformed, target).has_value());
}

TEST(AttributeWritebackTargetOverloadTest, TargetNotInSourceReturnsNullopt) {
  constexpr std::string_view kSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r1"/></svg>)";
  AttributeWritebackTarget target;
  target.elementId = RcString("missing");
  target.elementPath.push_back(
      AttributeWritebackPathSegment{0, xml::XMLQualifiedName(RcString(), RcString("rect"))});

  EXPECT_FALSE(buildAttributeWriteback(kSource, target, "fill", "blue").has_value());
  EXPECT_FALSE(buildAttributeRemoveWriteback(kSource, target, "fill").has_value());
  EXPECT_FALSE(buildElementRemoveWriteback(kSource, target).has_value());
}

TEST(AttributeWritebackTargetOverloadTest, EmptyTargetReturnsNullopt) {
  AttributeWritebackTarget target;

  EXPECT_FALSE(buildAttributeWriteback(kSimpleSvg, target, "fill", "blue").has_value());
  EXPECT_FALSE(buildAttributeRemoveWriteback(kSimpleSvg, target, "fill").has_value());
  EXPECT_FALSE(buildElementRemoveWriteback(kSimpleSvg, target).has_value());
}

TEST(AttributeWritebackTargetOverloadTest, IdSearchWinsWhenSiblingOrderChanges) {
  constexpr std::string_view kOriginalSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="a"/><rect id="b"/></svg>)";
  auto originalDocument = ParseDocument(kOriginalSource);
  ASSERT_TRUE(originalDocument.has_value());
  const svg::SVGElement rectB = GetElementById(*originalDocument, "#b");
  const auto target = captureAttributeWritebackTarget(rectB);
  ASSERT_TRUE(target.has_value());

  constexpr std::string_view kReorderedSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="b"/><rect id="a"/></svg>)";
  auto patch = buildAttributeWriteback(kReorderedSource, *target, "fill", "blue");
  ASSERT_TRUE(patch.has_value());

  std::string source(kReorderedSource);
  const auto result = applyPatches(source, {{*patch}});
  ASSERT_EQ(result.applied, 1u);
  EXPECT_EQ(
      source,
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="b" fill="blue"/><rect id="a"/></svg>)");
}

TEST(AttributeWritebackTargetOverloadTest, PathResolutionSkipsNonElementSiblings) {
  constexpr std::string_view kSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><!-- note --><rect width="1"/><rect width="2"/></svg>)";
  auto document = ParseDocument(kSource);
  ASSERT_TRUE(document.has_value());
  auto first = document->svgElement().firstChild();
  ASSERT_TRUE(first.has_value());
  auto second = first->nextSibling();
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(second->id().empty());
  const auto target = captureAttributeWritebackTarget(*second);
  ASSERT_TRUE(target.has_value());
  EXPECT_FALSE(target->elementId.has_value());

  auto patch = buildAttributeWriteback(kSource, *target, "fill", "blue");
  ASSERT_TRUE(patch.has_value());

  std::string source(kSource);
  const auto result = applyPatches(source, {{*patch}});
  ASSERT_EQ(result.applied, 1u);
  EXPECT_EQ(
      source,
      R"(<svg xmlns="http://www.w3.org/2000/svg"><!-- note --><rect width="1"/><rect width="2" fill="blue"/></svg>)");
}

TEST(AttributeWritebackTargetOverloadTest, ResolveTargetFallsBackToPathWhenIdMisses) {
  constexpr std::string_view kSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><g><rect id="r1"/></g></svg>)";
  auto document = ParseDocument(kSource);
  ASSERT_TRUE(document.has_value());
  svg::SVGElement rect = GetElementById(*document, "#r1");
  auto target = captureAttributeWritebackTarget(rect);
  ASSERT_TRUE(target.has_value());
  target->elementId = RcString("missing");

  std::optional<svg::SVGElement> resolved = resolveAttributeWritebackTarget(*document, *target);

  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->tagName(), xml::XMLQualifiedNameRef("rect"));
}

TEST(AttributeWritebackTargetOverloadTest, ResolveTargetRejectsBadRootSegment) {
  auto document = ParseDocument(kSimpleSvg);
  ASSERT_TRUE(document.has_value());
  AttributeWritebackTarget target;
  target.elementPath.push_back(
      AttributeWritebackPathSegment{1, xml::XMLQualifiedName(RcString(), RcString("svg"))});

  EXPECT_FALSE(resolveAttributeWritebackTarget(*document, target).has_value());
}

TEST(AttributeWritebackTargetOverloadTest, InsertsAttributeViaTargetOverload) {
  auto document = ParseDocument(kSimpleSvg);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement rect = GetElementById(*document, "#r1");
  const auto target = captureAttributeWritebackTarget(rect);
  ASSERT_TRUE(target.has_value());

  auto patch = buildAttributeWriteback(kSimpleSvg, *target, "stroke", "green");
  ASSERT_TRUE(patch.has_value());

  std::string source(kSimpleSvg);
  const auto result = applyPatches(source, {{*patch}});
  EXPECT_EQ(result.applied, 1u);
  EXPECT_THAT(source, testing::HasSubstr("stroke=\"green\""));
}

TEST(AttributeWritebackTargetOverloadTest, RemovesAttributeWithLeadingSpaceViaTargetOverload) {
  // Single-attribute element: the removal must consume the leading space rather
  // than the (absent) trailing space, exercising the `start - 1` branch.
  constexpr std::string_view kSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r1" fill="red"/></svg>)";
  auto document = ParseDocument(kSource);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement rect = GetElementById(*document, "#r1");
  const auto target = captureAttributeWritebackTarget(rect);
  ASSERT_TRUE(target.has_value());

  auto patch = buildAttributeRemoveWriteback(kSource, *target, "fill");
  ASSERT_TRUE(patch.has_value());

  std::string source(kSource);
  const auto result = applyPatches(source, {{*patch}});
  ASSERT_EQ(result.applied, 1u);
  EXPECT_EQ(source, R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r1"/></svg>)");
}

// --- buildAttributeWriteback inserts before tag close ----------------------

TEST(AttributeWritebackTargetOverloadTest, InsertsBeforeSelfClosingSlash) {
  // Element written with a space before `/>` - insertion point is just before
  // the `/`, exercising the `/>` detection branch in the insert scanner.
  constexpr std::string_view kSource = R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r1" />)"
                                       R"(</svg>)";
  auto document = ParseDocument(kSource);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement rect = GetElementById(*document, "#r1");
  const auto target = captureAttributeWritebackTarget(rect);
  ASSERT_TRUE(target.has_value());

  auto patch = buildAttributeWriteback(kSource, *target, "fill", "blue");
  ASSERT_TRUE(patch.has_value());

  std::string source(kSource);
  const auto result = applyPatches(source, {{*patch}});
  ASSERT_EQ(result.applied, 1u);
  EXPECT_THAT(source, testing::HasSubstr("fill=\"blue\""));

  ParseWarningSink sink;
  auto reparse = svg::parser::SVGParser::ParseSVG(source, sink);
  ASSERT_TRUE(reparse.hasResult()) << source;
  auto reRect = reparse.result().querySelector("#r1");
  ASSERT_TRUE(reRect.has_value());
  EXPECT_THAT(reRect->getAttribute("fill"), testing::Optional(RcString("blue")));
}

TEST(AttributeWritebackTargetOverloadTest, InsertScannerSkipsQuotedTagCloseCharacters) {
  constexpr std::string_view kSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r1" data-note="a > b / c"/></svg>)";
  auto document = ParseDocument(kSource);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement rect = GetElementById(*document, "#r1");
  const auto target = captureAttributeWritebackTarget(rect);
  ASSERT_TRUE(target.has_value());

  auto patch = buildAttributeWriteback(kSource, *target, "fill", "blue");
  ASSERT_TRUE(patch.has_value());

  std::string source(kSource);
  const auto result = applyPatches(source, {{*patch}});
  ASSERT_EQ(result.applied, 1u);
  EXPECT_THAT(source, testing::HasSubstr(R"(data-note="a > b / c" fill="blue")"));
}

TEST(AttributeWritebackTargetOverloadTest, InsertScannerSkipsSingleQuotedTagCloseCharacters) {
  constexpr std::string_view kSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r1" data-note='a > b / c'/></svg>)";
  auto document = ParseDocument(kSource);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement rect = GetElementById(*document, "#r1");
  const auto target = captureAttributeWritebackTarget(rect);
  ASSERT_TRUE(target.has_value());

  auto patch = buildAttributeWriteback(kSource, *target, "fill", "blue");
  ASSERT_TRUE(patch.has_value());

  std::string source(kSource);
  const auto result = applyPatches(source, {{*patch}});
  ASSERT_EQ(result.applied, 1u);
  EXPECT_THAT(source, testing::HasSubstr(R"(data-note='a > b / c' fill="blue")"));
}

TEST(AttributeWritebackTargetOverloadTest, RemoveInlineElementAfterNonIndentTextKeepsPrefix) {
  constexpr std::string_view kSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg">prefix<circle id="a"/></svg>)";
  auto document = ParseDocument(kSource);
  ASSERT_TRUE(document.has_value());
  const svg::SVGElement circle = GetElementById(*document, "#a");
  const auto target = captureAttributeWritebackTarget(circle);
  ASSERT_TRUE(target.has_value());

  auto patch = buildElementRemoveWriteback(kSource, *target);
  ASSERT_TRUE(patch.has_value());

  std::string source(kSource);
  const auto result = applyPatches(source, {{*patch}});
  ASSERT_EQ(result.applied, 1u);
  EXPECT_EQ(source, R"(<svg xmlns="http://www.w3.org/2000/svg">prefix</svg>)");
}

TEST(AttributeWritebackTargetOverloadTest, RemoveAttributeConsumesXmlWhitespaceKinds) {
  struct Case {
    std::string_view source;
    std::string_view expected;
  };

  constexpr Case kCases[] = {
      {
          "<svg xmlns=\"http://www.w3.org/2000/svg\"><circle cx=\"1\"\ttransform=\"translate(5)\" "
          "r=\"2\"/></svg>",
          "<svg xmlns=\"http://www.w3.org/2000/svg\"><circle cx=\"1\" r=\"2\"/></svg>",
      },
      {
          "<svg xmlns=\"http://www.w3.org/2000/svg\"><circle cx=\"1\"\ntransform=\"translate(5)\" "
          "r=\"2\"/></svg>",
          "<svg xmlns=\"http://www.w3.org/2000/svg\"><circle cx=\"1\" r=\"2\"/></svg>",
      },
      {
          "<svg xmlns=\"http://www.w3.org/2000/svg\"><circle cx=\"1\"\rtransform=\"translate(5)\" "
          "r=\"2\"/></svg>",
          "<svg xmlns=\"http://www.w3.org/2000/svg\"><circle cx=\"1\" r=\"2\"/></svg>",
      },
  };

  for (const Case& testCase : kCases) {
    SCOPED_TRACE(testCase.source);
    auto document = ParseDocument(testCase.source);
    ASSERT_TRUE(document.has_value());
    const svg::SVGElement circle = GetElementById(*document, "circle");
    const auto target = captureAttributeWritebackTarget(circle);
    ASSERT_TRUE(target.has_value());

    auto patch = buildAttributeRemoveWriteback(testCase.source, *target, "transform");
    ASSERT_TRUE(patch.has_value());

    std::string source(testCase.source);
    const auto result = applyPatches(source, {{*patch}});
    ASSERT_EQ(result.applied, 1u);
    EXPECT_EQ(source, testCase.expected);
  }
}

TEST(AttributeWritebackTargetOverloadTest, RemoveRootElementAtEndOfSource) {
  constexpr std::string_view kSource = R"(<svg xmlns="http://www.w3.org/2000/svg"/>)";
  auto document = ParseDocument(kSource);
  ASSERT_TRUE(document.has_value());
  const auto target = captureAttributeWritebackTarget(document->svgElement());
  ASSERT_TRUE(target.has_value());

  auto patch = buildElementRemoveWriteback(kSource, *target);
  ASSERT_TRUE(patch.has_value());

  std::string source(kSource);
  const auto result = applyPatches(source, {{*patch}});
  ASSERT_EQ(result.applied, 1u);
  EXPECT_TRUE(source.empty());
}

}  // namespace
}  // namespace donner::editor
