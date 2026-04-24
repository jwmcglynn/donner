#include "donner/editor/ContentSniffer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace donner::editor {
namespace {

std::vector<uint8_t> Bytes(std::string_view s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

TEST(ContentSnifferTest, EmptyInputReportsEmpty) {
  auto r = DescribeNonSvgBytes({});
  ASSERT_TRUE(r.has_value());
  EXPECT_NE(r->find("Empty"), std::string::npos);
}

TEST(ContentSnifferTest, WhitespaceOnlyReportsWhitespace) {
  auto bytes = Bytes("   \n\t\r  ");
  auto r = DescribeNonSvgBytes(bytes);
  ASSERT_TRUE(r.has_value());
  EXPECT_NE(r->find("whitespace"), std::string::npos);
}

TEST(ContentSnifferTest, HtmlDoctypeIsFlagged) {
  auto bytes = Bytes("<!DOCTYPE html>\n<html><head></head></html>");
  auto r = DescribeNonSvgBytes(bytes);
  ASSERT_TRUE(r.has_value());
  EXPECT_NE(r->find("HTML page"), std::string::npos);
}

TEST(ContentSnifferTest, HtmlDoctypeLowercaseIsFlagged) {
  auto bytes = Bytes("<!doctype html>\n...");
  auto r = DescribeNonSvgBytes(bytes);
  ASSERT_TRUE(r.has_value());
  EXPECT_NE(r->find("HTML page"), std::string::npos);
}

TEST(ContentSnifferTest, HtmlWithoutDoctypeStillFlagged) {
  auto bytes = Bytes("<html><body>hi</body></html>");
  auto r = DescribeNonSvgBytes(bytes);
  ASSERT_TRUE(r.has_value());
  EXPECT_NE(r->find("HTML page"), std::string::npos);
}

TEST(ContentSnifferTest, LeadingBomAndWhitespaceSkippedBeforeDetection) {
  // UTF-8 BOM + newline + HTML.
  std::vector<uint8_t> bytes = {0xEF, 0xBB, 0xBF, '\n'};
  auto more = Bytes("<html>");
  bytes.insert(bytes.end(), more.begin(), more.end());
  auto r = DescribeNonSvgBytes(bytes);
  ASSERT_TRUE(r.has_value());
  EXPECT_NE(r->find("HTML page"), std::string::npos);
}

TEST(ContentSnifferTest, JsonIsFlagged) {
  auto bytes = Bytes("{\"foo\": 42}");
  auto r = DescribeNonSvgBytes(bytes);
  ASSERT_TRUE(r.has_value());
  EXPECT_NE(r->find("JSON"), std::string::npos);
}

TEST(ContentSnifferTest, JsonArrayIsFlagged) {
  auto bytes = Bytes("[1, 2, 3]");
  auto r = DescribeNonSvgBytes(bytes);
  ASSERT_TRUE(r.has_value());
  EXPECT_NE(r->find("JSON"), std::string::npos);
}

TEST(ContentSnifferTest, SvgPrefixDefersToParser) {
  auto bytes = Bytes("<svg xmlns=\"http://www.w3.org/2000/svg\"/>");
  EXPECT_FALSE(DescribeNonSvgBytes(bytes).has_value());
}

TEST(ContentSnifferTest, XmlDeclarationDefersToParser) {
  auto bytes = Bytes("<?xml version=\"1.0\"?>\n<svg/>");
  EXPECT_FALSE(DescribeNonSvgBytes(bytes).has_value());
}

TEST(ContentSnifferTest, NonXmlBinaryFlagsFirstByte) {
  std::vector<uint8_t> bytes = {'h', 'e', 'l', 'l', 'o'};
  auto r = DescribeNonSvgBytes(bytes);
  ASSERT_TRUE(r.has_value());
  EXPECT_NE(r->find("doesn't look like XML"), std::string::npos);
}

TEST(ContentSnifferTest, NonHtmlDoctypeFallsThroughToParser) {
  // SVG 1.1 documents sometimes ship a `<!DOCTYPE svg ...>` preamble.
  // We don't have enough signal at the prefix level to distinguish that
  // from `<!DOCTYPE math>` or similar non-SVG XML, so we defer to the
  // parser — it'll accept `<!DOCTYPE svg>` happily, or complain with a
  // precise error if the body isn't SVG.
  auto bytes = Bytes("<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.1//EN\">\n<svg/>");
  EXPECT_FALSE(DescribeNonSvgBytes(bytes).has_value());
}

}  // namespace
}  // namespace donner::editor
