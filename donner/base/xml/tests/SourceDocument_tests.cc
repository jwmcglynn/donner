#include "donner/base/xml/SourceDocument.h"

#include <gtest/gtest.h>

namespace donner::xml {

TEST(SourceDocumentTest, AppliesSingleReplacementAndUpdatesOffsets) {
  SourceDocument document(RcString("alpha beta gamma"));

  SourceDocument::Replacement replacement{
      FileOffsetRange{FileOffset::Offset(6), FileOffset::Offset(10)}, RcString("BETA")};

  auto result = document.applyReplacements({replacement});
  ASSERT_TRUE(result.hasResult());

  const auto& updated = result.result();
  EXPECT_EQ(std::string_view(updated.text), "alpha BETA gamma");

  const auto startOffset =
      updated.offsetMap.translateOffset(FileOffset::OffsetWithLineInfo(6, {1, 6}));
  EXPECT_EQ(startOffset.offset.value(), 6u);
  EXPECT_EQ(startOffset.lineInfo->line, 1u);
  EXPECT_EQ(startOffset.lineInfo->offsetOnLine, 6u);

  const auto insideReplaced =
      updated.offsetMap.translateOffset(FileOffset::OffsetWithLineInfo(8, {1, 8}));
  EXPECT_EQ(insideReplaced.offset.value(), 8u);
  EXPECT_EQ(insideReplaced.lineInfo->offsetOnLine, 8u);

  const auto afterReplacement =
      updated.offsetMap.translateOffset(FileOffset::OffsetWithLineInfo(12, {1, 12}));
  EXPECT_EQ(afterReplacement.offset.value(), 12u);
  EXPECT_EQ(afterReplacement.lineInfo->offsetOnLine, 12u);
}

TEST(SourceDocumentTest, MergesMultipleReplacementsAndLineInfo) {
  SourceDocument document(RcString("line1\nline2 middle\nline3 tail"));

  std::vector<SourceDocument::Replacement> replacements = {
      {FileOffsetRange{FileOffset::Offset(6), FileOffset::Offset(11)}, RcString("TWO")},
      {FileOffsetRange{FileOffset::Offset(19), FileOffset::Offset(24)}, RcString("LINE-THREE")}};

  auto result = document.applyReplacements(replacements);
  ASSERT_TRUE(result.hasResult());

  const auto& updated = result.result();
  EXPECT_EQ(std::string_view(updated.text), "line1\nTWO middle\nLINE-THREE tail");

  const auto translatedNewline =
      updated.offsetMap.translateOffset(FileOffset::OffsetWithLineInfo(18, {2, 6}));
  EXPECT_EQ(translatedNewline.offset.value(), 16u);
  ASSERT_TRUE(translatedNewline.lineInfo.has_value());
  EXPECT_EQ(translatedNewline.lineInfo->line, 2u);
  EXPECT_EQ(translatedNewline.lineInfo->offsetOnLine, 10u);

  const auto tailOffset =
      updated.offsetMap.translateOffset(FileOffset::OffsetWithLineInfo(24, {3, 1}));
  EXPECT_EQ(tailOffset.offset.value(), 27u);
  EXPECT_EQ(tailOffset.lineInfo->line, 3u);
  EXPECT_EQ(tailOffset.lineInfo->offsetOnLine, 10u);
}

TEST(SourceDocumentTest, TranslatesRangesForSubsequentEdits) {
  SourceDocument document(RcString("one two three four"));

  std::vector<SourceDocument::Replacement> replacements = {
      {FileOffsetRange{FileOffset::Offset(4), FileOffset::Offset(7)}, RcString("2")}};

  auto firstResult = document.applyReplacements(replacements);
  ASSERT_TRUE(firstResult.hasResult());

  const auto& applied = firstResult.result();
  EXPECT_EQ(std::string_view(applied.text), "one 2 three four");

  const FileOffsetRange translatedThree = applied.offsetMap.translateRange(
      FileOffsetRange{FileOffset::Offset(8), FileOffset::Offset(13)});
  EXPECT_EQ(translatedThree.start.offset.value(), 6u);
  EXPECT_EQ(translatedThree.end.offset.value(), 11u);

  SourceDocument updated(applied.text);
  auto secondResult = updated.applyReplacements({{
      translatedThree,
      RcString("THREE"),
  }});
  ASSERT_TRUE(secondResult.hasResult());
  EXPECT_EQ(std::string_view(secondResult.result().text), "one 2 THREE four");
}

TEST(SourceDocumentTest, RejectsOverlappingReplacements) {
  SourceDocument document(RcString("abcdef"));

  std::vector<SourceDocument::Replacement> replacements = {
      {FileOffsetRange{FileOffset::Offset(1), FileOffset::Offset(3)}, RcString("X")},
      {FileOffsetRange{FileOffset::Offset(2), FileOffset::Offset(4)}, RcString("Y")}};

  auto result = document.applyReplacements(replacements);
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().reason, RcString("Replacements must be non-overlapping and ordered"));
  EXPECT_EQ(result.error().location.offset.value(), 2u);
}

}  // namespace donner::xml
