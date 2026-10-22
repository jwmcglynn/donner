#include "donner/svg/parser/SVGParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <string>
#include <utility>

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
#include "donner/svg/components/ParsedPayloadResourceBudget.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
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

TEST(SVGParser, RawInputMetadataAcceptanceRequiresValidUtf8XmlCharacters) {
  const std::array<std::pair<std::string, bool>, 10> values{{{"ASCII", true},
                                                             {"\xC3\xA9\xF0\x9F\x99\x82", true},
                                                             {"\x80", false},
                                                             {"\xC0\xAF", false},
                                                             {"\xE2\x82", false},
                                                             {"\xED\xA0\x80", false},
                                                             {"\xF4\x90\x80\x80", false},
                                                             {"\x0B", false},
                                                             {"\xEF\xBF\xBE", false},
                                                             {std::string(1, '\0'), false}}};
  for (size_t index = 0; index < values.size(); ++index) {
    SCOPED_TRACE(index);
    const auto& [value, accepted] = values[index];
    ParseWarningSink warnings;
    const auto result = SVGParser::ParseSVG(
        "<svg xmlns='http://www.w3.org/2000/svg'><metadata>" + value + "</metadata></svg>",
        warnings);
    EXPECT_EQ(result.hasResult(), accepted);
    EXPECT_EQ(result.hasError(), !accepted);
  }
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

TEST(SVGParser, RejectsInputLargerThanConfiguredLimit) {
  SVGParser::Options options;
  options.maximumInputSize = 64;

  ParseWarningSink warnings;
  EXPECT_THAT(SVGParser::ParseSVG(std::string(65, ' '), warnings, options),
              ParseErrorIs(testing::HasSubstr("maximum input size")));
}

TEST(SVGParser, RejectsSvgzWhoseExpandedSizeExceedsConfiguredLimit) {
  // 128 repeated 'A' bytes compressed with gzip -n. The compressed input is 24 bytes.
  static const uint8_t kGzipData[] = {
      0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x73, 0x74,
      0x1c, 0x58, 0x00, 0x00, 0xde, 0x8a, 0x18, 0x04, 0x80, 0x00, 0x00, 0x00,
  };
  SVGParser::Options options;
  options.maximumInputSize = 64;

  ParseWarningSink warnings;
  const std::string_view gzipStr(reinterpret_cast<const char*>(kGzipData), sizeof(kGzipData));
  EXPECT_THAT(SVGParser::ParseSVG(gzipStr, warnings, options),
              ParseErrorIs(testing::HasSubstr("maximum decompressed size")));
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

TEST(SVGParser, CssImportWarningUsesDocumentSourceOffset) {
  const std::string source =
      "<svg xmlns='http://www.w3.org/2000/svg'><style>\n "
      "@import 'hidden.css'; rect { fill: green; }</style><rect/></svg>";
  ParseWarningSink warnings;
  EXPECT_THAT(SVGParser::ParseSVG(source, warnings), NoParseError());
  ASSERT_THAT(warnings.warnings(), testing::SizeIs(1));
  EXPECT_THAT(warnings.warnings()[0].severity, testing::Eq(DiagnosticSeverity::Warning));
  EXPECT_THAT(warnings.warnings()[0].reason,
              testing::Eq("CSS @import is unsupported; imported stylesheets are not loaded"));
  EXPECT_THAT(warnings.warnings()[0].range.start.offset, testing::Eq(source.find("@import")));
  EXPECT_THAT(
      warnings.warnings()[0],
      ParseWarningIs(
          2, 1, testing::Eq("CSS @import is unsupported; imported stylesheets are not loaded")));
}

TEST(SVGParser, CssImportWarningsMapTextCdataAndSplitKeywords) {
  const std::array<std::string_view, 4> bodies = {
      "  @import 'hidden.css'; rect { fill: green; }",
      "<![CDATA[@import 'hidden.css'; rect { fill: green; }]]>",
      " \n <![CDATA[@import 'hidden.css'; rect { fill: green; }]]>",
      "  @<![CDATA[import 'hidden.css'; rect { fill: green; }]]>"};
  for (const std::string_view body : bodies) {
    SCOPED_TRACE(body);
    const std::string source = "<svg xmlns='http://www.w3.org/2000/svg'><style>" +
                               std::string(body) + "</style><rect/></svg>";
    ParseWarningSink warnings;
    EXPECT_THAT(SVGParser::ParseSVG(source, warnings), NoParseError());
    ASSERT_THAT(warnings.warnings(), testing::SizeIs(1));
    EXPECT_THAT(warnings.warnings().front().range.start.offset, testing::Eq(source.find('@')));
    EXPECT_THAT(warnings.warnings().front().range.end.offset, testing::Eq(source.find('@')));
  }
}

TEST(SVGParser, CssImportSourceOffsetCountsUtf8Bytes) {
  const std::string source =
      "<svg xmlns='http://www.w3.org/2000/svg'><style>/* \xc3\xa9 */ @import 'hidden.css';"
      "rect { fill: green; }</style><rect/></svg>";
  ParseWarningSink warnings;
  EXPECT_THAT(SVGParser::ParseSVG(source, warnings), NoParseError());
  ASSERT_THAT(warnings.warnings(), testing::SizeIs(1));
  EXPECT_THAT(warnings.warnings().front().range.start.offset, testing::Eq(source.find("@import")));
  EXPECT_THAT(
      warnings.warnings().front(),
      ParseWarningIs(1, source.find("@import"), testing::HasSubstr("@import is unsupported")));
}

TEST(SVGParser, SourceLessCssImportWarningKeepsCssLocalOffset) {
  xml::XMLDocument xmlDocument;
  auto svg = xml::XMLNode::CreateElementNode(xmlDocument, "svg");
  svg.setAttribute("xmlns", "http://www.w3.org/2000/svg");
  xmlDocument.root().appendChild(svg);
  auto style = xml::XMLNode::CreateElementNode(xmlDocument, "style");
  svg.appendChild(style);
  auto data = xml::XMLNode::CreateDataNode(xmlDocument, "  @import 'hidden.css';");
  style.appendChild(data);
  ParseWarningSink warnings;
  EXPECT_THAT(SVGParser::ParseXMLDocument(std::move(xmlDocument), warnings), NoParseError());
  ASSERT_THAT(warnings.warnings(), testing::SizeIs(1));
  EXPECT_THAT(warnings.warnings().front().range.start.offset, testing::Eq(2u));
  EXPECT_THAT(warnings.warnings().front().range.start.lineInfo, testing::Eq(std::nullopt));
}

TEST(SVGParser, CssImportAfterUnmappedChunkBoundaryKeepsCssLocalOffset) {
  // The entity makes the second chunk's source length differ from its decoded value, so only the
  // CDATA chunk is mapped. The import starts exactly where that mapped chunk ends, which is not
  // the end of the stylesheet, so it must not be mapped to the end of the CDATA content.
  const std::string source =
      "<svg xmlns='http://www.w3.org/2000/svg'><style><![CDATA[/*x*/]]>@import 'hidden.css'; "
      "/* &amp; */</style><rect/></svg>";
  ParseWarningSink warnings;
  EXPECT_THAT(SVGParser::ParseSVG(source, warnings), NoParseError());
  ASSERT_THAT(warnings.warnings(), testing::SizeIs(1));
  EXPECT_THAT(warnings.warnings().front().range.start.offset,
              testing::Eq(std::string_view("/*x*/").size()));
  EXPECT_THAT(warnings.warnings().front().range.start.lineInfo, testing::Eq(std::nullopt));
}

TEST(SVGParser, StylesheetPointMappingUsesSegmentEndOnlyAtStylesheetEnd) {
  components::StylesheetSourceMap trailingUnmapped;
  trailingUnmapped.addSegment(0, 3, FileOffset::Offset(100));
  trailingUnmapped.setCssLength(10);
  EXPECT_THAT(trailingUnmapped.mapPointToDocumentSource(FileOffset::Offset(3)),
              testing::Eq(std::nullopt));

  components::StylesheetSourceMap fullyMapped;
  fullyMapped.addSegment(0, 3, FileOffset::Offset(100));
  fullyMapped.setCssLength(3);
  EXPECT_THAT(fullyMapped.mapPointToDocumentSource(FileOffset::Offset(3)),
              testing::Eq(FileOffset::Offset(103)));
}

TEST(SVGParser, CssImportWarningRefusalFlagsSurviveSourceMapping) {
  std::string source = "<svg xmlns='http://www.w3.org/2000/svg'><style>";
  for (std::size_t i = 0; i <= ParseWarningSink::kMaximumWarnings; ++i) {
    source += "@import 'hidden.css';";
  }
  source += "rect { fill: green; }</style><rect/></svg>";
  ParseWarningSink warnings;
  EXPECT_THAT(SVGParser::ParseSVG(source, warnings), NoParseError());
  EXPECT_THAT(warnings.warnings(), testing::SizeIs(ParseWarningSink::kMaximumWarnings));
  EXPECT_THAT(warnings.resourceLimitExceeded(), testing::IsTrue());
  EXPECT_LE(warnings.warningBytes(), ParseWarningSink::kMaximumWarningBytes);
}

TEST(SVGParser, CssImportDiagnosticsDoNotBypassStylesheetPayloadAdmission) {
  constexpr std::string_view source =
      "<svg xmlns='http://www.w3.org/2000/svg'><style>"
      "@import 'hidden.css'; rect { fill: green; }</style></svg>";
  SVGParser::Options options;
  options.maximumParsedPayloadSize = 4096;
  ParseWarningSink warnings;
  auto result = SVGParser::ParseSVG(source, warnings, options);
  ASSERT_THAT(result, NoParseError());
  const auto* budget =
      result.result().registry().ctx().find<components::ParsedPayloadResourceBudget>();
  ASSERT_THAT(budget, testing::NotNull());
  EXPECT_THAT(budget->securityStats().rejected, testing::IsTrue());
  EXPECT_THAT(warnings.warnings(),
              testing::Contains(testing::Field(&ParseDiagnostic::reason,
                                               testing::HasSubstr("parsed-payload budget"))));
  EXPECT_THAT(warnings.resourceLimitExceeded(), testing::IsFalse());
}

TEST(SVGParser, StylesheetPointMappingDoesNotChangeExistingRangeEndSemantics) {
  components::StylesheetSourceMap map;
  map.addSegment(0, 3, FileOffset::Offset(100));
  map.addSegment(3, 10, FileOffset::Offset(300));
  EXPECT_THAT(map.mapPointToDocumentSource(FileOffset::Offset(3)),
              testing::Eq(FileOffset::Offset(300)));
  EXPECT_THAT(map.mapToDocumentSource(SourceRange{FileOffset::Offset(0), FileOffset::Offset(3)}),
              testing::Eq(SourceRange{FileOffset::Offset(100), FileOffset::Offset(103)}));
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

TEST(SVGParser, InvalidPresentationAttributeWarnsOnceAndKeepsRawAttribute) {
  const std::string_view invalidXml(
      R"(<svg xmlns="http://www.w3.org/2000/svg">
           <rect stroke-width="notanumber" />
         </svg>)");

  ParseWarningSink warnings;
  auto documentResult = SVGParser::ParseSVG(invalidXml, warnings);
  ASSERT_THAT(documentResult, NoParseError());

  // Exactly one diagnostic: the presentation-attribute value is parsed once, so a failure is
  // reported once. The position is the subparser's own, since an attribute value is not a view
  // into the document text and cannot be mapped back to it.
  EXPECT_THAT(warnings.warnings(),
              ElementsAre(ParseWarningIs(1, 0, "Invalid length or percentage")));

  // A value that fails to parse is still round-tripped as a raw XML attribute.
  const SVGElement rect = documentResult.result().querySelector("rect").value();
  EXPECT_THAT(rect.getAttribute("stroke-width"), testing::Optional(RcString("notanumber")));
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

    EXPECT_THAT(
        warnings.warnings(),
        ElementsAre(AllOf(ParseErrorPos(2, 13),
                          ParseErrorIs("Retaining element <path> with an unsupported namespace as "
                                       "unknown. Expected 'svg', found ''"))));
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

TEST(SVGParser, UnsupportedDeclaredEncodingFollowsRawByteValidation) {
  ParseWarningSink warnings;
  constexpr std::string_view prefix =
      "<?xml version='1.0' encoding='Windows-1251'?>"
      "<svg xmlns='http://www.w3.org/2000/svg'><text>";
  EXPECT_THAT(SVGParser::ParseSVG(std::string(prefix) + "ASCII</text></svg>", warnings),
              ParseErrorIs(testing::HasSubstr("Unsupported XML encoding")));
  EXPECT_THAT(SVGParser::ParseSVG(std::string(prefix) + "\xCF\xF0</text></svg>", warnings),
              ParseErrorIs("Invalid UTF-8 sequence"));
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

TEST(SVGParser, ForeignNamespaceElementsAreRetainedAsUnknown) {
  ParseWarningSink warnings;
  auto result = SVGParser::ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:other="http://example.test/other">)"
      R"(<other:group id="foreign" custom="kept"><rect id="inner" width="10" height="10"/>))"
      R"(</other:group></svg>)",
      warnings);
  ASSERT_THAT(result, NoParseError());

  // Retained and selectable, like unknown SVG-namespace elements.
  auto foreign = result.result().querySelector("#foreign");
  ASSERT_TRUE(foreign.has_value());
  EXPECT_EQ(foreign->type(), ElementType::Unknown);

  // SVG-namespace children inside the foreign wrapper project normally.
  auto inner = result.result().querySelector("#inner");
  ASSERT_TRUE(inner.has_value());
  EXPECT_EQ(inner->type(), ElementType::Rect);
  EXPECT_THAT(inner->getAttribute("width"), testing::Optional(RcString("10")));

  // The shared XML tree keeps the foreign element with attributes and source locations.
  xml::XMLDocument xmlDoc = result.result().xmlDocument();
  std::optional<xml::XMLNode> svgNode = xmlDoc.root().firstChild();
  ASSERT_TRUE(svgNode.has_value());
  std::optional<xml::XMLNode> foreignNode = svgNode->firstChild();
  ASSERT_TRUE(foreignNode.has_value());
  EXPECT_EQ(foreignNode->tagName(), xml::XMLQualifiedName(RcString("other"), RcString("group")));
  EXPECT_THAT(foreignNode->getAttribute("custom"), testing::Optional(RcString("kept")));
  std::optional<SourceRange> location = foreignNode->getNodeLocation();
  ASSERT_TRUE(location.has_value());
  ASSERT_TRUE(location->start.offset.has_value());
  ASSERT_TRUE(location->end.offset.has_value());
  const std::string_view source = result.result().source();
  EXPECT_THAT(
      source.substr(*location->start.offset, *location->end.offset - *location->start.offset),
      testing::StartsWith("<other:group"));
}

TEST(SVGParser, ForeignNamespaceSubtreeWarnsOnceAndCountsTowardCaps) {
  // A retained foreign subtree materializes one entity per element and is charged to the
  // tree-node and depth caps like any other content. Under the default caps a large subtree
  // parses, and the unsupported-namespace warning is reported once at the top of the subtree
  // rather than once per descendant.
  constexpr int kForeignDescendants = 512;
  std::string source =
      R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:other="http://example.test/other">)"
      R"(<other:root id="foreign-root">)";
  for (int i = 0; i < kForeignDescendants; ++i) {
    source += "<other:child/>";
  }
  source += "</other:root></svg>";

  ParseWarningSink warnings;
  auto result = SVGParser::ParseSVG(source, warnings);
  ASSERT_THAT(result, NoParseError());

  std::vector<std::string> retentionWarnings;
  std::string allWarnings;
  for (const ParseDiagnostic& warning : warnings.warnings()) {
    allWarnings += warning.reason.str();
    allWarnings += "\n";
    if (warning.reason.str().find("Retaining element") != std::string::npos) {
      retentionWarnings.push_back(warning.reason.str());
    }
  }
  ASSERT_EQ(retentionWarnings.size(), 1u) << allWarnings;
  EXPECT_THAT(retentionWarnings[0], testing::HasSubstr("other:root"));

  auto foreignRoot = result.result().querySelector("#foreign-root");
  ASSERT_TRUE(foreignRoot.has_value());
  EXPECT_EQ(foreignRoot->type(), ElementType::Unknown);
}

TEST(SVGParser, ForeignNamespaceNestingDoesNotSuppressALaterForeignSibling) {
  // The suppression state is counted, not a flag: leaving a foreign element nested inside a
  // foreign element must restore "inside a foreign subtree", while leaving the outer one must
  // clear it so the next top-level foreign element is still reported.
  ParseWarningSink warnings;
  auto result = SVGParser::ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:other="http://example.test/other">)"
      R"(<other:outer><other:inner/></other:outer>)"
      R"(<other:sibling/>)"
      R"(</svg>)",
      warnings);
  ASSERT_THAT(result, NoParseError());

  std::vector<std::string> retentionWarnings;
  std::string allWarnings;
  for (const ParseDiagnostic& warning : warnings.warnings()) {
    allWarnings += warning.reason.str();
    allWarnings += "\n";
    if (warning.reason.str().find("Retaining element") != std::string::npos) {
      retentionWarnings.push_back(warning.reason.str());
    }
  }
  ASSERT_EQ(retentionWarnings.size(), 2u) << allWarnings;
  EXPECT_THAT(retentionWarnings[0], testing::HasSubstr("other:outer"));
  EXPECT_THAT(retentionWarnings[1], testing::HasSubstr("other:sibling"));
}

