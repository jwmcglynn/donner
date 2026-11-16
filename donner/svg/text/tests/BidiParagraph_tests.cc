#include "donner/svg/text/BidiParagraph.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <initializer_list>
#include <ostream>
#include <string_view>
#include <vector>

namespace donner::svg {

/// Prints a run as its scalar range and level.
void PrintTo(const BidiParagraph::Run& run, std::ostream* os) {
  *os << "[" << run.start << ", " << run.end << ") level " << static_cast<int>(run.level);
}

namespace {
using testing::AllOf;
using testing::Contains;
using testing::ElementsAre;
using testing::Field;
using testing::IsTrue;
using testing::Optional;
using testing::ResultOf;

std::vector<uint32_t> VisualOrder(const std::vector<BidiParagraph::Run>& runs) {
  std::vector<uint32_t> result;
  for (const auto& run : runs) {
    for (uint32_t index = run.start; index < run.end; ++index) {
      result.push_back(run.level % 2 ? run.end - 1 - (index - run.start) : index);
    }
  }
  return result;
}

/// The visual order of the scalars in [start, end), or nullopt when the line is rejected. With
/// \p source, scalars that are bidi controls are left out.
std::optional<std::vector<uint32_t>> VisualOrderOf(const BidiParagraph& paragraph, uint32_t start,
                                                   uint32_t end, BidiResourceBudget& budget,
                                                   std::u32string_view source = {}) {
  const auto runs = paragraph.visualRuns(start, end, budget);
  if (!runs) {
    return std::nullopt;
  }
  std::vector<uint32_t> order = VisualOrder(*runs);
  std::erase_if(order, [&](uint32_t index) {
    return index < source.size() && BidiParagraph::IsControl(source[index]);
  });
  return order;
}

/// The resolved levels of scalars [0, count).
std::vector<int> LevelsOf(const BidiParagraph& paragraph, uint32_t count) {
  std::vector<int> levels;
  for (uint32_t index = 0; index < count; ++index) {
    levels.push_back(paragraph.levelAt(index));
  }
  return levels;
}

/// The resolved levels of the scalars at \p indices.
std::vector<int> LevelsOf(const BidiParagraph& paragraph, std::initializer_list<uint32_t> indices) {
  std::vector<int> levels;
  for (const uint32_t index : indices) {
    levels.push_back(paragraph.levelAt(index));
  }
  return levels;
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

TEST(BidiParagraphTest, RightToLeftBaseMovesTrailingNeutralsToTheLineStart) {
  BidiResourceBudget budget;
  // An rtl paragraph keeps the left-to-right word in order and sends the final period, a
  // neutral resolved to the base direction, to the visual start of the line.
  auto paragraph = BidiParagraph::Analyze({'a', 'b', '.'}, budget, /*base=*/1);
  ASSERT_THAT(paragraph.has_value(), IsTrue());
  const auto runs = paragraph->visualRuns(0, 3, budget);
  ASSERT_THAT(runs.has_value(), IsTrue());
  EXPECT_THAT(VisualOrder(*runs), ElementsAre(2u, 0u, 1u));
  EXPECT_THAT(paragraph->levelAt(0), testing::Eq(2));
  EXPECT_THAT(paragraph->levelAt(2), testing::Eq(1));
  EXPECT_THAT(paragraph->paragraphLevelAt(0), testing::Eq(1));
}

TEST(BidiParagraphTest, OverrideGivesEveryScalarTheBaseLevel) {
  const std::u32string_view source = U"ab مر.";
  {
    BidiResourceBudget budget;
    auto paragraph = BidiParagraph::Analyze({source.begin(), source.end()}, budget, /*base=*/1,
                                            /*forceAnalysis=*/false, /*override=*/true);
    ASSERT_THAT(paragraph.has_value(), IsTrue());
    const auto runs = paragraph->visualRuns(0, source.size(), budget);
    ASSERT_THAT(runs.has_value(), IsTrue());
    // Every character, Latin included, runs right to left in reverse source order.
    EXPECT_THAT(VisualOrder(*runs), ElementsAre(5u, 4u, 3u, 2u, 1u, 0u));
    // Script runs stay separate so each shapes with its own script: Arabic, then Latin.
    ASSERT_THAT(runs->size(), testing::Eq(2u));
    EXPECT_THAT((*runs)[0].start, testing::Eq(3u));
    EXPECT_THAT((*runs)[0].end, testing::Eq(6u));
    EXPECT_THAT((*runs)[1].start, testing::Eq(0u));
    for (uint32_t index = 0; index < source.size(); ++index) {
      EXPECT_THAT(paragraph->levelAt(index), testing::Eq(1));
      EXPECT_THAT(paragraph->paragraphLevelAt(index), testing::Eq(1));
    }
  }
  {
    BidiResourceBudget budget;
    auto paragraph = BidiParagraph::Analyze({source.begin(), source.end()}, budget, /*base=*/0,
                                            /*forceAnalysis=*/false, /*override=*/true);
    ASSERT_THAT(paragraph.has_value(), IsTrue());
    const auto runs = paragraph->visualRuns(0, source.size(), budget);
    ASSERT_THAT(runs.has_value(), IsTrue());
    // A left-to-right override keeps even the Arabic letters in source order.
    EXPECT_THAT(VisualOrder(*runs), ElementsAre(0u, 1u, 2u, 3u, 4u, 5u));
    EXPECT_THAT(paragraph->levelAt(3), testing::Eq(0));
  }
  BidiResourceBudget budget;
  EXPECT_THAT(BidiParagraph::Analyze({source.begin(), source.end()}, budget, /*base=*/2,
                                     /*forceAnalysis=*/false, /*override=*/true)
                  .has_value(),
              testing::IsFalse())
      << "an override needs an explicit direction";
}

TEST(BidiParagraphTest, BaseZeroSeparatorsKeepUniformLevelsWithoutForcingAnalysis) {
  for (char32_t cp : {U'\n', U'\r', U'\u2029'}) {
    EXPECT_THAT(BidiParagraph::NeedsAnalysis(cp), testing::IsFalse());
  }
  EXPECT_THAT(BidiParagraph::NeedsAnalysis(U'א'), IsTrue());
  const std::u32string_view source = U"AB\nCD\r\nEF\u2029GH";
  for (bool force : {false, true}) {
    SCOPED_TRACE(force);
    BidiResourceBudget budget;
    auto paragraph = BidiParagraph::Analyze({source.begin(), source.end()}, budget, 0, force);
    ASSERT_THAT(paragraph.has_value(), IsTrue());
    const auto runs = paragraph->visualRuns(0, source.size(), budget);
    ASSERT_THAT(runs.has_value(), IsTrue());
    EXPECT_THAT(VisualOrder(*runs), ElementsAre(0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u));
    for (uint32_t index = 0; index < source.size(); ++index) {
      EXPECT_THAT(paragraph->levelAt(index), testing::Eq(0));
      EXPECT_THAT(paragraph->paragraphLevelAt(index), testing::Eq(0));
    }
  }
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

TEST(BidiParagraphTest, InjectedOverrideReordersOnlyItsScalars) {
  // "ab cd" with an injected RLO before `c` and PDF after `d`, as `unicode-bidi: bidi-override;
  // direction: rtl` on an element holding "cd".
  const std::vector<BidiParagraph::InjectedControl> controls{{3, bidi_control::kRlo},
                                                             {5, bidi_control::kPdf}};
  BidiResourceBudget budget;
  auto paragraph = BidiParagraph::Analyze({'a', 'b', ' ', 'c', 'd'}, budget, /*base=*/0,
                                          /*forceAnalysis=*/false, /*override=*/false, controls);
  ASSERT_NE(paragraph, std::nullopt);
  EXPECT_THAT(VisualOrderOf(*paragraph, 0, 5, budget), Optional(ElementsAre(0u, 1u, 2u, 4u, 3u)));
  EXPECT_THAT(LevelsOf(*paragraph, 5), ElementsAre(0, 0, 0, 1, 1));
  EXPECT_THAT(paragraph->paragraphLevelAt(4), testing::Eq(0));
}

TEST(BidiParagraphTest, InjectedIsolateResolvesItsContentAtItsOwnLevel) {
  // An `rtl` isolate (RLI ... PDI) around "ab" in a left-to-right paragraph raises the Latin
  // letters to level 2 and keeps them in order; the digit after it stays at the paragraph level.
  const std::vector<BidiParagraph::InjectedControl> isolate{{1, bidi_control::kRli},
                                                            {3, bidi_control::kPdi}};
  BidiResourceBudget budget;
  auto paragraph = BidiParagraph::Analyze({'x', 'a', 'b', '1'}, budget, 0, false, false, isolate);
  ASSERT_NE(paragraph, std::nullopt);
  EXPECT_THAT(VisualOrderOf(*paragraph, 0, 4, budget), Optional(ElementsAre(0u, 1u, 2u, 3u)));
  EXPECT_THAT(LevelsOf(*paragraph, 4), ElementsAre(0, 2, 2, 0));
}

TEST(BidiParagraphTest, InjectedIsolateInsideParagraphOverrideKeepsItsOwnOrder) {
  // A right-to-left paragraph override reverses "xy " but not the left-to-right isolate around
  // "ab": isolates do not inherit the enclosing override.
  const std::vector<BidiParagraph::InjectedControl> isolate{{3, bidi_control::kLri},
                                                            {5, bidi_control::kPdi}};
  const std::u32string_view source = U"xy ab";
  BidiResourceBudget budget;
  auto paragraph = BidiParagraph::Analyze({source.begin(), source.end()}, budget, /*base=*/1,
                                          /*forceAnalysis=*/false, /*override=*/true, isolate);
  ASSERT_NE(paragraph, std::nullopt);
  EXPECT_THAT(VisualOrderOf(*paragraph, 0, 5, budget), Optional(ElementsAre(3u, 4u, 2u, 1u, 0u)));
  // The paragraph override resolves through a leading RLO, one embedding above the base level
  // with the same parity, and the isolate opens the next even level inside it.
  EXPECT_THAT(LevelsOf(*paragraph, 5), ElementsAre(3, 3, 3, 4, 4));
  EXPECT_THAT(paragraph->paragraphLevelAt(0), testing::Eq(1));
}

TEST(BidiParagraphTest, OverrideWithInjectedControlsStillSplitsScriptRuns) {
  // The paragraph override around mixed Latin and Arabic still shapes each script separately.
  const std::vector<BidiParagraph::InjectedControl> isolate{{0, bidi_control::kLri},
                                                            {1, bidi_control::kPdi}};
  const std::u32string_view source = U"zab مر";
  BidiResourceBudget budget;
  auto paragraph =
      BidiParagraph::Analyze({source.begin(), source.end()}, budget, 1, false, true, isolate);
  ASSERT_NE(paragraph, std::nullopt);
  const auto runs = paragraph->visualRuns(0, source.size(), budget);
  EXPECT_THAT(runs, Optional(ResultOf(VisualOrder, ElementsAre(5u, 4u, 3u, 2u, 1u, 0u))));
  EXPECT_THAT(runs, Optional(Contains(AllOf(Field(&BidiParagraph::Run::start, 4u),
                                            Field(&BidiParagraph::Run::end, 6u)))))
      << "the Arabic letters form their own run";
}

// Explicit controls in the source text open their levels inside a paragraph override whether or
// not an element also injects controls, as UAX #9 resolves them after the override's own RLO.
TEST(BidiParagraphTest, SourceControlsInsideParagraphOverrideOpenTheirLevels) {
  // x LRO a b PDF y, overridden right to left: the source LRO keeps "ab" in order.
  const std::u32string_view source = U"x\u202dab\u202cy";
  const std::vector<BidiParagraph::InjectedControl> none;
  // An isolate after the last scalar encloses nothing, but still takes the injected path.
  const std::vector<BidiParagraph::InjectedControl> trailing{{6, bidi_control::kRli},
                                                             {6, bidi_control::kPdi}};
  for (const auto* controls : {&none, &trailing}) {
    SCOPED_TRACE(controls->size());
    BidiResourceBudget budget;
    auto paragraph = BidiParagraph::Analyze({source.begin(), source.end()}, budget, /*base=*/1,
                                            /*forceAnalysis=*/false, /*override=*/true, *controls);
    ASSERT_NE(paragraph, std::nullopt);
    EXPECT_THAT(VisualOrderOf(*paragraph, 0, 6, budget, source),
                Optional(ElementsAre(5u, 2u, 3u, 0u)));
    EXPECT_THAT(LevelsOf(*paragraph, {0, 2, 3, 5}), ElementsAre(3, 4, 4, 3));
  }
}

// An element's injected isolate leaves the source controls inside it in effect.
TEST(BidiParagraphTest, SourceControlsInsideAnInjectedIsolateOpenTheirLevels) {
  // a [LRI] RLO b c PDF [PDI] d: the source override reverses "bc" inside the isolate.
  const std::u32string_view source = U"a\u202ebc\u202cd";
  const std::vector<BidiParagraph::InjectedControl> isolate{{1, bidi_control::kLri},
                                                            {5, bidi_control::kPdi}};
  BidiResourceBudget budget;
  auto paragraph =
      BidiParagraph::Analyze({source.begin(), source.end()}, budget, 0, false, false, isolate);
  ASSERT_NE(paragraph, std::nullopt);
  EXPECT_THAT(VisualOrderOf(*paragraph, 0, 6, budget, source),
              Optional(ElementsAre(0u, 3u, 2u, 5u)));
  EXPECT_THAT(LevelsOf(*paragraph, {0, 2, 3, 5}), ElementsAre(0, 3, 3, 0));
}

TEST(BidiParagraphTest, EmbeddingsPastTheMaximumDepthOverflowWithoutRejection) {
  // 130 alternating embeddings open before `a` and close before `b`; UAX #9 stops raising the
  // embedding level at its maximum depth of 125 and ignores the overflowing controls and their
  // pops. `a`, left to right at that odd level, resolves one level higher.
  std::vector<BidiParagraph::InjectedControl> controls;
  for (int depth = 0; depth < 130; ++depth) {
    controls.push_back({0, depth % 2 == 0 ? bidi_control::kRle : bidi_control::kLre});
  }
  for (int depth = 0; depth < 130; ++depth) {
    controls.push_back({1, bidi_control::kPdf});
  }
  BidiResourceBudget budget;
  auto paragraph = BidiParagraph::Analyze({'a', 'b'}, budget, 0, false, false, controls);
  ASSERT_NE(paragraph, std::nullopt);
  EXPECT_THAT(LevelsOf(*paragraph, 2), ElementsAre(126, 0));
  EXPECT_THAT(VisualOrderOf(*paragraph, 0, 2, budget), Optional(ElementsAre(0u, 1u)));
  EXPECT_THAT(budget.remaining.codepoints,
              testing::Eq(BidiResourceBudget::Limits{}.codepoints - 2 - controls.size()));
}

// An override injected for an inline element gives its mixed Latin and Arabic content one level,
// so its runs still split at the script boundary, as a paragraph override's do; otherwise the
// Arabic letters would shape inside a Latin run and lose their joining. Isolate overrides and an
// override nested in another one behave the same way.
TEST(BidiParagraphTest, InjectedOverrideStillSplitsScriptRuns) {
  const std::u32string_view source = U"ab \u0645\u0631";
  const std::vector<std::vector<BidiParagraph::InjectedControl>> cases{
      {{0, bidi_control::kRlo}, {5, bidi_control::kPdf}},
      {{0, bidi_control::kFsi},
       {0, bidi_control::kRlo},
       {5, bidi_control::kPdf},
       {5, bidi_control::kPdi}},
      {{0, bidi_control::kLro},
       {0, bidi_control::kRlo},
       {5, bidi_control::kPdf},
       {5, bidi_control::kPdf}},
  };
  for (const auto& controls : cases) {
    SCOPED_TRACE(controls.size());
    BidiResourceBudget budget;
    auto paragraph =
        BidiParagraph::Analyze({source.begin(), source.end()}, budget, 0, false, false, controls);
    ASSERT_NE(paragraph, std::nullopt);
    const auto runs = paragraph->visualRuns(0, source.size(), budget);
    EXPECT_THAT(runs, Optional(ResultOf(VisualOrder, ElementsAre(4u, 3u, 2u, 1u, 0u))));
    EXPECT_THAT(runs, Optional(Contains(AllOf(Field(&BidiParagraph::Run::start, 3u),
                                              Field(&BidiParagraph::Run::end, 5u)))))
        << "the Arabic letters form their own run";
  }
}

TEST(BidiParagraphTest, InjectedControlsAreBudgetedAndValidated) {
  const std::vector<BidiParagraph::InjectedControl> controls{{0, bidi_control::kRlo},
                                                             {2, bidi_control::kPdf}};
  BidiResourceBudget tight;
  tight.remaining.codepoints = 3;
  EXPECT_EQ(BidiParagraph::Analyze({'a', 'b'}, tight, 0, false, false, controls), std::nullopt);
  EXPECT_THAT(tight.rejected, IsTrue());

  for (const auto& invalid : {std::vector<BidiParagraph::InjectedControl>{{0, 'a'}},
                              std::vector<BidiParagraph::InjectedControl>{{3, bidi_control::kPdf}},
                              std::vector<BidiParagraph::InjectedControl>{
                                  {2, bidi_control::kRlo}, {1, bidi_control::kPdf}}}) {
    BidiResourceBudget budget;
    EXPECT_EQ(BidiParagraph::Analyze({'a', 'b'}, budget, 0, false, false, invalid), std::nullopt);
  }
}

// A paragraph override is charged for its leading control only when it actually injects one.
TEST(BidiParagraphTest, OverrideWithoutInjectedControlsChargesOnlyItsScalars) {
  BidiResourceBudget exact;
  exact.remaining.codepoints = 2;
  auto paragraph = BidiParagraph::Analyze({'a', 'b'}, exact, /*base=*/1, /*forceAnalysis=*/false,
                                          /*override=*/true);
  ASSERT_NE(paragraph, std::nullopt);
  EXPECT_THAT(exact.remaining.codepoints, testing::Eq(0u));
  EXPECT_THAT(VisualOrderOf(*paragraph, 0, 2, exact), Optional(ElementsAre(1u, 0u)));

  const std::vector<BidiParagraph::InjectedControl> isolate{{0, bidi_control::kLri},
                                                            {1, bidi_control::kPdi}};
  BidiResourceBudget injected;
  injected.remaining.codepoints = 4;
  EXPECT_EQ(BidiParagraph::Analyze({'a', 'b'}, injected, 1, false, true, isolate), std::nullopt)
      << "two scalars, two controls and the leading override exceed four";
}

// Controls around no scalars enclose nothing: they are dropped, so the paragraph still has no
// scalar to address.
TEST(BidiParagraphTest, ControlsWithoutScalarsAddNoAddressableScalar) {
  const std::vector<BidiParagraph::InjectedControl> controls{{0, bidi_control::kRlo}};
  BidiResourceBudget budget;
  auto paragraph = BidiParagraph::Analyze({}, budget, 0, false, false, controls);
  ASSERT_NE(paragraph, std::nullopt);
  EXPECT_THAT(paragraph->visualRuns(0, 0, budget), Optional(testing::IsEmpty()));
  EXPECT_THAT(budget.remaining.codepoints, testing::Eq(BidiResourceBudget::Limits{}.codepoints));
  EXPECT_EQ(paragraph->visualRuns(0, 1, budget), std::nullopt);
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
