#include "donner/base/parser/DataUrlParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/RcString.h"
#include "gmock/gmock.h"

namespace donner::parser {

using testing::Eq;
using testing::VariantWith;

TEST(DataUrlParser, Invalid) {
  EXPECT_THAT(DataUrlParser::Parse("data:"),
              VariantWith<DataUrlParserError>(Eq(DataUrlParserError::InvalidDataUrl)));
  EXPECT_THAT(DataUrlParser::Parse("data:image/png;base64"),
              VariantWith<DataUrlParserError>(Eq(DataUrlParserError::InvalidDataUrl)));
}

TEST(DataUrlParser, ExternalUrl) {
  auto result = DataUrlParser::Parse("http://example.com/foo.png");
  ASSERT_TRUE(std::holds_alternative<DataUrlParser::Result>(result));
  const auto& r = std::get<DataUrlParser::Result>(result);
  EXPECT_EQ(r.kind, DataUrlParser::Result::Kind::ExternalUrl);
  ASSERT_TRUE(std::holds_alternative<RcString>(r.payload));
  EXPECT_EQ(std::get<RcString>(r.payload), "http://example.com/foo.png");
}

TEST(DataUrlParser, SimpleText) {
  auto result = DataUrlParser::Parse("data:,Hello%2C%20World%21");
  ASSERT_TRUE(std::holds_alternative<DataUrlParser::Result>(result));
  const auto& r = std::get<DataUrlParser::Result>(result);
  EXPECT_EQ(r.kind, DataUrlParser::Result::Kind::Data);
  EXPECT_EQ(r.mimeType, "");

  std::string_view expectedStr = "Hello, World!";
  EXPECT_THAT(r.payload,
              testing::VariantWith<std::vector<uint8_t>>(testing::ElementsAreArray(expectedStr)));
}

TEST(DataUrlParser, SimpleBase64) {
  auto result = DataUrlParser::Parse("data:text/plain;base64,SGVsbG8sIFdvcmxkIQ==");
  ASSERT_TRUE(std::holds_alternative<DataUrlParser::Result>(result));
  const auto& r = std::get<DataUrlParser::Result>(result);
  EXPECT_EQ(r.kind, DataUrlParser::Result::Kind::Data);
  EXPECT_EQ(r.mimeType, "text/plain");

  const std::string_view expectedStr = "Hello, World!";
  EXPECT_THAT(r.payload,
              testing::VariantWith<std::vector<uint8_t>>(testing::ElementsAreArray(expectedStr)));
}

TEST(DataUrlParser, MimeTypeWithParameters) {
  auto result =
      DataUrlParser::Parse("data:text/html;charset=utf-8,%3Ch1%3EHello%2C%20World%21%3C%2Fh1%3E");
  ASSERT_TRUE(std::holds_alternative<DataUrlParser::Result>(result));
  const auto& r = std::get<DataUrlParser::Result>(result);
  EXPECT_EQ(r.kind, DataUrlParser::Result::Kind::Data);
  EXPECT_EQ(r.mimeType, "text/html");

  const std::string_view expectedStr = "<h1>Hello, World!</h1>";
  EXPECT_THAT(r.payload,
              testing::VariantWith<std::vector<uint8_t>>(testing::ElementsAreArray(expectedStr)));
}

TEST(DataUrlParser, MimeTypeWithParametersAndBase64) {
  auto result =
      DataUrlParser::Parse("data:text/html;charset=utf-8;base64,PGgxPkhlbGxvLCBXb3JsZCE8L2gxPg==");
  ASSERT_TRUE(std::holds_alternative<DataUrlParser::Result>(result));
  const auto& r = std::get<DataUrlParser::Result>(result);
  EXPECT_EQ(r.kind, DataUrlParser::Result::Kind::Data);
  EXPECT_EQ(r.mimeType, "text/html");

  const std::string_view expectedStr = "<h1>Hello, World!</h1>";
  EXPECT_THAT(r.payload,
              testing::VariantWith<std::vector<uint8_t>>(testing::ElementsAreArray(expectedStr)));
}

TEST(DataUrlParser, FontWoff) {
  auto result = DataUrlParser::Parse("data:application/x-font-woff;charset=utf-8;base64,d09GRg==");
  ASSERT_TRUE(std::holds_alternative<DataUrlParser::Result>(result));
  const auto& r = std::get<DataUrlParser::Result>(result);
  EXPECT_EQ(r.kind, DataUrlParser::Result::Kind::Data);
  EXPECT_EQ(r.mimeType, "application/x-font-woff");

  EXPECT_THAT(r.payload,
              testing::VariantWith<std::vector<uint8_t>>(testing::ElementsAre('w', 'O', 'F', 'F')));
}

/// @test that a valid URL-encoded data URL with an explicit MIME type is decoded.
TEST(DataUrlParser, UrlEncodedWithMime) {
  // Here, the comma separates the MIME type from the data.
  // "hello%20world" should URL-decode to "hello world".
  auto result = DataUrlParser::Parse("data:text/plain,hello%20world");
  ASSERT_TRUE(std::holds_alternative<DataUrlParser::Result>(result));
  const auto& r = std::get<DataUrlParser::Result>(result);
  EXPECT_EQ(r.kind, DataUrlParser::Result::Kind::Data);
  EXPECT_EQ(r.mimeType, "text/plain");

  const std::string_view expectedStr = "hello world";
  EXPECT_THAT(r.payload,
              testing::VariantWith<std::vector<uint8_t>>(testing::ElementsAreArray(expectedStr)));
}

/// @test that a valid URL-encoded data URL without an explicit MIME type is decoded.
TEST(DataUrlParser, UrlEncodedNoMime) {
  // With no explicit MIME type, defaults to text/plain.
  auto result = DataUrlParser::Parse("data:,hello%20world");
  ASSERT_TRUE(std::holds_alternative<DataUrlParser::Result>(result));
  const auto& r = std::get<DataUrlParser::Result>(result);
  EXPECT_EQ(r.kind, DataUrlParser::Result::Kind::Data);
  EXPECT_EQ(r.mimeType, "");

  const std::string_view expectedStr = "hello world";
  EXPECT_THAT(r.payload,
              testing::VariantWith<std::vector<uint8_t>>(testing::ElementsAreArray(expectedStr)));
}

/// @test that an invalid base64 data URL is handled appropriately.
TEST(DataUrlParser, InvalidBase64) {
  // "!!!!" is not valid base64 and should result in an error.
  auto result = DataUrlParser::Parse("data:image/png;base64,!!!!");
  EXPECT_THAT(result, VariantWith<DataUrlParserError>(Eq(DataUrlParserError::InvalidDataUrl)));
}

}  // namespace donner::parser