TEST(SVGParser, ForeignNamespaceSubtreeExceedingTreeNodeCapIsRejected) {
  // The cap is enforced on the retained subtree, so a foreign document cannot buy unbounded
  // entities by being foreign.
  SVGParser::Options options;
  options.maximumTreeNodes = 8;

  std::string source =
      R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:other="http://example.test/other">)"
      R"(<other:root>)";
  for (int i = 0; i < 32; ++i) {
    source += "<other:child/>";
  }
  source += "</other:root></svg>";

  ParseWarningSink warnings;
  auto result = SVGParser::ParseSVG(source, warnings, options);
  EXPECT_THAT(result, ParseErrorIs(testing::HasSubstr("count exceeded")));
}

TEST(SVGParser, AnimationElementsDoNotRequireExperimentalOptIn) {
  const std::string_view source(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><animate id="a" attributeName="x"/></svg>)");

  {
    // Animation elements are enabled by the default parser.
    ParseWarningSink warnings;
    auto result = SVGParser::ParseSVG(source, warnings);
    ASSERT_THAT(result, NoParseError());

    auto element = result.result().querySelector("#a");
    ASSERT_TRUE(element.has_value());
    EXPECT_EQ(element->type(), ElementType::Animate);
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
    EXPECT_THAT(SVGParser::ParseSVG(corrupt, warnings), ParseErrorIs(testing::HasSubstr("gzip")));
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

TEST(SVGParser, ParseXMLDocumentEnforcesConversionTreeNodeLimit) {
  xml::XMLDocument xmlDocument;
  xml::XMLNode svg = xml::XMLNode::CreateElementNode(xmlDocument, "svg");
  svg.setAttribute("xmlns", "http://www.w3.org/2000/svg");
  xmlDocument.root().appendChild(svg);
  svg.appendChild(xml::XMLNode::CreateElementNode(xmlDocument, "rect"));
  svg.appendChild(xml::XMLNode::CreateElementNode(xmlDocument, "circle"));

  SVGParser::Options options;
  options.maximumTreeNodes = 2;
  ParseWarningSink warnings;
  EXPECT_THAT(SVGParser::ParseXMLDocument(std::move(xmlDocument), warnings, options),
              ParseErrorIs(testing::HasSubstr("tree-node count")));
}

TEST(SVGParser, ParseXMLDocumentEnforcesConversionTreeDepthLimit) {
  xml::XMLDocument xmlDocument;
  xml::XMLNode svg = xml::XMLNode::CreateElementNode(xmlDocument, "svg");
  svg.setAttribute("xmlns", "http://www.w3.org/2000/svg");
  xmlDocument.root().appendChild(svg);
  xml::XMLNode group = xml::XMLNode::CreateElementNode(xmlDocument, "g");
  svg.appendChild(group);
  group.appendChild(xml::XMLNode::CreateElementNode(xmlDocument, "rect"));

  SVGParser::Options options;
  options.maximumTreeDepth = 2;
  ParseWarningSink warnings;
  EXPECT_THAT(SVGParser::ParseXMLDocument(std::move(xmlDocument), warnings, options),
              ParseErrorIs(testing::HasSubstr("element depth")));
}

TEST(SVGParser, ParseSVGAppliesConversionLimitsToRawInput) {
  SVGParser::Options options;
  options.maximumTreeNodes = 2;
  ParseWarningSink warnings;
  EXPECT_THAT(
      SVGParser::ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><rect/><circle/></svg>)",
                          warnings, options),
      ParseErrorIs(testing::HasSubstr("element count")));
}

