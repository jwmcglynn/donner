#include "donner/editor/SoftWrap.h"

#include <gtest/gtest.h>

#include <vector>

namespace donner::editor {
namespace {

TEST(SoftWrapTest, AlignsXmlAttributeContinuationAfterElementName) {
  constexpr std::string_view line = R"(  <rect id="target" x="10" y="20" width="30" height="40"/>)";

  EXPECT_EQ(ComputeXmlContinuationIndent(line), 8);

  const std::vector<SoftWrapSegment> segments = ComputeSoftWrapSegments(line, 30);
  ASSERT_GE(segments.size(), 2u);
  EXPECT_FALSE(segments[0].continuation);
  EXPECT_EQ(segments[1].indentColumns, 8);
  EXPECT_TRUE(segments[1].continuation);
}

TEST(SoftWrapTest, UsesPreviousLineIndentForNonElementText) {
  constexpr std::string_view line = "    text that should wrap near a word boundary";

  EXPECT_EQ(ComputeXmlContinuationIndent(line), 4);

  const std::vector<SoftWrapSegment> segments = ComputeSoftWrapSegments(line, 22);
  ASSERT_GE(segments.size(), 2u);
  EXPECT_EQ(segments[1].indentColumns, 4);
}

TEST(SoftWrapTest, EmptyLineStillProducesOneVisualRow) {
  EXPECT_EQ(ComputeSoftWrapSegments("", 20), (std::vector<SoftWrapSegment>{SoftWrapSegment{}}));
}

}  // namespace
}  // namespace donner::editor
