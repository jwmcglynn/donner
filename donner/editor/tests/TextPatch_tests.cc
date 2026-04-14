#include "donner/editor/TextPatch.h"

#include <gtest/gtest.h>

namespace donner::editor {
namespace {

TEST(TextPatch, EmptyPatchesNoOp) {
  std::string source = "hello";
  auto result = applyPatches(source, {});
  EXPECT_EQ(result.applied, 0u);
  EXPECT_EQ(result.rejectedBounds, 0u);
  EXPECT_EQ(source, "hello");
}

TEST(TextPatch, SingleReplace) {
  std::string source = "fill: red";
  TextPatch patch{6, 3, "blue"};
  auto result = applyPatches(source, {{patch}});
  EXPECT_EQ(result.applied, 1u);
  EXPECT_EQ(source, "fill: blue");
}

TEST(TextPatch, SingleInsert) {
  std::string source = "<rect/>";
  // Insert " fill=\"red\"" before the "/>"
  TextPatch patch{5, 0, " fill=\"red\""};
  auto result = applyPatches(source, {{patch}});
  EXPECT_EQ(result.applied, 1u);
  EXPECT_EQ(source, "<rect fill=\"red\"/>");
}

TEST(TextPatch, SingleDelete) {
  std::string source = "fill: red; stroke: blue";
  // Delete "; stroke: blue" (offset 9, length 14)
  TextPatch patch{9, 14, ""};
  auto result = applyPatches(source, {{patch}});
  EXPECT_EQ(result.applied, 1u);
  EXPECT_EQ(source, "fill: red");
}

TEST(TextPatch, MultiplePatchesAppliedInDescendingOrder) {
  // Two non-overlapping patches. Regardless of input order, they should
  // both apply correctly because applyPatches sorts by descending offset.
  std::string source = "fill: red; stroke: green";
  std::vector<TextPatch> patches = {
      {6, 3, "blue"},      // replace "red" with "blue"
      {19, 5, "orange"},   // replace "green" with "orange"
  };
  auto result = applyPatches(source, patches);
  EXPECT_EQ(result.applied, 2u);
  EXPECT_EQ(source, "fill: blue; stroke: orange");
}

TEST(TextPatch, PatchesInForwardOrderStillWork) {
  // Same patches as above but in forward order — the sort handles it.
  std::string source = "fill: red; stroke: green";
  std::vector<TextPatch> patches = {
      {19, 5, "orange"},   // higher offset first in input
      {6, 3, "blue"},
  };
  auto result = applyPatches(source, patches);
  EXPECT_EQ(result.applied, 2u);
  EXPECT_EQ(source, "fill: blue; stroke: orange");
}

TEST(TextPatch, BoundsCheckRejectsOverflow) {
  std::string source = "short";
  TextPatch patch{3, 10, "x"};  // offset 3 + length 10 > 5
  auto result = applyPatches(source, {{patch}});
  EXPECT_EQ(result.applied, 0u);
  EXPECT_EQ(result.rejectedBounds, 1u);
  EXPECT_EQ(source, "short");
}

TEST(TextPatch, BoundsCheckRejectsOffsetPastEnd) {
  std::string source = "abc";
  TextPatch patch{10, 0, "x"};  // offset past end
  auto result = applyPatches(source, {{patch}});
  EXPECT_EQ(result.applied, 0u);
  EXPECT_EQ(result.rejectedBounds, 1u);
}

TEST(TextPatch, InsertAtEnd) {
  std::string source = "hello";
  TextPatch patch{5, 0, " world"};
  auto result = applyPatches(source, {{patch}});
  EXPECT_EQ(result.applied, 1u);
  EXPECT_EQ(source, "hello world");
}

TEST(TextPatch, InsertAtBeginning) {
  std::string source = "world";
  TextPatch patch{0, 0, "hello "};
  auto result = applyPatches(source, {{patch}});
  EXPECT_EQ(result.applied, 1u);
  EXPECT_EQ(source, "hello world");
}

TEST(TextPatch, ReplaceEntireString) {
  std::string source = "old content";
  TextPatch patch{0, 11, "new"};
  auto result = applyPatches(source, {{patch}});
  EXPECT_EQ(result.applied, 1u);
  EXPECT_EQ(source, "new");
}

TEST(TextPatch, EmptySourceInsert) {
  std::string source;
  TextPatch patch{0, 0, "hello"};
  auto result = applyPatches(source, {{patch}});
  EXPECT_EQ(result.applied, 1u);
  EXPECT_EQ(source, "hello");
}

TEST(TextPatch, MixOfValidAndInvalidPatches) {
  std::string source = "abcdefgh";
  std::vector<TextPatch> patches = {
      {0, 1, "A"},       // valid: replace 'a' with 'A'
      {100, 1, "X"},     // invalid: offset way past end
      {7, 1, "H"},       // valid: replace 'h' with 'H'
  };
  auto result = applyPatches(source, patches);
  EXPECT_EQ(result.applied, 2u);
  EXPECT_EQ(result.rejectedBounds, 1u);
  EXPECT_EQ(source, "AbcdefgH");
}

TEST(TextPatch, ReplacementLongerThanOriginal) {
  std::string source = "ab";
  TextPatch patch{1, 1, "xyz"};  // replace 'b' with 'xyz'
  auto result = applyPatches(source, {{patch}});
  EXPECT_EQ(result.applied, 1u);
  EXPECT_EQ(source, "axyz");
}

TEST(TextPatch, ReplacementShorterThanOriginal) {
  std::string source = "abcde";
  TextPatch patch{1, 3, "x"};  // replace 'bcd' with 'x'
  auto result = applyPatches(source, {{patch}});
  EXPECT_EQ(result.applied, 1u);
  EXPECT_EQ(source, "axe");
}

}  // namespace
}  // namespace donner::editor