TEST(SVGParser, ParseSVGAppliesConversionDepthLimitToRawInput) {
  SVGParser::Options options;
  options.maximumTreeDepth = 2;
  ParseWarningSink warnings;
  EXPECT_THAT(SVGParser::ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><g><rect/></g></svg>)",
                                  warnings, options),
              ParseErrorIs(testing::AnyOf(testing::HasSubstr("nesting depth"),
                                          testing::HasSubstr("element depth"))));
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
  const auto& stylesheet = styleElement->entityHandle().get<components::StylesheetComponent>();
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
  const auto& descriptiveText = title->entityHandle().get<components::DescriptiveTextComponent>();
  EXPECT_EQ(descriptiveText.text, "Hello World");
}

TEST(SVGParser, AttributeDiagnosticsPrecedeContentBudgetDiagnostics) {
  ParseWarningSink warnings;
  SVGParser::Options options;
  options.maximumContentProjectionChunks = 1;
  auto result = SVGParser::ParseSVG(
      "<svg xmlns='http://www.w3.org/2000/svg'>"
      "<text stroke-width='invalid'>one<![CDATA[two]]></text></svg>",
      warnings, options);
  ASSERT_THAT(result, NoParseError());
  EXPECT_THAT(warnings.warnings(),
              ElementsAre(testing::Field(&ParseDiagnostic::reason,
                                         testing::Eq("Invalid length or percentage")),
                          testing::Field(&ParseDiagnostic::reason,
                                         testing::HasSubstr("content-projection budget"))));
  const auto text = result.result().querySelector("text");
  ASSERT_THAT(text, testing::Ne(std::nullopt));
  EXPECT_THAT(text->getAttribute("stroke-width"), testing::Optional(RcString("invalid")));
  EXPECT_THAT(text->cast<SVGTextElement>().textContent(), testing::IsEmpty());
}

