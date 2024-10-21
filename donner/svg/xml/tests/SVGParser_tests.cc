#include "donner/svg/xml/SVGParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/parser/tests/ParseResultTestUtils.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/renderer/RendererUtils.h"

using namespace donner::base::parser;
using testing::AllOf;
using testing::ElementsAre;

MATCHER_P3(ParseWarningIs, line, offset, errorMessageMatcher, "") {
  if (arg.location.lineInfo) {
    return testing::ExplainMatchResult(errorMessageMatcher, arg.reason, result_listener) &&
           arg.location.lineInfo->offsetOnLine == offset && arg.location.lineInfo->line == line;
  }

  return testing::ExplainMatchResult(errorMessageMatcher, arg.reason, result_listener) &&
         line == 0 && arg.location.offset == offset;
}

// TODO: Add an ErrorHighlightsText matcher

namespace donner::svg::parser {

TEST(SVGParser, Simple) {
  SVGParser::InputBuffer simpleXml =
      std::string_view(R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg">
          </svg>)");

  std::vector<ParseError> warnings;
  EXPECT_THAT(SVGParser::ParseSVG(simpleXml, &warnings), NoParseError());

  EXPECT_THAT(warnings, ElementsAre());
}

TEST(SVGParser, Style) {
  SVGParser::InputBuffer simpleXml =
      std::string_view(R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg">
           <rect x="5" y="5" width="90" height="90" stroke="red" />
           <rect x="10" y="10" width="80" height="80" fill="green" />
         </svg>)");

  std::vector<ParseError> warnings;
  EXPECT_THAT(SVGParser::ParseSVG(simpleXml, &warnings), NoParseError());

  EXPECT_THAT(warnings, ElementsAre());
}

TEST(SVGParser, Attributes) {
  const SVGParser::InputBuffer kAttributeXml =
      std::string_view(R"(<svg id="svg1" xmlns="http://www.w3.org/2000/svg">
           <rect stroke="red" user-attribute="value" />
         </svg>)");

  SVGParser::Options options;
  {
    options.disableUserAttributes = false;

    // Copy attributeXml before parsing since it will be modified.
    SVGParser::InputBuffer attributeXml = kAttributeXml;

    std::vector<ParseError> warnings;
    auto documentResult = SVGParser::ParseSVG(attributeXml, &warnings, options);
    ASSERT_THAT(documentResult, NoParseError());

    EXPECT_THAT(warnings, ElementsAre());

    const SVGElement rect = documentResult.result().querySelector("rect").value();

    EXPECT_THAT(rect.getAttribute("stroke"), testing::Optional(RcString("red")));
    EXPECT_THAT(rect.getAttribute("user-attribute"), testing::Optional(RcString("value")));
  }

  {
    options.disableUserAttributes = true;

    // Copy attributeXml before parsing since it will be modified.
    SVGParser::InputBuffer attributeXml = kAttributeXml;

    std::vector<ParseError> warnings;
    auto documentResult = SVGParser::ParseSVG(attributeXml, &warnings, options);
    ASSERT_THAT(documentResult, NoParseError());

    EXPECT_THAT(warnings,
                ElementsAre(ParseWarningIs(
                    2, 46, "Unknown attribute 'user-attribute' (disableUserAttributes: true)")));

    const SVGElement rect = documentResult.result().querySelector("rect").value();

    EXPECT_THAT(rect.getAttribute("stroke"), testing::Optional(RcString("red")));
    EXPECT_THAT(rect.getAttribute("user-attribute"), testing::Eq(std::nullopt));
  }
}

TEST(SVGParser, XmlParseErrors) {
  {
    SVGParser::InputBuffer badXml = std::string_view(R"(<!)");

    std::vector<ParseError> warnings;
    EXPECT_THAT(SVGParser::ParseSVG(badXml, &warnings),
                AllOf(ParseErrorPos(1, 2), ParseErrorIs("unexpected end of data")));
  }

  {
    SVGParser::InputBuffer badXml =
        std::string_view(R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg">
             <path></invalid>
           </svg>)");

    std::vector<ParseError> warnings;
    EXPECT_THAT(SVGParser::ParseSVG(badXml, &warnings),
                AllOf(ParseErrorPos(2, 28), ParseErrorIs("invalid closing tag name")));
  }
}

TEST(SVGParser, Warning) {
  SVGParser::InputBuffer simpleXml =
      std::string_view(R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg">
           <path d="M 100 100 h 2!" />
         </svg>)");

  // TODO: Add another test to verify warnings from SVGParser and not during render-tree
  // instantiation.
  std::vector<ParseError> warnings;
  auto documentResult = SVGParser::ParseSVG(simpleXml);
  ASSERT_THAT(documentResult, NoParseError());
  RendererUtils::prepareDocumentForRendering(documentResult.result(), /*verbose*/ false, &warnings);
  // TODO: Map this offset back to absolute values (2, 24)
  EXPECT_THAT(warnings,
              ElementsAre(ParseWarningIs(0, 13, "Failed to parse number: Unexpected character")));
}

TEST(SVGParser, InvalidXmlns) {
  SVGParser::InputBuffer simpleXml =
      std::string_view(R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="invalid">
         </svg>)");

  std::vector<ParseError> warnings;
  EXPECT_THAT(SVGParser::ParseSVG(simpleXml, &warnings), NoParseError());

  EXPECT_THAT(warnings, ElementsAre(ParseErrorIs("Unexpected namespace 'invalid'")));
}

TEST(SVGParser, PrefixedXmlns) {
  SVGParser::InputBuffer xmlnsXml =
      std::string_view(R"(<svg:svg viewBox="0 0 200 200" xmlns:svg="http://www.w3.org/2000/svg">
           <svg:path d="M 100 100 h 2" />
         </svg:svg>)");

  std::vector<ParseError> warnings;
  EXPECT_THAT(SVGParser::ParseSVG(xmlnsXml, &warnings), NoParseError());

  EXPECT_THAT(warnings, ElementsAre());
}

TEST(SVGParser, MismatchedNamespace) {
  {
    SVGParser::InputBuffer mismatchedSvgXmlnsXml =
        std::string_view(R"(<svg viewBox="0 0 200 200" xmlns:svg="http://www.w3.org/2000/svg">
             <svg:path d="M 100 100 h 2" />
           </svg>)");

    std::vector<ParseError> warnings;
    EXPECT_THAT(
        SVGParser::ParseSVG(mismatchedSvgXmlnsXml, &warnings),
        AllOf(ParseErrorPos(1, 1),
              ParseErrorIs("<svg> has a mismatched namespace prefix. Expected 'svg', found ''")));
  }

  {
    SVGParser::InputBuffer mismatchedXmlnsXml =
        std::string_view(R"(<svg:svg viewBox="0 0 200 200" xmlns:svg="http://www.w3.org/2000/svg">
             <path d="M 100 100 h 2" />
           </svg:svg>)");

    std::vector<ParseError> warnings;
    EXPECT_THAT(SVGParser::ParseSVG(mismatchedXmlnsXml, &warnings), NoParseError());

    EXPECT_THAT(
        warnings,
        ElementsAre(AllOf(ParseErrorPos(2, 14),
                          ParseErrorIs("Ignored element <path> with an unsupported namespace"))));
  }

  {
    SVGParser::InputBuffer invalidNsXml =
        std::string_view(R"(<svg:svg viewBox="0 0 200 200" xmlns:svg="http://www.w3.org/2000/svg">
             <other:path d="M 100 100 h 2" />
           </svg:svg>)");

    std::vector<ParseError> warnings;
    EXPECT_THAT(SVGParser::ParseSVG(invalidNsXml, &warnings),
                AllOf(ParseErrorPos(2, 14), ParseErrorIs("No namespace definition found")));
  }

  {
    SVGParser::InputBuffer invalidAttributeNsXml =
        std::string_view(R"(<svg:svg viewBox="0 0 200 200" xmlns:svg="http://www.w3.org/2000/svg">
             <svg:path svg:d="M 100 100 h 2" />
           </svg:svg>)");

    std::vector<ParseError> warnings;
    EXPECT_THAT(SVGParser::ParseSVG(invalidAttributeNsXml, &warnings), NoParseError());

    EXPECT_THAT(warnings,
                ElementsAre(AllOf(
                    ParseErrorPos(2, 23),
                    ParseErrorIs("Ignored attribute 'svg:d' with an unsupported namespace"))));
  }
}

}  // namespace donner::svg::parser
