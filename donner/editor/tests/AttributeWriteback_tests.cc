#include "donner/editor/AttributeWriteback.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

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
  // A value containing NUL can't be XML-escaped — buildAttributeWriteback
  // must return nullopt rather than producing invalid XML.
  auto patch = buildAttributeWriteback(kSimpleSvg, *rect_, "fill",
                                       std::string_view("a\0b", 3));
  EXPECT_FALSE(patch.has_value());
}

TEST(AttributeWritebackRegressionTest, TargetCorrectnessUsesCircleNotAncestorGroup) {
  auto document = ParseDocument(kGroupedCircleSvg);
  ASSERT_TRUE(document.has_value());
  svg::SVGElement circle = GetElementById(*document, "#c1");

  auto patch = buildAttributeWriteback(kGroupedCircleSourceShiftedBeforeCircle, circle,
                                       "transform", "translate(5)");
  ASSERT_TRUE(patch.has_value());

  std::string source(kGroupedCircleSourceShiftedBeforeCircle);
  const auto result = applyPatches(source, {{*patch}});
  EXPECT_EQ(result.applied, 1u);

  EXPECT_THAT(source, testing::HasSubstr(
                          "<circle id=\"c1\" cx=\"40\" cy=\"40\" r=\"10\" "
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

  EXPECT_THAT(source, testing::HasSubstr(
                          "<circle id=\"c1\" cx=\"40\" cy=\"40\" r=\"10\" "
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

  EXPECT_THAT(source,
              testing::HasSubstr("<g id=\"group\" transform=\"translate(10, 0)\">"));
  EXPECT_THAT(source, testing::HasSubstr(
                          "<circle id=\"c1\" cx=\"40\" cy=\"40\" r=\"10\" "
                          "transform=\"translate(5)\""));
  EXPECT_THAT(source, testing::Not(testing::HasSubstr("translate(15)")));
}

}  // namespace
}  // namespace donner::editor