TEST(SVGParser, NestedElementsConsumeTextChunksButNotDescriptiveChunks) {
  for (const std::string_view tag : {"text", "title", "desc", "metadata"}) {
    SCOPED_TRACE(tag);
    const std::string source = "<svg xmlns='http://www.w3.org/2000/svg'><" + std::string(tag) +
                               " id='parent'>one<g/><!-- ignored --><![CDATA[two]]></" +
                               std::string(tag) + "></svg>";
    for (const std::size_t limit : {2u, 3u}) {
      SCOPED_TRACE(limit);
      SVGParser::Options options;
      options.maximumContentProjectionChunks = limit;
      ParseWarningSink warnings;
      auto result = SVGParser::ParseSVG(source, warnings, options);
      ASSERT_THAT(result, NoParseError());
      const auto parent = result.result().querySelector("#parent");
      ASSERT_THAT(parent, testing::Ne(std::nullopt));
      const bool rejected = tag == "text" && limit == 2;
      if (tag == "text") {
        EXPECT_THAT(parent->cast<SVGTextElement>().textContent(),
                    testing::Eq(rejected ? "" : "onetwo"));
      } else {
        EXPECT_THAT(parent->entityHandle().get<components::DescriptiveTextComponent>().text,
                    testing::Eq("onetwo"));
      }
      EXPECT_THAT(result.result()
                      .registry()
                      .ctx()
                      .get<components::ParsedPayloadResourceBudget>()
                      .securityStats()
                      .rejected,
                  testing::Eq(rejected));
      if (rejected) {
        EXPECT_THAT(warnings.warnings(),
                    ElementsAre(testing::Field(&ParseDiagnostic::reason,
                                               testing::HasSubstr("content-projection budget"))));
      } else {
        EXPECT_THAT(warnings.warnings(), testing::IsEmpty());
      }
    }
  }
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

  EXPECT_THAT(
      renderWarnings.warnings(),
      testing::Contains(testing::Field(&ParseDiagnostic::reason,
                                       testing::HasSubstr("Unexpected element <notsvg> at root"))));
}

