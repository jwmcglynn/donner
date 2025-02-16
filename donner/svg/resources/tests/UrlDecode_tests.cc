#include "donner/svg/resources/UrlDecode.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace donner::svg {

using testing::ElementsAre;

TEST(UrlDecode, EmptyString) {
  EXPECT_THAT(UrlDecode(""), std::vector<uint8_t>{});
}

TEST(UrlDecode, NoPercentEncoding) {
  // No percent signs: output is identical to input.
  EXPECT_THAT(UrlDecode("Hello"), ElementsAre('H', 'e', 'l', 'l', 'o'));
}

TEST(UrlDecode, ValidPercentEncoding) {
  // "%20" is valid and decodes to a space.
  EXPECT_THAT(UrlDecode("Hello%20World"),
              ElementsAre('H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'));
}

TEST(UrlDecode, TrailingPercent) {
  // A '%' with no following characters remains literal.
  EXPECT_THAT(UrlDecode("abc%"), ElementsAre('a', 'b', 'c', '%'));
}

TEST(UrlDecode, PercentWithSingleFollowingChar) {
  // A '%' followed by only one character is not a valid sequence.
  EXPECT_THAT(UrlDecode("abc%4"), ElementsAre('a', 'b', 'c', '%', '4'));
}

TEST(UrlDecode, InvalidPercentEncoding) {
  // Here, "%2X" is invalid because 'X' is not a valid hex digit.
  // The '%' is not decoded but copied verbatim.
  EXPECT_THAT(UrlDecode("Hello%2X"), ElementsAre('H', 'e', 'l', 'l', 'o', '%', '2', 'X'));
}

TEST(UrlDecode, MultiplePercentSequences) {
  // "%25" is valid and decodes to '%'.
  EXPECT_THAT(UrlDecode("100%25 sure"), ElementsAre('1', '0', '0', 0x25, ' ', 's', 'u', 'r', 'e'));
}

TEST(UrlDecode, MixedValidInvalidPercentSequences) {
  // In "A%41B%G1C":
  //   - "%41" is valid and decodes to 'A'
  //   - "%G1" is invalid because 'G' is not a valid hex digit,
  //     so '%' is copied literally along with 'G' and '1'.
  EXPECT_THAT(UrlDecode("A%41B%G1C"), ElementsAre('A', 'A', 'B', '%', 'G', '1', 'C'));
}

TEST(UrlDecode, UTF8PercentDecoding) {
  // "caf%C3%A9" should decode to the bytes for "caf" followed by 0xC3, 0xA9,
  // which is the UTF-8 encoding for 'é'.
  EXPECT_THAT(UrlDecode("caf%C3%a9"),
              ElementsAre('c', 'a', 'f', static_cast<uint8_t>(0xC3), static_cast<uint8_t>(0xA9)));
}

TEST(UrlDecode, PlusSignRemainsUnchanged) {
  // Plus signs are not converted to spaces in this decoder.
  EXPECT_THAT(UrlDecode("Hello+World"),
              ElementsAre('H', 'e', 'l', 'l', 'o', '+', 'W', 'o', 'r', 'l', 'd'));
}

}  // namespace donner::svg
