#include "donner/svg/xml/XMLParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/parser/tests/ParseResultTestUtils.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/renderer/RendererUtils.h"

using testing::AllOf;
using testing::ElementsAre;

MATCHER_P3(ParseWarningIs, line, offset, errorMessageMatcher, "") {
  return testing::ExplainMatchResult(errorMessageMatcher, arg.reason, result_listener) &&
         arg.line == line && arg.offset == offset;
}

namespace donner::svg {

TEST(XmlParser, Simple) {
  std::vector<ParseError> warnings;
  EXPECT_THAT(XMLParser::ParseSVG(
                  R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg">
                     </svg>)",
                  &warnings),
              NoParseError());

  EXPECT_THAT(warnings, ElementsAre());
}

TEST(XmlParser, Style) {
  XMLParser::InputBuffer simpleXml =
      R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg">
           <rect x="5" y="5" width="90" height="90" stroke="red" />
           <rect x="10" y="10" width="80" height="80" fill="green" />
         </svg>)";

  std::vector<ParseError> warnings;
  EXPECT_THAT(XMLParser::ParseSVG(simpleXml, &warnings), NoParseError());

  EXPECT_THAT(warnings, ElementsAre());
}

TEST(XmlParser, Attributes) {
  const XMLParser::InputBuffer kAttributeXml =
      R"(<svg id="svg1" xmlns="http://www.w3.org/2000/svg">
           <rect stroke="red" user-attribute="value" />
         </svg>)";

  XMLParser::Options options;
  {
    options.disableUserAttributes = false;

    // Copy attributeXml before parsing since it will be modified.
    XMLParser::InputBuffer attributeXml = kAttributeXml;

    std::vector<ParseError> warnings;
    auto documentResult = XMLParser::ParseSVG(attributeXml, &warnings, options);
    ASSERT_THAT(documentResult, NoParseError());

    EXPECT_THAT(warnings, ElementsAre());

    const SVGElement rect = documentResult.result().querySelector("rect").value();

    EXPECT_THAT(rect.getAttribute("stroke"), testing::Optional(RcString("red")));
    EXPECT_THAT(rect.getAttribute("user-attribute"), testing::Optional(RcString("value")));
  }

  {
    options.disableUserAttributes = true;

    // Copy attributeXml before parsing since it will be modified.
    XMLParser::InputBuffer attributeXml = kAttributeXml;

    std::vector<ParseError> warnings;
    auto documentResult = XMLParser::ParseSVG(attributeXml, &warnings, options);
    ASSERT_THAT(documentResult, NoParseError());

    EXPECT_THAT(warnings,
                ElementsAre(ParseWarningIs(
                    2, 37, "Unknown attribute 'user-attribute' (disableUserAttributes: true)")));

    const SVGElement rect = documentResult.result().querySelector("rect").value();

    EXPECT_THAT(rect.getAttribute("stroke"), testing::Optional(RcString("red")));
    EXPECT_THAT(rect.getAttribute("user-attribute"), testing::Eq(std::nullopt));
  }
}

TEST(XmlParser, XmlParseErrors) {
  {
    XMLParser::InputBuffer badXml = R"(<!)";

    std::vector<ParseError> warnings;
    EXPECT_THAT(XMLParser::ParseSVG(badXml, &warnings),
                AllOf(ParseErrorPos(1, 2), ParseErrorIs("unexpected end of data")));
  }

  {
    XMLParser::InputBuffer badXml =
        R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg">
             <path></invalid>
           </svg>)";

    std::vector<ParseError> warnings;
    EXPECT_THAT(XMLParser::ParseSVG(badXml, &warnings),
                AllOf(ParseErrorPos(2, 17), ParseErrorIs("invalid closing tag name")));
  }
}

