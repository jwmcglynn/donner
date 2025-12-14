#include "donner/base/encoding/Base64.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/ParseResultTestUtils.h"

namespace donner {

using testing::HasSubstr;

TEST(Base64, EmptyString) {
  EXPECT_THAT(DecodeBase64Data(""), ParseResultIs(std::vector<uint8_t>{}));
}

TEST(Base64, ValidBase64) {
  EXPECT_THAT(DecodeBase64Data("TWFu"), ParseResultIs(std::vector<uint8_t>{'M', 'a', 'n'}));
  EXPECT_THAT(DecodeBase64Data("TWE="), ParseResultIs(std::vector<uint8_t>{'M', 'a'}));
  EXPECT_THAT(DecodeBase64Data("TQ=="), ParseResultIs(std::vector<uint8_t>{'M'}));
}

TEST(Base64, ValidBase64WithWhitespace) {
  EXPECT_THAT(DecodeBase64Data(" TWE= "), ParseResultIs(std::vector<uint8_t>{'M', 'a'}));
  EXPECT_THAT(DecodeBase64Data("\nTWE=\n"), ParseResultIs(std::vector<uint8_t>{'M', 'a'}));
}

TEST(Base64, InvalidCharacter) {
  EXPECT_THAT(DecodeBase64Data("TW@="), ParseErrorIs(HasSubstr("Invalid base64 char '@'")));
  EXPECT_THAT(DecodeBase64Data("TWE*"), ParseErrorIs(HasSubstr("Invalid base64 char '*'")));
}

TEST(Base64Encode, EmptyData) {
  EXPECT_EQ(EncodeBase64Data(std::span<const uint8_t>{}), "");
}

TEST(Base64Encode, SingleByte) {
  EXPECT_EQ(EncodeBase64Data(std::span<const uint8_t>{std::vector<uint8_t>{'M'}}), "TQ==");
  EXPECT_EQ(EncodeBase64Data(std::span<const uint8_t>{std::vector<uint8_t>{0x00}}), "AA==");
  EXPECT_EQ(EncodeBase64Data(std::span<const uint8_t>{std::vector<uint8_t>{0xFF}}), "/w==");
}

TEST(Base64Encode, TwoBytes) {
  EXPECT_EQ(EncodeBase64Data(std::span<const uint8_t>{std::vector<uint8_t>{'M', 'a'}}), "TWE=");
  EXPECT_EQ(EncodeBase64Data(std::span<const uint8_t>{std::vector<uint8_t>{0x00, 0x00}}),
            "AAA=");
  EXPECT_EQ(EncodeBase64Data(std::span<const uint8_t>{std::vector<uint8_t>{0xFF, 0xFF}}),
            "//8=");
}

TEST(Base64Encode, ThreeBytes) {
  EXPECT_EQ(EncodeBase64Data(std::span<const uint8_t>{std::vector<uint8_t>{'M', 'a', 'n'}}),
            "TWFu");
  EXPECT_EQ(EncodeBase64Data(std::span<const uint8_t>{std::vector<uint8_t>{0x00, 0x00, 0x00}}),
            "AAAA");
  EXPECT_EQ(EncodeBase64Data(std::span<const uint8_t>{std::vector<uint8_t>{0xFF, 0xFF, 0xFF}}),
            "////");
}

TEST(Base64Encode, MultipleChunks) {
  const std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'};
  EXPECT_EQ(EncodeBase64Data(data), "SGVsbG8gV29ybGQ=");
}

TEST(Base64Encode, AllByteValues) {
  std::vector<uint8_t> allBytes(256);
  for (size_t i = 0; i < 256; ++i) {
    allBytes[i] = static_cast<uint8_t>(i);
  }

  const std::string encoded = EncodeBase64Data(allBytes);
  auto decoded = DecodeBase64Data(encoded);
  ASSERT_TRUE(decoded.hasResult());
  EXPECT_EQ(decoded.result(), allBytes);
}

TEST(Base64Encode, BinaryData) {
  const std::vector<uint8_t> binaryData = {0x00, 0x01, 0x02, 0xFD, 0xFE, 0xFF};
  const std::string encoded = EncodeBase64Data(binaryData);
  auto decoded = DecodeBase64Data(encoded);
  ASSERT_TRUE(decoded.hasResult());
  EXPECT_EQ(decoded.result(), binaryData);
}

TEST(Base64Encode, RoundTrip) {
  const std::vector<std::vector<uint8_t>> testCases = {
      {},
      {'A'},
      {'A', 'B'},
      {'A', 'B', 'C'},
      {'A', 'B', 'C', 'D'},
      {'A', 'B', 'C', 'D', 'E'},
      {'T', 'h', 'e', ' ', 'q', 'u', 'i', 'c', 'k', ' ', 'b', 'r', 'o', 'w', 'n', ' ', 'f', 'o',
       'x'},
  };

  for (const auto& original : testCases) {
    const std::string encoded = EncodeBase64Data(original);
    auto decoded = DecodeBase64Data(encoded);
    ASSERT_TRUE(decoded.hasResult()) << "Failed to decode: " << encoded;
    EXPECT_EQ(decoded.result(), original) << "Round-trip failed for encoded: " << encoded;
  }
}

}  // namespace donner
