#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/svg/parser/tests/parse_result_test_utils.h"
#include "src/svg/xml/xml_parser.h"

using testing::AllOf;
using testing::ElementsAre;

MATCHER_P3(ParseWarningIs, line, offset, errorMessageMatcher, "") {
  return testing::ExplainMatchResult(errorMessageMatcher, arg.reason, result_listener) &&
         arg.line == line && arg.offset == offset;
}

namespace donner {

namespace {
static std::span<char> spanFromString(std::string& data) {
  return std::span<char>(data.data(), data.size());
}
}  // namespace

TEST(XmlParser, Simple) {
  std::string simpleXml =
      R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg">
</svg>)";

  std::vector<ParseError> warnings;
  EXPECT_THAT(XMLParser::ParseSVG(spanFromString(simpleXml), &warnings), NoParseError());

  EXPECT_THAT(warnings, ElementsAre());
}

TEST(XmlParser, XmlParseErrors) {
  {
    std::string badXml = R"(<!)";

    std::vector<ParseError> warnings;
    EXPECT_THAT(XMLParser::ParseSVG(spanFromString(badXml), &warnings),
                AllOf(ParseErrorPos(1, 2), ParseErrorIs("unexpected end of data")));
  }

  {
    std::string badXml = R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg">
  <path></invalid>
</svg>)";

    std::vector<ParseError> warnings;
    EXPECT_THAT(XMLParser::ParseSVG(spanFromString(badXml), &warnings),
                AllOf(ParseErrorPos(2, 17), ParseErrorIs("invalid closing tag name")));
  }
}

TEST(XmlParser, Warning) {
  std::string simpleXml =
      R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg">
  <path d="M 100 100 h 2!" />
</svg>)";

  std::vector<ParseError> warnings;
  EXPECT_THAT(XMLParser::ParseSVG(spanFromString(simpleXml), &warnings), NoParseError());
  EXPECT_THAT(warnings,
              ElementsAre(ParseWarningIs(2, 24, "Failed to parse number: Invalid argument")));
}

}  // namespace donner
