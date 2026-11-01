#include "donner/svg/text/BidiParagraph.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string_view>
#include <vector>

namespace donner::svg {
namespace {
using testing::ElementsAre;
using testing::Field;
using testing::IsTrue;
using testing::Optional;

std::vector<uint32_t> VisualOrder(const std::vector<BidiParagraph::Run>& runs) {
  std::vector<uint32_t> result;
  for (const auto& run : runs) {
    for (uint32_t index = run.start; index < run.end; ++index) {
      result.push_back(run.level % 2 ? run.end - 1 - (index - run.start) : index);
    }
  }
  return result;
}

TEST(BidiParagraphTest, DefaultSvgBaseIsExplicitLeftToRight) {
  BidiResourceBudget budget;
  auto paragraph = BidiParagraph::Analyze({U'م', U'ر', ' ', 'a', 'b', '.'}, budget);
  ASSERT_THAT(paragraph.has_value(), IsTrue());
  const auto runs = paragraph->visualRuns(0, 6, budget);
  ASSERT_THAT(runs.has_value(), IsTrue());
  EXPECT_THAT(VisualOrder(*runs), ElementsAre(1u, 0u, 2u, 3u, 4u, 5u));
  EXPECT_THAT(paragraph->levelAt(5), testing::Eq(0));
}

TEST(BidiParagraphTest, ParagraphSeparatorsAndCrLfDoNotDropFollowingText) {
  for (const std::u32string_view source : {U"אב\nגד", U"אב\r\nגד"}) {
    SCOPED_TRACE(source.size());
    BidiResourceBudget budget;
    auto paragraph = BidiParagraph::Analyze({source.begin(), source.end()}, budget);
    ASSERT_THAT(paragraph.has_value(), IsTrue());
    const auto runs = paragraph->visualRuns(0, source.size(), budget);
    ASSERT_THAT(runs.has_value(), IsTrue());
    if (source.size() == 5) {
      EXPECT_THAT(VisualOrder(*runs), ElementsAre(1u, 0u, 2u, 4u, 3u));
    } else {
      EXPECT_THAT(VisualOrder(*runs), ElementsAre(1u, 0u, 2u, 3u, 5u, 4u));
    }
  }
}

TEST(BidiParagraphTest, ActualLineEndResetsTrailingWhitespaceBeforeReordering) {
  BidiResourceBudget budget;
  auto paragraph = BidiParagraph::Analyze({U'א', U'ב', ' ', ' ', U'ג', U'ד'}, budget);
  ASSERT_THAT(paragraph.has_value(), IsTrue());
  const auto line = paragraph->visualRuns(0, 3, budget);
  ASSERT_THAT(line.has_value(), IsTrue());
  EXPECT_THAT(VisualOrder(*line), ElementsAre(1u, 0u, 2u));
  const auto whole = paragraph->visualRuns(0, 6, budget);
  ASSERT_THAT(whole.has_value(), IsTrue());
  EXPECT_THAT(VisualOrder(*whole), ElementsAre(5u, 4u, 3u, 2u, 1u, 0u));
}

TEST(BidiParagraphTest, FastPathAndFullAnalysisConsumeTheSameVisualRunBudget) {
  for (const bool force : {false, true}) {
    SCOPED_TRACE(force);
    BidiResourceBudget budget;
    budget.remaining.runs = 1;
    auto paragraph = BidiParagraph::Analyze({'a', 'b', '.'}, budget, 0, force);
    ASSERT_THAT(paragraph.has_value(), IsTrue());
    const auto runs = paragraph->visualRuns(0, 3, budget);
    ASSERT_THAT(runs.has_value(), IsTrue());
    EXPECT_THAT(VisualOrder(*runs), ElementsAre(0u, 1u, 2u));
    EXPECT_THAT(budget.remaining.runs, testing::Eq(0u));
    EXPECT_THAT(budget.rejected, testing::IsFalse());
  }
}

TEST(BidiParagraphTest, ExhaustionIsStickyAcrossParagraphsAndLineQueries) {
  BidiResourceBudget budget;
  budget.remaining.codepoints = 2;
  EXPECT_THAT(BidiParagraph::Analyze({'a', 'b', 'c'}, budget).has_value(), testing::IsFalse());
  EXPECT_THAT(budget.rejected, IsTrue());
  EXPECT_THAT(BidiParagraph::Analyze({'a'}, budget).has_value(), testing::IsFalse());

  BidiResourceBudget runs;
  runs.remaining.runs = 1;
  auto paragraph = BidiParagraph::Analyze({'a', U'א', 'b'}, runs);
  ASSERT_THAT(paragraph.has_value(), IsTrue());
  EXPECT_THAT(paragraph->visualRuns(0, 3, runs).has_value(), testing::IsFalse());
  EXPECT_THAT(runs.rejected, IsTrue());
}

TEST(BidiParagraphTest, ParagraphAdmissionPrecedesAnalysisAndAlsoChargesTheFastPath) {
  for (bool force : {false, true}) {
    SCOPED_TRACE(force);
    BidiResourceBudget denied;
    denied.remaining.paragraphs = 0;
    EXPECT_THAT(BidiParagraph::Analyze({'a'}, denied, 0, force).has_value(), testing::IsFalse());
    EXPECT_THAT(denied.rejected, IsTrue());
    BidiResourceBudget accepted;
    accepted.remaining.paragraphs = 1;
    EXPECT_THAT(BidiParagraph::Analyze({'a'}, accepted, 0, force).has_value(), IsTrue());
    EXPECT_THAT(accepted.remaining.paragraphs, testing::Eq(0u));
  }
}

TEST(BidiParagraphTest, SourceControlsAndMirrorsHaveSeparateAddressability) {
  for (char32_t control : {0x061cu, 0x200eu, 0x200fu, 0x202au, 0x202bu, 0x202cu, 0x202du, 0x202eu,
                           0x2066u, 0x2067u, 0x2068u, 0x2069u}) {
    EXPECT_THAT(BidiParagraph::IsControl(control), IsTrue()) << static_cast<uint32_t>(control);
  }
  EXPECT_THAT(BidiParagraph::IsControl(0x200d), testing::IsFalse());
  EXPECT_THAT(BidiParagraph::IsControl(0x200c), testing::IsFalse());
  EXPECT_THAT(BidiParagraph::Mirror('('), testing::Eq(')'));
  EXPECT_THAT(BidiParagraph::Mirror('A'), testing::Eq('A'));
}

}  // namespace
}  // namespace donner::svg
