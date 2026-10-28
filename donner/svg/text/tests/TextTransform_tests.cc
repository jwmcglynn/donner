#include "donner/svg/text/TextTransform.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace donner::svg::text_transform {
namespace {
using testing::ElementsAre;
using testing::Eq;

TEST(TextTransformTest, FullWidthRetainsOriginalByteOffsetsAcrossWidthChanges) {
  const auto mapped = MakeFullWidth("A\xc3\xa9\xef\xbd\xb6\xef\xbe\x9e\xf0\x9f\x98\x80", true);
  ASSERT_THAT(mapped.has_value(), Eq(true));
  EXPECT_THAT(mapped->text, Eq("\xef\xbc\xa1\xc3\xa9\xe3\x82\xab\xe3\x82\x99\xf0\x9f\x98\x80"));
  EXPECT_THAT(mapped->sourceByteOffsets, ElementsAre(0u, 1u, 3u, 6u, 9u));
}

TEST(TextTransformTest, FullWidthPreservesAlreadyWideAndUnmappedCharacters) {
  EXPECT_THAT(FullWidthCodepoint(U' ', false), Eq(U' '));
  EXPECT_THAT(FullWidthCodepoint(U' ', true), Eq(char32_t(0x3000)));
  EXPECT_THAT(FullWidthCodepoint(0xff21, true), Eq(char32_t(0xff21)));
  EXPECT_THAT(FullWidthCodepoint(0x00e9, true), Eq(char32_t(0x00e9)));
  EXPECT_THAT(FullWidthCodepoint(0xffe8, true), Eq(char32_t(0x2502)));
  EXPECT_THAT(NeedsFullWidth("\xef\xbc\xa1\xc3\xa9", true), Eq(false));
  EXPECT_THAT(NeedsFullWidth(" ", false), Eq(false));
  EXPECT_THAT(NeedsFullWidth(" ", true), Eq(true));
}

TEST(TextTransformTest, FullWidthBoundsEncodedTextAndIndexPayloadBeforeAllocation) {
  EXPECT_THAT(MakeFullWidth("A", true, 6).has_value(), Eq(false));
  const auto exact = MakeFullWidth("A", true, 7);
  ASSERT_THAT(exact.has_value(), Eq(true));
  EXPECT_THAT(exact->text, Eq("\xef\xbc\xa1"));
  EXPECT_THAT(MakeFullWidth("AB", true, 13).has_value(), Eq(false));
  EXPECT_THAT(MakeFullWidth("AB", true, 14).has_value(), Eq(true));
  EXPECT_THAT(MakeFullWidth("", true, 0).has_value(), Eq(true));
}

}  // namespace
}  // namespace donner::svg::text_transform