TEST(XmlParser, Warning) {
  XMLParser::InputBuffer simpleXml =
      R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg">
           <path d="M 100 100 h 2!" />
         </svg>)";

  // TODO: Add another test to verify warnings from XMLParser and not during render-tree
  // instantiation.
  std::vector<ParseError> warnings;
  auto documentResult = XMLParser::ParseSVG(simpleXml);
  ASSERT_THAT(documentResult, NoParseError());
  RendererUtils::prepareDocumentForRendering(documentResult.result(), /*verbose*/ false, &warnings);
  // TODO: Map this offset back to absolute values (2, 24)
  EXPECT_THAT(warnings,
              ElementsAre(ParseWarningIs(0, 13, "Failed to parse number: Unexpected character")));
}

TEST(XmlParser, InvalidXmlns) {
  XMLParser::InputBuffer simpleXml =
      R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="invalid">
         </svg>)";

  std::vector<ParseError> warnings;
  EXPECT_THAT(XMLParser::ParseSVG(simpleXml, &warnings), NoParseError());

  EXPECT_THAT(warnings, ElementsAre(ParseErrorIs("Unexpected namespace 'invalid'")));
}

TEST(XmlParser, PrefixedXmlns) {
  XMLParser::InputBuffer xmlnsXml =
      R"(<svg:svg viewBox="0 0 200 200" xmlns:svg="http://www.w3.org/2000/svg">
           <svg:path d="M 100 100 h 2" />
         </svg:svg>)";

  std::vector<ParseError> warnings;
  EXPECT_THAT(XMLParser::ParseSVG(xmlnsXml, &warnings), NoParseError());

  EXPECT_THAT(warnings, ElementsAre());
}

TEST(XmlParser, MismatchedNamespace) {
  {
    XMLParser::InputBuffer mismatchedSvgXmlnsXml =
        R"(<svg viewBox="0 0 200 200" xmlns:svg="http://www.w3.org/2000/svg">
             <svg:path d="M 100 100 h 2" />
           </svg>)";

    std::vector<ParseError> warnings;
    EXPECT_THAT(
        XMLParser::ParseSVG(mismatchedSvgXmlnsXml, &warnings),
        AllOf(ParseErrorPos(1, 1),
              ParseErrorIs("<svg> has a mismatched namespace prefix. Expected 'svg', found ''")));
  }

  {
    XMLParser::InputBuffer mismatchedXmlnsXml =
        R"(<svg:svg viewBox="0 0 200 200" xmlns:svg="http://www.w3.org/2000/svg">
             <path d="M 100 100 h 2" />
           </svg:svg>)";

    std::vector<ParseError> warnings;
    EXPECT_THAT(XMLParser::ParseSVG(mismatchedXmlnsXml, &warnings), NoParseError());

    EXPECT_THAT(
        warnings,
        ElementsAre(AllOf(ParseErrorPos(2, 3),
                          ParseErrorIs("Ignored element <path> with an unsupported namespace"))));
  }

  {
    XMLParser::InputBuffer invalidNsXml =
        R"(<svg:svg viewBox="0 0 200 200" xmlns:svg="http://www.w3.org/2000/svg">
             <other:path d="M 100 100 h 2" />
           </svg:svg>)";

    std::vector<ParseError> warnings;
    EXPECT_THAT(XMLParser::ParseSVG(invalidNsXml, &warnings),
                AllOf(ParseErrorPos(2, 3), ParseErrorIs("No namespace definition found")));
  }

  {
    XMLParser::InputBuffer invalidAttributeNsXml =
        R"(<svg:svg viewBox="0 0 200 200" xmlns:svg="http://www.w3.org/2000/svg">
             <svg:path svg:d="M 100 100 h 2" />
           </svg:svg>)";

    std::vector<ParseError> warnings;
    EXPECT_THAT(XMLParser::ParseSVG(invalidAttributeNsXml, &warnings), NoParseError());

    EXPECT_THAT(warnings,
                ElementsAre(AllOf(
                    ParseErrorPos(2, 12),
                    ParseErrorIs("Ignored attribute 'svg:d' with an unsupported namespace"))));
  }
}

}  // namespace donner::svg
