#include "donner/svg/parser/SVGParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/xml/XMLDocument.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/base/xml/XMLParser.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/svg/SVGAElement.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGTSpanElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/SVGTextPathElement.h"
#include "donner/svg/components/DescriptiveTextComponent.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/renderer/RendererUtils.h"

using testing::AllOf;
using testing::ElementsAre;

MATCHER_P3(ParseWarningIs, line, offset, errorMessageMatcher, "") {
  if (arg.range.start.lineInfo) {
    return testing::ExplainMatchResult(errorMessageMatcher, arg.reason, result_listener) &&
           arg.range.start.lineInfo->offsetOnLine == offset &&
           arg.range.start.lineInfo->line == line;
  }

  return testing::ExplainMatchResult(errorMessageMatcher, arg.reason, result_listener) &&
         line == 0 && arg.range.start.offset == offset;
}

// TODO: Add an ErrorHighlightsText matcher

namespace donner::svg::parser {

TEST(SVGParser, Simple) {
  const std::string_view simpleXml(
      R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg">
          </svg>)");

  ParseWarningSink warnings;
  EXPECT_THAT(SVGParser::ParseSVG(simpleXml, warnings), NoParseError());

  EXPECT_THAT(warnings.warnings(), ElementsAre());
}

TEST(SVGParser, Svgz) {
  // gzip-compressed "<svg xmlns='http://www.w3.org/2000/svg'></svg>"
  static const uint8_t kGzipData[] = {
      0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0xb3, 0x29, 0x2e,
      0x4b, 0x57, 0xa8, 0xc8, 0xcd, 0xc9, 0x2b, 0xb6, 0x55, 0xcf, 0x28, 0x29, 0x29,
      0xb0, 0xd2, 0xd7, 0x2f, 0x2f, 0x2f, 0xd7, 0x2b, 0x37, 0xd6, 0xcb, 0x2f, 0x4a,
      0xd7, 0x37, 0x32, 0x30, 0x30, 0xd0, 0x07, 0xaa, 0x50, 0xb7, 0xb3, 0x01, 0x51,
      0x76, 0x00, 0xf7, 0xa3, 0x84, 0x65, 0x2e, 0x00, 0x00, 0x00};
  const std::string_view gzipStr(reinterpret_cast<const char*>(kGzipData), sizeof(kGzipData));

  ParseWarningSink warnings;
  EXPECT_THAT(SVGParser::ParseSVG(gzipStr, warnings), NoParseError());

  EXPECT_THAT(warnings.warnings(), ElementsAre());
}

TEST(SVGParser, WithoutNamespace) {
  const std::string_view simpleXml("<svg></svg>");

  ParseWarningSink warnings;
  EXPECT_THAT(
      SVGParser::ParseSVG(simpleXml, warnings),
      ParseErrorIs("<svg> has an empty namespace URI. Expected 'http://www.w3.org/2000/svg'"));

  EXPECT_THAT(warnings.warnings(), ElementsAre());
}

TEST(SVGParser, WithoutNamespaceInline) {
  const std::string_view simpleXml("<svg></svg>");

  SVGParser::Options options;
  options.parseAsInlineSVG = true;

  ParseWarningSink warnings;
  EXPECT_THAT(SVGParser::ParseSVG(simpleXml, warnings, options), NoParseError());

  EXPECT_THAT(warnings.warnings(), ElementsAre());
}

TEST(SVGParser, Style) {
  const std::string_view simpleXml(
      R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg">
           <rect x="5" y="5" width="90" height="90" stroke="red" />
           <rect x="10" y="10" width="80" height="80" fill="green" />
         </svg>)");

  ParseWarningSink warnings;
  EXPECT_THAT(SVGParser::ParseSVG(simpleXml, warnings), NoParseError());

  EXPECT_THAT(warnings.warnings(), ElementsAre());
}

TEST(SVGParser, StyleConcatenatesTextAndCDataButRejectsElementChildren) {
  const std::string_view styleXml(
      R"(<svg xmlns="http://www.w3.org/2000/svg">
           <style>
             rect { fill:
             <![CDATA[red;]]>
             }
           </style>
         </svg>)");
  ParseWarningSink warnings;
  EXPECT_THAT(SVGParser::ParseSVG(styleXml, warnings), NoParseError());

  const std::string_view badStyleXml(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><style><rect/></style></svg>)");
  ParseWarningSink badWarnings;
  EXPECT_THAT(SVGParser::ParseSVG(badStyleXml, badWarnings),
              ParseErrorIs(testing::HasSubstr("Unexpected <style> element contents")));
}

TEST(SVGParser, TextContentElementsCollectTextAndChunkBoundaries) {
  const std::string_view textXml(
      R"(<svg xmlns="http://www.w3.org/2000/svg">
           <text>one<tspan>two<g/></tspan><a>three<g/></a>
             <textPath href="#path">four<g/></textPath>
           </text>
         </svg>)");

  ParseWarningSink warnings;
  EXPECT_THAT(SVGParser::ParseSVG(textXml, warnings), NoParseError());
}

TEST(SVGParser, UnsupportedNamespacesWarnAndContinueInsideSvg) {
  const std::string_view source(
      R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:other="http://example.test/other">
           <rect other:attr="value"/>
           <other:rect/>
         </svg>)");

  ParseWarningSink warnings;
  auto documentResult = SVGParser::ParseSVG(source, warnings);

  EXPECT_THAT(documentResult, NoParseError());
  EXPECT_THAT(warnings.warnings(),
              testing::Contains(testing::Field(&ParseDiagnostic::reason,
                                               testing::HasSubstr("unsupported namespace"))));
}

TEST(SVGParser, Attributes) {
  const std::string_view attributeXml =
      std::string_view(R"(<svg id="svg1" xmlns="http://www.w3.org/2000/svg">
           <rect stroke="red" user-attribute="value" />
         </svg>)");

  SVGParser::Options options;
  {
    options.disableUserAttributes = false;

    ParseWarningSink warnings;
    auto documentResult = SVGParser::ParseSVG(attributeXml, warnings, options);
    ASSERT_THAT(documentResult, NoParseError());

    EXPECT_THAT(warnings.warnings(), ElementsAre());

    auto maybeRect = documentResult.result().querySelector("rect");
    ASSERT_THAT(maybeRect, testing::Ne(std::nullopt));

    const SVGElement rect = maybeRect.value();

    EXPECT_THAT(rect.getAttribute("stroke"), testing::Optional(RcString("red")));
    EXPECT_THAT(rect.getAttribute("user-attribute"), testing::Optional(RcString("value")));
  }

  {
    options.disableUserAttributes = true;

    ParseWarningSink warnings;
    auto documentResult = SVGParser::ParseSVG(attributeXml, warnings, options);
    ASSERT_THAT(documentResult, NoParseError());

    EXPECT_THAT(warnings.warnings(),
                ElementsAre(ParseWarningIs(
                    2, 30, "Unknown attribute 'user-attribute' (disableUserAttributes: true)")));

    const SVGElement rect = documentResult.result().querySelector("rect").value();

    EXPECT_THAT(rect.getAttribute("stroke"), testing::Optional(RcString("red")));
    EXPECT_THAT(rect.getAttribute("user-attribute"), testing::Eq(std::nullopt));
  }
}

TEST(SVGParser, XmlParseErrors) {
  {
    const std::string_view badXml = std::string_view(R"(<!)");

    ParseWarningSink warnings;
    EXPECT_THAT(SVGParser::ParseSVG(badXml, warnings),
                AllOf(ParseErrorPos(1, 1), ParseErrorIs("Unrecognized node starting with '<!'")));
  }

  {
    const std::string_view badXml(
        R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg">
             <path></invalid>
           </svg>)");

    ParseWarningSink warnings;
    EXPECT_THAT(SVGParser::ParseSVG(badXml, warnings),
                AllOf(ParseErrorPos(2, 21), ParseErrorIs("Mismatched closing tag")));
  }
}

TEST(SVGParser, ParseXMLDocumentReportsMissingSvgRoot) {
  xml::XMLParser::Options options = xml::XMLParser::Options::ParseAll();
  auto xmlResult = xml::XMLParser::Parse("<!-- only comments -->", options);
  ASSERT_THAT(xmlResult, NoParseError());

  ParseWarningSink warnings;
  EXPECT_THAT(SVGParser::ParseXMLDocument(std::move(xmlResult.result()), warnings),
              ParseErrorIs("No SVG element found in document"));
}

TEST(SVGParser, Warning) {
  const std::string_view simpleXml(
      R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg">
           <path d="M 100 100 h 2!" />
         </svg>)");

  // TODO: Add another test to verify warnings from SVGParser and not during render-tree
  // instantiation.
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  auto documentResult = SVGParser::ParseSVG(simpleXml, disabled);
  ASSERT_THAT(documentResult, NoParseError());
  ParseWarningSink warnings;
  RendererUtils::prepareDocumentForRendering(documentResult.result(), /*verbose*/ false, warnings);
  // TODO: Map this offset back to absolute values (2, 24)
  EXPECT_THAT(warnings.warnings(),
              ElementsAre(ParseWarningIs(0, 13, "Failed to parse number: Unexpected character")));
}

TEST(SVGParser, InvalidXmlns) {
  const std::string_view invalidXml(R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="invalid">
         </svg>)");

  ParseWarningSink warnings;
  EXPECT_THAT(SVGParser::ParseSVG(invalidXml, warnings),
              ParseErrorIs("<svg> has an unexpected namespace URI 'invalid'. "
                           "Expected 'http://www.w3.org/2000/svg'"));

  EXPECT_THAT(warnings.warnings(), ElementsAre(AllOf(ParseErrorIs("Unexpected namespace 'invalid'"),
                                                     ParseErrorPos(1, 37))));
}

TEST(SVGParser, DoubleXmlNs) {
  const std::string_view invalidXml(
      R"(<svg id="svg1" xmlns="http://www.w3.org/2000/svg" xmlns:svg="http://www.w3.org/2000/svg">
            <rect id="rect" />
            <svg:rect svg:id="nsRect" />
         </svg>)");

  ParseWarningSink warnings;
  auto docResult = SVGParser::ParseSVG(invalidXml, warnings);
  ASSERT_THAT(docResult, NoParseError());
  EXPECT_THAT(warnings.warnings(), ElementsAre());

  // Get both <rect> elements and verify they are the right type.
  SVGDocument document = docResult.result();

  auto maybeFirstRect = document.svgElement().firstChild();
  ASSERT_THAT(maybeFirstRect, testing::Ne(std::nullopt));

  const SVGElement firstRect = maybeFirstRect.value();
  EXPECT_THAT(firstRect.tagName(), testing::Eq("rect"));
  EXPECT_EQ(firstRect.type(), ElementType::Rect);

  // Verify the attribute is set correctly.
  EXPECT_THAT(firstRect.getAttribute("id"), testing::Optional(RcString("rect")));

  auto maybeSecondRect = maybeFirstRect->nextSibling();
  ASSERT_THAT(maybeSecondRect, testing::Ne(std::nullopt));

  const SVGElement secondRect = maybeSecondRect.value();
  EXPECT_THAT(secondRect.tagName(), testing::Eq(xml::XMLQualifiedNameRef("svg", "rect")));
  EXPECT_EQ(secondRect.type(), ElementType::Rect);

  // Verify the attribute is set correctly.
  EXPECT_THAT(secondRect.getAttribute(xml::XMLQualifiedNameRef("svg", "id")),
              testing::Optional(RcString("nsRect")));
}

TEST(SVGParser, PrefixedXmlns) {
  const std::string_view xmlnsXml(
      R"(<svg:svg viewBox="0 0 200 200" xmlns:svg="http://www.w3.org/2000/svg">
           <svg:path d="M 100 100 h 2" />
         </svg:svg>)");

  ParseWarningSink warnings;
  EXPECT_THAT(SVGParser::ParseSVG(xmlnsXml, warnings), NoParseError());

  EXPECT_THAT(warnings.warnings(), ElementsAre());
}

TEST(SVGParser, PrefixedXmlnsWithAttributes) {
  const std::string_view xmlnsXml(
      R"(<svg:svg viewBox="0 0 200 200" xmlns:svg="http://www.w3.org/2000/svg">
           <svg:path svg:d="M 100 100 h 2" />
         </svg:svg>)");

  ParseWarningSink warnings;
  EXPECT_THAT(SVGParser::ParseSVG(xmlnsXml, warnings), NoParseError());

  EXPECT_THAT(warnings.warnings(), ElementsAre());
}

TEST(SVGParser, MismatchedNamespace) {
  {
    const std::string_view mismatchedSvgXmlnsXml(
        R"(<svg viewBox="0 0 200 200" xmlns:svg="http://www.w3.org/2000/svg">
             <svg:path d="M 100 100 h 2" />
           </svg>)");

    ParseWarningSink warnings;
    EXPECT_THAT(
        SVGParser::ParseSVG(mismatchedSvgXmlnsXml, warnings),
        AllOf(ParseErrorPos(1, 0), ParseErrorIs("<svg> has an empty namespace "
                                                "URI. Expected 'http://www.w3.org/2000/svg'")));
  }

  {
    const std::string_view mismatchedXmlnsXml(
        R"(<svg:svg viewBox="0 0 200 200" xmlns:svg="http://www.w3.org/2000/svg">
             <path d="M 100 100 h 2" />
           </svg:svg>)");

    ParseWarningSink warnings;
    EXPECT_THAT(SVGParser::ParseSVG(mismatchedXmlnsXml, warnings), NoParseError());

    EXPECT_THAT(warnings.warnings(),
                ElementsAre(AllOf(ParseErrorPos(2, 13),
                                  ParseErrorIs("Ignored element <path> with an unsupported "
                                               "namespace. Expected 'svg', found ''"))));
  }

  {
    const std::string_view invalidAttributeNsXml(
        R"(<svg:svg viewBox="0 0 200 200" xmlns:svg="http://www.w3.org/2000/svg">
             <svg:path invalid:d="M 100 100 h 2" />
           </svg:svg>)");

    ParseWarningSink warnings;
    EXPECT_THAT(SVGParser::ParseSVG(invalidAttributeNsXml, warnings), NoParseError());

    EXPECT_THAT(warnings.warnings(),
                ElementsAre(AllOf(
                    ParseErrorPos(2, 23),
                    ParseErrorIs("Ignored attribute 'invalid:d' with an unsupported namespace"))));
  }
}

TEST(SVGParser, RootElementNotSvgFails) {
  ParseWarningSink warnings;
  EXPECT_THAT(SVGParser::ParseSVG(R"(<foo xmlns="http://www.w3.org/2000/svg"/>)", warnings),
              ParseErrorIs("Unexpected element <foo> at root, first element must be <svg>"));
}

TEST(SVGParser, UnknownElementInSvgNamespace) {
  ParseWarningSink warnings;
  auto result = SVGParser::ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><notAnElement id="u"/></svg>)", warnings);
  ASSERT_THAT(result, NoParseError());

  auto unknown = result.result().querySelector("#u");
  ASSERT_TRUE(unknown.has_value());
  EXPECT_EQ(unknown->type(), ElementType::Unknown);
  EXPECT_THAT(unknown->tagName(), testing::Eq("notAnElement"));
}

TEST(SVGParser, ExperimentalElementsRequireOptIn) {
  const std::string_view source(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><animate id="a" attributeName="x"/></svg>)");

  {
    // Without the experimental option, <animate> parses as an unknown element.
    ParseWarningSink warnings;
    auto result = SVGParser::ParseSVG(source, warnings);
    ASSERT_THAT(result, NoParseError());

    auto element = result.result().querySelector("#a");
    ASSERT_TRUE(element.has_value());
    EXPECT_EQ(element->type(), ElementType::Unknown);
  }

  {
    SVGParser::Options options;
    options.enableExperimental = true;

    ParseWarningSink warnings;
    auto result = SVGParser::ParseSVG(source, warnings, options);
    ASSERT_THAT(result, NoParseError());

    auto element = result.result().querySelector("#a");
    ASSERT_TRUE(element.has_value());
    EXPECT_EQ(element->type(), ElementType::Animate);
  }
}

TEST(SVGParser, NestedStyleElementContentsErrorPropagates) {
  ParseWarningSink warnings;
  EXPECT_THAT(SVGParser::ParseSVG(
                  R"(<svg xmlns="http://www.w3.org/2000/svg"><g><style><rect/></style></g></svg>)",
                  warnings),
              ParseErrorIs(testing::HasSubstr("Unexpected <style> element contents")));
}

TEST(SVGParser, GzipErrors) {
  {
    // Gzip magic bytes with no payload.
    const std::string_view truncated("\x1f\x8b", 2);
    ParseWarningSink warnings;
    EXPECT_THAT(SVGParser::ParseSVG(truncated, warnings),
                ParseErrorIs(testing::HasSubstr("Failed to decompress gzip data")));
  }

  {
    // Valid magic bytes followed by a corrupt stream.
    std::string corrupt("\x1f\x8b", 2);
    corrupt += std::string(32, 'x');

    ParseWarningSink warnings;
    EXPECT_THAT(SVGParser::ParseSVG(corrupt, warnings),
                ParseErrorIs(testing::HasSubstr("gzip")));
  }
}

TEST(SVGParser, InlineSvgWithExplicitInvalidNamespaceFails) {
  SVGParser::Options options;
  options.parseAsInlineSVG = true;

  ParseWarningSink warnings;
  EXPECT_THAT(SVGParser::ParseSVG(R"(<svg xmlns="invalid"></svg>)", warnings, options),
              ParseErrorIs("<svg> has an unexpected namespace URI 'invalid'. "
                           "Expected 'http://www.w3.org/2000/svg'"));
}

TEST(SVGParser, PrefixedXmlnsDeclaredBeforeDefault) {
  const std::string_view source(
      R"(<svg xmlns:s="http://www.w3.org/2000/svg" xmlns="http://www.w3.org/2000/svg">
           <rect id="plain"/>
           <s:rect s:id="prefixed"/>
         </svg>)");

  ParseWarningSink warnings;
  auto result = SVGParser::ParseSVG(source, warnings);
  ASSERT_THAT(result, NoParseError());
  EXPECT_THAT(warnings.warnings(), ElementsAre());

  SVGDocument document = result.result();
  auto plain = document.querySelector("#plain");
  ASSERT_TRUE(plain.has_value());
  EXPECT_EQ(plain->type(), ElementType::Rect);

  auto prefixed = document.svgElement().lastChild();
  ASSERT_TRUE(prefixed.has_value());
  EXPECT_EQ(prefixed->type(), ElementType::Rect);
  EXPECT_THAT(prefixed->tagName(), testing::Eq(xml::XMLQualifiedNameRef("s", "rect")));
}

TEST(SVGParser, ParseXMLDocumentSuccessAndRootError) {
  xml::XMLParser::Options xmlOptions = xml::XMLParser::Options::ParseAll();

  {
    auto xmlResult = xml::XMLParser::Parse(
        R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r"/></svg>)", xmlOptions);
    ASSERT_THAT(xmlResult, NoParseError());

    ParseWarningSink warnings;
    auto result = SVGParser::ParseXMLDocument(std::move(xmlResult.result()), warnings);
    ASSERT_THAT(result, NoParseError());
    EXPECT_TRUE(result.result().querySelector("#r").has_value());
  }

  {
    auto xmlResult = xml::XMLParser::Parse("<foo/>", xmlOptions);
    ASSERT_THAT(xmlResult, NoParseError());

    ParseWarningSink warnings;
    EXPECT_THAT(SVGParser::ParseXMLDocument(std::move(xmlResult.result()), warnings),
                ParseErrorIs("Unexpected element <foo> at root, first element must be <svg>"));
  }
}

TEST(SVGParser, ProgrammaticDocumentStyleWithoutSourceLocations) {
  // Build an XML document via the DOM API. Its nodes have no source offsets, exercising the
  // <style> source-map fallback paths.
  xml::XMLDocument xmlDocument;

  xml::XMLNode svg = xml::XMLNode::CreateElementNode(xmlDocument, "svg");
  svg.setAttribute("xmlns", "http://www.w3.org/2000/svg");
  xmlDocument.root().appendChild(svg);

  xml::XMLNode style = xml::XMLNode::CreateElementNode(xmlDocument, "style");
  svg.appendChild(style);
  style.appendChild(xml::XMLNode::CreateDataNode(xmlDocument, "rect { fill: red; }"));

  ParseWarningSink warnings;
  auto result = SVGParser::ParseXMLDocument(std::move(xmlDocument), warnings);
  ASSERT_THAT(result, NoParseError());

  auto styleElement = result.result().querySelector("style");
  ASSERT_TRUE(styleElement.has_value());
  const auto& stylesheet =
      styleElement->entityHandle().get<components::StylesheetComponent>();
  EXPECT_EQ(stylesheet.text, "rect { fill: red; }");
  EXPECT_FALSE(stylesheet.stylesheet.rules().empty());
}

TEST(SVGParser, DescriptiveElementsCaptureTextContent) {
  const std::string_view source(
      R"(<svg xmlns="http://www.w3.org/2000/svg">
           <title id="t">Hello<!-- ignored --><![CDATA[ World]]></title>
         </svg>)");

  ParseWarningSink warnings;
  auto result = SVGParser::ParseSVG(source, warnings);
  ASSERT_THAT(result, NoParseError());

  auto title = result.result().querySelector("#t");
  ASSERT_TRUE(title.has_value());
  const auto& descriptiveText =
      title->entityHandle().get<components::DescriptiveTextComponent>();
  EXPECT_EQ(descriptiveText.text, "Hello World");
}

TEST(SVGParser, TextContentSkipsCommentsAndCollectsCData) {
  const std::string_view source(
      R"(<svg xmlns="http://www.w3.org/2000/svg">
           <text id="text">one<!-- c --><![CDATA[two]]><tspan id="span"><![CDATA[three]]><!-- c --></tspan><a id="link"><![CDATA[four]]><!-- c --></a><textPath id="tp"><![CDATA[five]]><!-- c --></textPath></text>
         </svg>)");

  ParseWarningSink warnings;
  auto result = SVGParser::ParseSVG(source, warnings);
  ASSERT_THAT(result, NoParseError());

  SVGDocument document = result.result();
  EXPECT_EQ(document.querySelector("#text")->cast<SVGTextElement>().textContent(), "onetwo");
  EXPECT_EQ(document.querySelector("#span")->cast<SVGTSpanElement>().textContent(), "three");
  EXPECT_EQ(document.querySelector("#link")->cast<SVGAElement>().textContent(), "four");
  EXPECT_EQ(document.querySelector("#tp")->cast<SVGTextPathElement>().textContent(), "five");
}

TEST(SVGParser, SubDocumentParseErrorsReportedAsWarnings) {
  // data URI decodes to "<notsvg/>", which fails SVG parsing inside the sub-document callback.
  const std::string_view source(
      R"(<svg xmlns="http://www.w3.org/2000/svg">
           <image href="data:image/svg+xml;base64,PG5vdHN2Zy8+" width="10" height="10"/>
         </svg>)");

  ParseWarningSink parseWarnings;
  auto result = SVGParser::ParseSVG(source, parseWarnings);
  ASSERT_THAT(result, NoParseError());

  ParseWarningSink renderWarnings;
  SVGDocument document = result.result();
  RendererUtils::prepareDocumentForRendering(document, /*verbose*/ false, renderWarnings);

  EXPECT_THAT(renderWarnings.warnings(),
              testing::Contains(
                  testing::Field(&ParseDiagnostic::reason,
                                 testing::HasSubstr("Unexpected element <notsvg> at root"))));
}

TEST(SVGParser, SecureStaticModeDisablesSubDocumentParsing) {
  const std::string_view source(
      R"(<svg xmlns="http://www.w3.org/2000/svg">
           <image href="data:image/svg+xml;base64,PG5vdHN2Zy8+" width="10" height="10"/>
         </svg>)");

  SVGDocument::Settings settings;
  settings.processingMode = ProcessingMode::SecureStatic;

  ParseWarningSink parseWarnings;
  auto result =
      SVGParser::ParseSVG(source, parseWarnings, SVGParser::Options(), std::move(settings));
  ASSERT_THAT(result, NoParseError());

  ParseWarningSink renderWarnings;
  SVGDocument document = result.result();
  RendererUtils::prepareDocumentForRendering(document, /*verbose*/ false, renderWarnings);

  // In SecureStatic mode resource loading is skipped entirely (SVG2 section 2.7.1): the
  // sub-document is not parsed and no warnings are produced.
  EXPECT_THAT(renderWarnings.warnings(), ElementsAre());
}

}  // namespace donner::svg::parser