TEST(SVGParser, SecureStaticModeParsesDataSubDocuments) {
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

  EXPECT_THAT(
      renderWarnings.warnings(),
      testing::Contains(testing::Field(&ParseDiagnostic::reason,
                                       testing::HasSubstr("Unexpected element <notsvg> at root"))));
}

namespace {

std::string SvgDataUrl(std::string_view source) {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string result = "data:image/svg+xml,";
  for (const unsigned char byte : source) {
    result += '%';
    result += kHex[byte >> 4];
    result += kHex[byte & 15];
  }
  return result;
}

std::string SvgImageDocument(std::string_view uri) {
  return "<svg xmlns='http://www.w3.org/2000/svg'><image id='image' href='" + std::string(uri) +
         "' width='8' height='8'/></svg>";
}

class NestedSvgOptionsTest : public testing::TestWithParam<ProcessingMode> {
protected:
  ParseResult<SVGDocument> parseParent(std::string_view uri, SVGParser::Options options = {},
                                       SVGDocument::Settings settings = {}) {
    settings.processingMode = GetParam();
    return SVGParser::ParseSVG(SvgImageDocument(uri), warnings_, options, std::move(settings));
  }

  SVGDocumentHandle loadChild(SVGDocument& document) {
    document.registry().ctx().get<components::ResourceManagerContext>().loadResources(warnings_);
    auto image = document.querySelector("#image");
    if (!image) {
      ADD_FAILURE() << "The parent image must survive parsing";
      return {};
    }
    const auto* loaded = image->entityHandle().try_get<components::LoadedSVGImageComponent>();
    return loaded ? loaded->subDocument : SVGDocumentHandle{};
  }

