#include "donner/editor/AttributeWriteback.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/editor/TextPatch.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::editor {
namespace {

using testing::Eq;

constexpr std::string_view kSimpleSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
  <rect id="r1" x="10" y="20" width="50" height="30" fill="red"/>
</svg>)";

class AttributeWritebackTest : public ::testing::Test {
protected:
  void SetUp() override {
    ParseWarningSink sink;
    auto result = svg::parser::SVGParser::ParseSVG(kSimpleSvg, sink);
    ASSERT_TRUE(result.hasResult());
    document_ = std::move(result.result());

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

}  // namespace
}  // namespace donner::editor
