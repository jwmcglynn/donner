#include "donner/editor/SoftWrap.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <vector>

namespace donner::editor {
namespace {

MATCHER_P2(SoftWrapSegmentIs, expectedIndentColumns, expectedContinuation, "") {
  return testing::ExplainMatchResult(
      testing::AllOf(testing::Field("indentColumns", &SoftWrapSegment::indentColumns,
                                    testing::Eq(expectedIndentColumns)),
                     testing::Field("continuation", &SoftWrapSegment::continuation,
                                    testing::Eq(expectedContinuation))),
      arg, result_listener);
}

TEST(SoftWrapTest, AlignsXmlAttributeContinuationAfterElementName) {
  constexpr std::string_view line = R"(  <rect id="target" x="10" y="20" width="30" height="40"/>)";

  EXPECT_EQ(ComputeXmlContinuationIndent(line), 8);

  const std::vector<SoftWrapSegment> segments = ComputeSoftWrapSegments(line, 30);
  ASSERT_GE(segments.size(), 2u);
  EXPECT_THAT(segments[0], SoftWrapSegmentIs(0, false));
  EXPECT_THAT(segments[1], SoftWrapSegmentIs(8, true));
}

TEST(SoftWrapTest, UsesPreviousLineIndentForNonElementText) {
  constexpr std::string_view line = "    text that should wrap near a word boundary";

  EXPECT_EQ(ComputeXmlContinuationIndent(line), 4);

  const std::vector<SoftWrapSegment> segments = ComputeSoftWrapSegments(line, 22);
  ASSERT_GE(segments.size(), 2u);
  EXPECT_THAT(segments[1], SoftWrapSegmentIs(4, true));
}

TEST(SoftWrapTest, EmptyLineStillProducesOneVisualRow) {
  EXPECT_EQ(ComputeSoftWrapSegments("", 20), (std::vector<SoftWrapSegment>{SoftWrapSegment{}}));
}

}  // namespace
}  // namespace donner::editor