  ParseWarningSink warnings_;
};

TEST_P(NestedSvgOptionsTest, TreeNodeLimitAppliesToDataChild) {
  const std::string uri =
      SvgDataUrl("<svg xmlns='http://www.w3.org/2000/svg'><rect/><circle/></svg>");
  SVGParser::Options options;
  options.maximumTreeNodes = 2;
  auto parent = parseParent(uri, options);
  ASSERT_THAT(parent, NoParseError());
  EXPECT_FALSE(loadChild(parent.result()));
  EXPECT_THAT(warnings_.warnings(),
              testing::Contains(
                  testing::Field(&ParseDiagnostic::reason, testing::HasSubstr("element count"))));
  auto unrestricted = parseParent(uri);
  ASSERT_THAT(unrestricted, NoParseError());
  EXPECT_TRUE(loadChild(unrestricted.result()));
}

TEST_P(NestedSvgOptionsTest, TreeDepthLimitAppliesToDataChild) {
  const std::string uri =
      SvgDataUrl("<svg xmlns='http://www.w3.org/2000/svg'><g><rect/></g></svg>");
  SVGParser::Options options;
  options.maximumTreeDepth = 2;
  auto parent = parseParent(uri, options);
  ASSERT_THAT(parent, NoParseError());
  EXPECT_FALSE(loadChild(parent.result()));
  EXPECT_THAT(warnings_.warnings(), testing::Contains(testing::Field(&ParseDiagnostic::reason,
                                                                     testing::HasSubstr("depth"))));
  auto unrestricted = parseParent(uri);
  ASSERT_THAT(unrestricted, NoParseError());
  EXPECT_TRUE(loadChild(unrestricted.result()));
}

TEST_P(NestedSvgOptionsTest, ExpandedInputLimitAppliesToDataChild) {
  // SVGZ expands to a namespace-qualified SVG containing 1024 'A' bytes in a desc element.
  constexpr std::string_view kUri =
      "data:image/svg+xml;base64,"
      "H4sIAAAAAAAC/7MpLktXqMjNySu2Vc8oKSmw0tcvLy/XKzfWyy9K1zcyMDDQB6pQt7NJSS1OtnMcBaNg"
      "FIxYYKMPLgZsQGWCHQCOY7VtOwQAAA==";
  SVGParser::Options options;
  options.maximumInputSize = 512;
  ASSERT_LT(SvgImageDocument(kUri).size(), options.maximumInputSize);
  auto parent = parseParent(kUri, options);
  ASSERT_THAT(parent, NoParseError());
  EXPECT_FALSE(loadChild(parent.result()));
  EXPECT_THAT(warnings_.warnings(),
              testing::Contains(testing::Field(&ParseDiagnostic::reason,
                                               testing::HasSubstr("maximum input size"))));
  auto unrestricted = parseParent(kUri);
  ASSERT_THAT(unrestricted, NoParseError());
  EXPECT_TRUE(loadChild(unrestricted.result()));
}

TEST_P(NestedSvgOptionsTest, ContentProjectionLimitAppliesToDataChild) {
  const std::string uri = SvgDataUrl(
      "<svg xmlns='http://www.w3.org/2000/svg'><text id='t'>one<![CDATA[two]]></text></svg>");
  SVGParser::Options options;
  options.maximumContentProjectionChunks = 1;
  auto parent = parseParent(uri, options);
  ASSERT_THAT(parent, NoParseError());
  auto childHandle = loadChild(parent.result());
  ASSERT_TRUE(childHandle);
  auto child = SVGDocument::CreateFromHandle(childHandle);
  ASSERT_TRUE(child.querySelector("#t"));
  EXPECT_TRUE(child.querySelector("#t")->cast<SVGTextElement>().textContent().empty());
  EXPECT_TRUE(child.registry()
                  .ctx()
                  .get<components::ParsedPayloadResourceBudget>()
                  .securityStats()
                  .rejected);
  EXPECT_THAT(warnings_.warnings(),
              testing::Contains(testing::Field(&ParseDiagnostic::reason,
                                               testing::HasSubstr("content-projection budget"))));
}

TEST_P(NestedSvgOptionsTest, LocalPayloadLimitSurvivesLargerSuppliedFamily) {
  const std::string uri = SvgDataUrl("<svg xmlns='http://www.w3.org/2000/svg'><style>" +
                                     std::string(128, ' ') + "rect { fill: green; }</style></svg>");
  SVGParser::Options options;
  options.maximumParsedPayloadSize = 64 * 1024;
  auto family = std::make_shared<components::DocumentResourceFamilyBudget>();
  SVGDocument::Settings settings;
  settings.resourceFamilyBudget = family;
  auto parent = parseParent(uri, options, std::move(settings));
  ASSERT_THAT(parent, NoParseError());
  EXPECT_FALSE(parent.result()
                   .registry()
                   .ctx()
                   .get<components::ParsedPayloadResourceBudget>()
                   .securityStats()
                   .rejected);
  auto childHandle = loadChild(parent.result());
  ASSERT_TRUE(childHandle);
  auto child = SVGDocument::CreateFromHandle(childHandle);
  EXPECT_EQ(child.registry().ctx().get<components::DocumentResourceFamilyContext>().budget, family);
  EXPECT_TRUE(child.registry()
                  .ctx()
                  .get<components::ParsedPayloadResourceBudget>()
                  .securityStats()
                  .rejected);
  EXPECT_THAT(warnings_.warnings(),
              testing::Contains(testing::Field(&ParseDiagnostic::reason,
                                               testing::HasSubstr("parsed-payload budget"))));
}

TEST_P(NestedSvgOptionsTest, LimitSnapshotReachesGrandchild) {
  const std::string grandchild = "<svg xmlns='http://www.w3.org/2000/svg'><rect/><circle/></svg>";
  const std::string childSource = SvgImageDocument(SvgDataUrl(grandchild));
  SVGParser::Options options;
  options.maximumTreeNodes = 2;
  auto parent = parseParent(SvgDataUrl(childSource), options);
  ASSERT_THAT(parent, NoParseError());
  options.maximumTreeNodes = SVGParser::kDefaultMaximumTreeNodes;
  auto childHandle = loadChild(parent.result());
  ASSERT_TRUE(childHandle);
  auto child = SVGDocument::CreateFromHandle(childHandle);
  EXPECT_FALSE(loadChild(child));
  EXPECT_THAT(warnings_.warnings(),
              testing::Contains(
                  testing::Field(&ParseDiagnostic::reason, testing::HasSubstr("element count"))));
}

TEST_P(NestedSvgOptionsTest, FeatureAndAttributeOptionsReachGrandchild) {
  const std::string grandchild =
      "<svg xmlns='http://www.w3.org/2000/svg'><animate id='a' attributeName='x'/>"
      "<rect id='r' user-attribute='value'/></svg>";
  SVGParser::Options options;
  options.enableExperimental = true;
  options.disableUserAttributes = true;
  auto parent = parseParent(SvgDataUrl(SvgImageDocument(SvgDataUrl(grandchild))), options);
  ASSERT_THAT(parent, NoParseError());
  auto childHandle = loadChild(parent.result());
  ASSERT_TRUE(childHandle);
  auto child = SVGDocument::CreateFromHandle(childHandle);
  auto grandchildHandle = loadChild(child);
  ASSERT_TRUE(grandchildHandle);
  auto nested = SVGDocument::CreateFromHandle(grandchildHandle);
  ASSERT_TRUE(nested.querySelector("#a"));
  EXPECT_EQ(nested.querySelector("#a")->type(), ElementType::Animate);
  ASSERT_TRUE(nested.querySelector("#r"));
  EXPECT_EQ(nested.querySelector("#r")->getAttribute("user-attribute"), std::nullopt);
}

TEST_P(NestedSvgOptionsTest, InlineParentDoesNotMakeDataChildInline) {
  SVGParser::Options options;
  options.parseAsInlineSVG = true;
  auto parent = parseParent(SvgDataUrl("<svg><rect/></svg>"), options);
  ASSERT_THAT(parent, NoParseError());
  EXPECT_FALSE(loadChild(parent.result()));
  EXPECT_THAT(warnings_.warnings(),
              testing::Contains(testing::Field(&ParseDiagnostic::reason,
                                               testing::HasSubstr("empty namespace URI"))));
}

TEST_P(NestedSvgOptionsTest, ExplicitCallbackRetainsItsOwnParsingPolicy) {
  const std::string childSource = "<svg xmlns='http://www.w3.org/2000/svg'><rect/><circle/></svg>";
  int callbackCount = 0;
  SVGDocument suppliedChild;
  SVGDocument::Settings settings;
  settings.svgParseCallback = [&callbackCount, &childSource, &suppliedChild](
                                  const std::vector<uint8_t>& bytes,
                                  ParseWarningSink&) -> std::optional<SVGDocumentHandle> {
    ++callbackCount;
    EXPECT_EQ(std::string(bytes.begin(), bytes.end()), childSource);
    return suppliedChild.handle();
  };
  SVGParser::Options options;
  options.maximumTreeNodes = 2;
  auto parent = parseParent(SvgDataUrl(childSource), options, std::move(settings));
  ASSERT_THAT(parent, NoParseError());
  EXPECT_EQ(loadChild(parent.result()), suppliedChild.handle());
  EXPECT_EQ(callbackCount, 1);
}

INSTANTIATE_TEST_SUITE_P(ProcessingModes, NestedSvgOptionsTest,
                         testing::Values(ProcessingMode::DynamicInteractive,
                                         ProcessingMode::SecureStatic,
                                         ProcessingMode::SecureAnimated));

}  // namespace

}  // namespace donner::svg::parser
