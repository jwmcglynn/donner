#include "donner/base/xml/Save.h"

#include <algorithm>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/FileOffset.h"
#include "donner/base/xml/SourceDocument.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::xml {
namespace {

using ::testing::Eq;
using ::testing::StrEq;

TEST(SaveTests, AppliesPlanAndReturnsDiagnostics) {
  SourceDocument source{RcString("hello world")};
  ReplaceSpanPlanner::ReplaceSpan capitalize{
      SourceDocument::Replacement{FileOffsetRange{FileOffset::Offset(0), FileOffset::Offset(1)},
                                  RcString("H")},
      std::nullopt};
  ReplaceSpanPlanner::ReplaceSpan substitute{
      SourceDocument::Replacement{FileOffsetRange{FileOffset::Offset(6), FileOffset::Offset(11)},
                                  RcString("there")},
      std::nullopt};

  auto result = SaveDocument(source, {capitalize, substitute});
  ASSERT_TRUE(result.hasResult());

  EXPECT_THAT(std::string_view(result.result().updatedText), StrEq("Hello there"));
  EXPECT_FALSE(result.result().diagnostics.usedFallback);
  ASSERT_EQ(result.result().diagnostics.appliedReplacements.size(), 2u);
  EXPECT_EQ(result.result().diagnostics.appliedReplacements[0].replacement, RcString("H"));
  EXPECT_EQ(result.result().diagnostics.appliedReplacements[1].replacement, RcString("there"));

  auto mapped = result.result().offsetMap.translateRange(
      FileOffsetRange{FileOffset::Offset(6), FileOffset::Offset(11)});
  EXPECT_TRUE(mapped.start.offset.has_value());
  EXPECT_TRUE(mapped.end.offset.has_value());
  EXPECT_EQ(mapped.start.offset.value(), 6u);
  EXPECT_EQ(mapped.end.offset.value(), 11u);
}

TEST(SaveTests, PreservesWhitespaceAndCommentsWithSpanAlignedEdits) {
  const std::string original =
      "<svg>\n  <!-- leading -->\n  <rect width=\"10\" height=\"20\"/>\n"
      "  <text>label</text>\n<!-- trailing -->\n</svg>\n";

  const auto widthPos = original.find("10");
  const auto labelPos = original.find("label");
  ASSERT_NE(widthPos, std::string::npos);
  ASSERT_NE(labelPos, std::string::npos);

  SourceDocument source{RcString(original)};
  ReplaceSpanPlanner::ReplaceSpan widthEdit{
      SourceDocument::Replacement{
          FileOffsetRange{FileOffset::Offset(widthPos), FileOffset::Offset(widthPos + 2)},
          RcString("12")},
      std::nullopt};
  ReplaceSpanPlanner::ReplaceSpan textEdit{
      SourceDocument::Replacement{
          FileOffsetRange{FileOffset::Offset(labelPos), FileOffset::Offset(labelPos + 5)},
          RcString("caption")},
      std::nullopt};

  auto result = SaveDocument(source, {widthEdit, textEdit});
  ASSERT_TRUE(result.hasResult());

  const std::string expected =
      "<svg>\n  <!-- leading -->\n  <rect width=\"12\" height=\"20\"/>\n"
      "  <text>caption</text>\n<!-- trailing -->\n</svg>\n";
  EXPECT_THAT(std::string_view(result.result().updatedText), StrEq(expected));
  EXPECT_FALSE(result.result().diagnostics.usedFallback);
  ASSERT_EQ(result.result().diagnostics.appliedReplacements.size(), 2u);
  EXPECT_EQ(result.result().diagnostics.appliedReplacements[0].replacement, RcString("12"));
  EXPECT_EQ(result.result().diagnostics.appliedReplacements[1].replacement, RcString("caption"));
}

TEST(SaveTests, RandomizedEditsMatchManualApplication) {
  const std::string baseLine = "<row a=\"100\">payload</row>\n";
  std::string original;
  for (int i = 0; i < 50; ++i) {
    original.append(baseLine);
  }

  std::mt19937 rng(1337);
  std::uniform_int_distribution<int> startDist(0, static_cast<int>(original.size() - 1));
  std::uniform_int_distribution<int> lenDist(1, 6);
  std::vector<char> choices = {'x', 'y', 'z', '1', '2', '3'};

  for (int iteration = 0; iteration < 20; ++iteration) {
    std::vector<ReplaceSpanPlanner::ReplaceSpan> edits;
    std::vector<std::pair<size_t, size_t>> ranges;

    const int editCount = 8;
    while (static_cast<int>(edits.size()) < editCount) {
      const size_t start = static_cast<size_t>(startDist(rng));
      const size_t len = static_cast<size_t>(lenDist(rng));
      if (start + len > original.size()) {
        continue;
      }

      const auto overlaps = std::any_of(ranges.begin(), ranges.end(), [&](const auto& range) {
        return !(start + len <= range.first || range.second <= start);
      });
      if (overlaps) {
        continue;
      }

      std::string replacement;
      for (size_t i = 0; i < len; ++i) {
        replacement.push_back(choices[startDist(rng) % choices.size()]);
      }

      ranges.emplace_back(start, start + len);
      edits.push_back(ReplaceSpanPlanner::ReplaceSpan{
          SourceDocument::Replacement{
              FileOffsetRange{FileOffset::Offset(start), FileOffset::Offset(start + len)},
              RcString(replacement)},
          std::nullopt});
    }

    std::sort(edits.begin(), edits.end(), [](const auto& lhs, const auto& rhs) {
      return lhs.replacement.range.start.offset.value() <
             rhs.replacement.range.start.offset.value();
    });

    std::string expected;
    size_t cursor = 0;
    for (const auto& edit : edits) {
      const auto& range = edit.replacement.range;
      expected.append(original.substr(cursor, range.start.offset.value() - cursor));
      expected.append(edit.replacement.replacement.data(), edit.replacement.replacement.size());
      cursor = range.end.offset.value();
    }
    expected.append(original.substr(cursor));

    SourceDocument source{RcString(original)};
    auto result = SaveDocument(source, edits);
    ASSERT_TRUE(result.hasResult());
    EXPECT_THAT(std::string_view(result.result().updatedText), StrEq(expected))
        << "iteration " << iteration;

    const auto randomIndex = static_cast<size_t>(startDist(rng) % original.size());
    const auto mapped = result.result().offsetMap.translateRange(
        FileOffsetRange{FileOffset::Offset(randomIndex), FileOffset::Offset(randomIndex + 1)});
    EXPECT_TRUE(mapped.start.offset.has_value());
    EXPECT_TRUE(mapped.end.offset.has_value());
  }
}

TEST(SaveTests, StressesLargeDocumentPerformance) {
  std::string original(200'000, 'a');
  std::vector<ReplaceSpanPlanner::ReplaceSpan> edits;
  edits.reserve(1000);

  for (size_t i = 0; i < 1000; ++i) {
    const size_t start = i * 150;
    const size_t end = start + 50;
    ASSERT_LT(end, original.size());

    std::string replacement(50, static_cast<char>('b' + (i % 10)));
    edits.push_back(ReplaceSpanPlanner::ReplaceSpan{
        SourceDocument::Replacement{
            FileOffsetRange{FileOffset::Offset(start), FileOffset::Offset(end)},
            RcString(replacement)},
        std::nullopt});
  }

  SourceDocument source{RcString(original)};
  auto result = SaveDocument(source, edits);
  ASSERT_TRUE(result.hasResult());
  EXPECT_EQ(result.result().updatedText.size(), original.size());
  EXPECT_FALSE(result.result().diagnostics.usedFallback);
  EXPECT_EQ(result.result().diagnostics.appliedReplacements.size(), edits.size());

  for (size_t i = 0; i < edits.size(); ++i) {
    const auto& applied = result.result().diagnostics.appliedReplacements[i];
    EXPECT_EQ(applied.range.start.offset.value(), edits[i].replacement.range.start.offset.value());
    EXPECT_EQ(applied.range.end.offset.value(), edits[i].replacement.range.end.offset.value());
  }
}

TEST(SaveTests, RejectsFallbackWhenDisallowed) {
  SourceDocument source{RcString("<svg></svg>")};
  ReplaceSpanPlanner::ReplaceSpan missingOffsets{
      SourceDocument::Replacement{
          FileOffsetRange{FileOffset::EndOfString(), FileOffset::EndOfString()},
          RcString("<rect/>")},
      SourceDocument::Replacement{FileOffsetRange{FileOffset::Offset(5), FileOffset::Offset(5)},
                                  RcString("<rect/>")}};

  SaveOptions options;
  options.allowFallbackExpansion = false;

  auto result = SaveDocument(source, {missingOffsets}, options);
  ASSERT_FALSE(result.hasResult());
  EXPECT_THAT(result.error().reason,
              RcString("Fallback replacements are disallowed by SaveOptions"));
}

TEST(SaveTests, PropagatesPlannerError) {
  SourceDocument source{RcString("abcde")};
  ReplaceSpanPlanner::ReplaceSpan first{
      SourceDocument::Replacement{FileOffsetRange{FileOffset::Offset(0), FileOffset::Offset(3)},
                                  RcString("xx")},
      std::nullopt};
  ReplaceSpanPlanner::ReplaceSpan overlap{
      SourceDocument::Replacement{FileOffsetRange{FileOffset::Offset(2), FileOffset::Offset(4)},
                                  RcString("yy")},
      std::nullopt};

  auto result = SaveDocument(source, {first, overlap});
  ASSERT_FALSE(result.hasResult());
  EXPECT_THAT(result.error().reason,
              RcString("Overlapping replacements with no compatible fallback"));
}

}  // namespace
}  // namespace donner::xml
