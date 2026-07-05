#include "donner/base/xml/XMLParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>
#include <utility>

#include "donner/base/ParseResult.h"
#include "donner/base/RcString.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/xml/XMLIncrementalParser.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/base/xml/components/EntityDeclarationsContext.h"

using testing::AllOf;
using testing::ElementsAre;
using testing::Eq;
using testing::Field;

// TODO: Add an ErrorHighlightsText matcher

namespace donner::xml {

auto MutationKindIs(XMLMutation::Kind kind) {
  return Field("kind", &XMLMutation::kind, kind);
}

auto MutationIs(XMLMutation::Kind kind, XMLNode node, ReparseScope scope) {
  return AllOf(MutationKindIs(kind), Field("node", &XMLMutation::node, node),
               Field("scope", &XMLMutation::scope, scope));
}

template <typename ValueMatcher>
auto AttributeMutationIs(XMLMutation::Kind kind, XMLNode node, XMLQualifiedName attributeName,
                         ValueMatcher valueMatcher, ReparseScope scope) {
  return AllOf(MutationIs(kind, node, scope),
               Field("attributeName", &XMLMutation::attributeName, std::move(attributeName)),
               Field("value", &XMLMutation::value, valueMatcher));
}

template <typename ValueMatcher>
auto NodeValueMutationIs(XMLNode node, ValueMatcher valueMatcher, ReparseScope scope) {
  return AllOf(MutationIs(XMLMutation::Kind::NodeValueChanged, node, scope),
               Field("value", &XMLMutation::value, valueMatcher));
}

template <typename DiagnosticMatcher>
auto SourceDiagnosticMutationIs(XMLNode node, DiagnosticMatcher diagnosticMatcher,
                                ReparseScope scope) {
  return AllOf(MutationIs(XMLMutation::Kind::SourceDiagnosticChanged, node, scope),
               Field("diagnostic", &XMLMutation::diagnostic, diagnosticMatcher));
}

std::string Utf8(char32_t codepoint) {
  std::string result;
  if (codepoint <= 0x7F) {
    result.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    result.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    result.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    result.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
  return result;
}

std::string_view SourceText(const XMLDocument& document, SourceRange range) {
  if (!range.start.offset.has_value() || !range.end.offset.has_value() ||
      *range.end.offset < *range.start.offset || *range.end.offset > document.source().size()) {
    return {};
  }

  return document.source().substr(*range.start.offset, *range.end.offset - *range.start.offset);
}

class XMLParserTests : public testing::Test {
protected:
  XMLParser::Options optionsCustomEntities() {
    XMLParser::Options options;
    options.parseCustomEntities = true;
    return options;
  }

  XMLParser::Options optionsDisableEntityTranslation() {
    XMLParser::Options options;
    options.disableEntityTranslation = true;
    return options;
  }

  /**
   * Parse an XML string and return the first node.
   */
  std::optional<XMLNode> parseAndGetFirstNode(std::string_view xml,
                                              XMLParser::Options options = {}) {
    SCOPED_TRACE(testing::Message() << "Parsing XML:\n" << xml << "\n\n");

    ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(xml, options);
    EXPECT_THAT(maybeDocument, NoParseError());
    if (maybeDocument.hasError()) {
      return std::nullopt;
    }

    document_ = std::move(maybeDocument.result());
    XMLNode root = document_.root();
    EXPECT_EQ(root.type(), XMLNode::Type::Document);
    EXPECT_THAT(root.nextSibling(), Eq(std::nullopt))
        << "XML must contain only a single element, such as <node></node>";

    return root.firstChild();
  }

  /**
   * Parse an XML string of format `<node>...</node>` and return the contents of the node.
   */
  ParseResult<RcString> parseAndGetNodeContents(std::string_view xml,
                                                XMLParser::Options options = {}) {
    std::optional<XMLNode> maybeNode = parseAndGetFirstNode(xml, options);
    EXPECT_TRUE(maybeNode.has_value())
        << "XML must contain a single element, such as <node></node>";

    if (!maybeNode) {
      return RcString("");
    }

    EXPECT_EQ(maybeNode->type(), XMLNode::Type::Element);

    EXPECT_THAT(maybeNode->nextSibling(), Eq(std::nullopt))
        << "XML must contain only a single element, such as <node></node>";

    if (maybeNode->value()) {
      return maybeNode->value().value();
    } else {
      return RcString("");
    }
  }

private:
  XMLDocument document_;
};

TEST_F(XMLParserTests, ParsedDocumentOwnsSourceStore) {
  constexpr std::string_view kXml = R"(<svg><rect fill="red"/></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  EXPECT_TRUE(document.hasSourceStore());
  EXPECT_EQ(document.source(), kXml);
  EXPECT_EQ(document.sourceVersion(), 0u);

  XMLSourceStore* sourceStore = document.sourceStore();
  ASSERT_NE(sourceStore, nullptr);
  const std::size_t fillOffset = document.source().find("red");
  ASSERT_NE(fillOffset, std::string_view::npos);

  std::optional<SourceAnchorSpan> fillSpan = sourceStore->createSpan(fillOffset, fillOffset + 3);
  ASSERT_TRUE(fillSpan.has_value());
  ASSERT_TRUE(sourceStore->replace(fillOffset, 3, "blue").has_value());

  EXPECT_EQ(document.source(), R"(<svg><rect fill="blue"/></svg>)");
  EXPECT_EQ(document.sourceVersion(), 1u);
  EXPECT_EQ(sourceStore->resolveSpan(*fillSpan), (ResolvedSourceSpan{fillOffset, fillOffset + 4}));
}

TEST_F(XMLParserTests, ParsedNodeLocationsFollowSourceStoreEdits) {
  constexpr std::string_view kXml = R"(<svg><rect id="r" fill="red"/></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode svg = document.root().firstChild().value();
  XMLNode rect = svg.firstChild().value();

  std::optional<SourceRange> originalLocation = rect.getNodeLocation();
  ASSERT_TRUE(originalLocation.has_value());
  ASSERT_TRUE(originalLocation->start.offset.has_value());
  ASSERT_TRUE(originalLocation->end.offset.has_value());
  EXPECT_EQ(
      document.source().substr(*originalLocation->start.offset,
                               *originalLocation->end.offset - *originalLocation->start.offset),
      R"(<rect id="r" fill="red"/>)");

  XMLSourceStore* sourceStore = document.sourceStore();
  ASSERT_NE(sourceStore, nullptr);
  ASSERT_TRUE(sourceStore->replace(*originalLocation->start.offset, 0, "<g/>").has_value());
  EXPECT_EQ(document.source(), R"(<svg><g/><rect id="r" fill="red"/></svg>)");

  std::optional<SourceRange> updatedLocation = rect.getNodeLocation();
  ASSERT_TRUE(updatedLocation.has_value());
  ASSERT_TRUE(updatedLocation->start.offset.has_value());
  ASSERT_TRUE(updatedLocation->end.offset.has_value());
  EXPECT_EQ(document.source().substr(*updatedLocation->start.offset,
                                     *updatedLocation->end.offset - *updatedLocation->start.offset),
            R"(<rect id="r" fill="red"/>)");
  EXPECT_EQ(rect.sourceStartOffset(), updatedLocation->start);

  std::optional<SourceRange> fillLocation = rect.getAttributeLocation(document.source(), "fill");
  ASSERT_TRUE(fillLocation.has_value());
  ASSERT_TRUE(fillLocation->start.offset.has_value());
  ASSERT_TRUE(fillLocation->end.offset.has_value());
  EXPECT_EQ(document.source().substr(*fillLocation->start.offset,
                                     *fillLocation->end.offset - *fillLocation->start.offset),
            R"(fill="red")");

  ASSERT_TRUE(sourceStore->replace(*fillLocation->start.offset, 0, R"(data-x="1" )").has_value());
  std::optional<SourceRange> movedFillLocation =
      rect.getAttributeLocation(document.source(), "fill");
  ASSERT_TRUE(movedFillLocation.has_value());
  ASSERT_TRUE(movedFillLocation->start.offset.has_value());
  ASSERT_TRUE(movedFillLocation->end.offset.has_value());
  EXPECT_EQ(
      document.source().substr(*movedFillLocation->start.offset,
                               *movedFillLocation->end.offset - *movedFillLocation->start.offset),
      R"(fill="red")");

  std::optional<XMLAttributeAtSourceOffset> attributeAtOffset =
      document.attributeAtSourceOffset(*movedFillLocation->start.offset + 1);
  ASSERT_TRUE(attributeAtOffset.has_value());
  EXPECT_EQ(attributeAtOffset->node, rect);
  EXPECT_EQ(attributeAtOffset->name, XMLQualifiedName("fill"));
  EXPECT_EQ(attributeAtOffset->location, *movedFillLocation);
}

TEST_F(XMLParserTests, ParsedAttributeSourceMetadataFollowsSourceStoreEdits) {
  constexpr std::string_view kXml = R"(<svg><rect id='r' fill="red"/></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode rect = document.root().firstChild()->firstChild().value();

  std::optional<XMLAttributeSourceLocation> idLocation = rect.getAttributeSourceLocation("id");
  ASSERT_TRUE(idLocation.has_value());
  EXPECT_EQ(idLocation->quote, '\'');
  EXPECT_EQ(document.source().substr(
                *idLocation->fullRange.start.offset,
                *idLocation->fullRange.end.offset - *idLocation->fullRange.start.offset),
            R"(id='r')");
  EXPECT_EQ(document.source().substr(
                *idLocation->valueRange.start.offset,
                *idLocation->valueRange.end.offset - *idLocation->valueRange.start.offset),
            "r");

  std::optional<XMLAttributeSourceLocation> fillLocation = rect.getAttributeSourceLocation("fill");
  ASSERT_TRUE(fillLocation.has_value());
  EXPECT_EQ(fillLocation->quote, '"');
  EXPECT_EQ(document.source().substr(
                *fillLocation->fullRange.start.offset,
                *fillLocation->fullRange.end.offset - *fillLocation->fullRange.start.offset),
            R"(fill="red")");
  EXPECT_EQ(document.source().substr(
                *fillLocation->valueRange.start.offset,
                *fillLocation->valueRange.end.offset - *fillLocation->valueRange.start.offset),
            "red");

  std::optional<XMLAttributeAtSourceOffset> attributeAtOffset =
      document.attributeAtSourceOffset(*fillLocation->valueRange.start.offset);
  ASSERT_TRUE(attributeAtOffset.has_value());
  EXPECT_EQ(attributeAtOffset->node, rect);
  EXPECT_EQ(attributeAtOffset->name, XMLQualifiedName("fill"));
  EXPECT_EQ(attributeAtOffset->location, fillLocation->fullRange);
  EXPECT_EQ(attributeAtOffset->valueLocation, fillLocation->valueRange);
  EXPECT_EQ(attributeAtOffset->quote, '"');

  XMLSourceStore* sourceStore = document.sourceStore();
  ASSERT_NE(sourceStore, nullptr);
  ASSERT_TRUE(
      sourceStore->replace(*fillLocation->fullRange.start.offset, 0, R"(data-x="1" )").has_value());

  std::optional<XMLAttributeSourceLocation> movedFillLocation =
      rect.getAttributeSourceLocation("fill");
  ASSERT_TRUE(movedFillLocation.has_value());
  EXPECT_EQ(document.source().substr(*movedFillLocation->fullRange.start.offset,
                                     *movedFillLocation->fullRange.end.offset -
                                         *movedFillLocation->fullRange.start.offset),
            R"(fill="red")");

  ASSERT_TRUE(
      sourceStore->replace(*movedFillLocation->valueRange.start.offset, 0, "dark").has_value());
  std::optional<XMLAttributeSourceLocation> expandedFillLocation =
      rect.getAttributeSourceLocation("fill");
  ASSERT_TRUE(expandedFillLocation.has_value());
  EXPECT_EQ(document.source().substr(*expandedFillLocation->valueRange.start.offset,
                                     *expandedFillLocation->valueRange.end.offset -
                                         *expandedFillLocation->valueRange.start.offset),
            "darkred");
}

TEST_F(XMLParserTests, ParsedElementSubspansFollowSourceStoreEdits) {
  constexpr std::string_view kXml = R"(<svg><text>Hello</text><empty/></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode svg = document.root().firstChild().value();
  XMLNode text = svg.firstChild().value();
  XMLNode data = text.firstChild().value();
  XMLNode empty = text.nextSibling().value();

  ASSERT_TRUE(svg.getOpeningTagLocation().has_value());
  EXPECT_EQ(SourceText(document, *svg.getOpeningTagLocation()), "<svg>");
  ASSERT_TRUE(svg.getClosingTagLocation().has_value());
  EXPECT_EQ(SourceText(document, *svg.getClosingTagLocation()), "</svg>");

  ASSERT_TRUE(text.getOpeningTagLocation().has_value());
  EXPECT_EQ(SourceText(document, *text.getOpeningTagLocation()), "<text>");
  ASSERT_TRUE(text.getClosingTagLocation().has_value());
  EXPECT_EQ(SourceText(document, *text.getClosingTagLocation()), "</text>");
  ASSERT_TRUE(text.getValueLocation().has_value());
  EXPECT_EQ(SourceText(document, *text.getValueLocation()), "Hello");

  ASSERT_TRUE(data.getValueLocation().has_value());
  EXPECT_EQ(SourceText(document, *data.getValueLocation()), "Hello");

  ASSERT_TRUE(empty.getOpeningTagLocation().has_value());
  EXPECT_EQ(SourceText(document, *empty.getOpeningTagLocation()), "<empty/>");
  EXPECT_EQ(empty.getClosingTagLocation(), std::nullopt);
  EXPECT_EQ(empty.getValueLocation(), std::nullopt);

  XMLSourceStore* sourceStore = document.sourceStore();
  ASSERT_NE(sourceStore, nullptr);
  ASSERT_TRUE(
      sourceStore->replace(*text.getOpeningTagLocation()->start.offset, 0, "<g/>").has_value());

  ASSERT_TRUE(text.getOpeningTagLocation().has_value());
  EXPECT_EQ(SourceText(document, *text.getOpeningTagLocation()), "<text>");
  ASSERT_TRUE(text.getClosingTagLocation().has_value());
  EXPECT_EQ(SourceText(document, *text.getClosingTagLocation()), "</text>");
  ASSERT_TRUE(text.getValueLocation().has_value());
  EXPECT_EQ(SourceText(document, *text.getValueLocation()), "Hello");
}

TEST_F(XMLParserTests, ParsedTextLikeNodeValueSubspans) {
  constexpr std::string_view kXml =
      R"(<?pi value?><!DOCTYPE svg><!-- comment --><svg><![CDATA[x<y]]></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml, XMLParser::Options::ParseAll());
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode pi = document.root().firstChild().value();
  XMLNode doctype = pi.nextSibling().value();
  XMLNode comment = doctype.nextSibling().value();
  XMLNode svg = comment.nextSibling().value();
  XMLNode cdata = svg.firstChild().value();

  ASSERT_TRUE(pi.getValueLocation().has_value());
  EXPECT_EQ(SourceText(document, *pi.getValueLocation()), "value");
  ASSERT_TRUE(doctype.getValueLocation().has_value());
  EXPECT_EQ(SourceText(document, *doctype.getValueLocation()), "svg");
  ASSERT_TRUE(comment.getOpeningTagLocation().has_value());
  EXPECT_EQ(SourceText(document, *comment.getOpeningTagLocation()), "<!--");
  ASSERT_TRUE(comment.getClosingTagLocation().has_value());
  EXPECT_EQ(SourceText(document, *comment.getClosingTagLocation()), "-->");
  ASSERT_TRUE(comment.getValueLocation().has_value());
  EXPECT_EQ(SourceText(document, *comment.getValueLocation()), " comment ");
  ASSERT_TRUE(cdata.getOpeningTagLocation().has_value());
  EXPECT_EQ(SourceText(document, *cdata.getOpeningTagLocation()), "<![CDATA[");
  ASSERT_TRUE(cdata.getClosingTagLocation().has_value());
  EXPECT_EQ(SourceText(document, *cdata.getClosingTagLocation()), "]]>");
  ASSERT_TRUE(cdata.getValueLocation().has_value());
  EXPECT_EQ(SourceText(document, *cdata.getValueLocation()), "x<y");
}

TEST_F(XMLParserTests, ParsesRepresentativeUnicodeNameStartRanges) {
  constexpr std::array<char32_t, 24> kNameStartCodepoints = {
      U'_',   U'a',   U'Z',   0x00C0, 0x00D6, 0x00D8, 0x00F6, 0x00F8,
      0x02FF, 0x0370, 0x037D, 0x037F, 0x1FFF, 0x200C, 0x200D, 0x2070,
      0x218F, 0x2C00, 0x2FEF, 0x3001, 0xD7FF, 0xF900, 0xFDF0, 0xEFFFF,
  };

  std::string xml = "<root>";
  for (std::size_t index = 0; index < kNameStartCodepoints.size(); ++index) {
    xml += "<";
    xml += Utf8(kNameStartCodepoints[index]);
    xml += "node";
    xml += std::to_string(index);
    xml += "/>";
  }
  xml += "</root>";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(xml);

  ASSERT_THAT(maybeDocument, NoParseError());
  XMLNode root = maybeDocument.result().root().firstChild().value();
  std::size_t childCount = 0;
  for (std::optional<XMLNode> child = root.firstChild(); child.has_value();
       child = child->nextSibling()) {
    ++childCount;
  }
  EXPECT_EQ(childCount, kNameStartCodepoints.size());
}

TEST_F(XMLParserTests, ParsesRepresentativeUnicodeNameCharRanges) {
  constexpr std::array<char32_t, 10> kNameCharCodepoints = {
      U'-', U'.', U'9', 0x00B7, 0x0300, 0x036F, 0x203F, 0x2040, 0x3001, 0xE0100,
  };

  std::string name = "a";
  for (char32_t codepoint : kNameCharCodepoints) {
    name += Utf8(codepoint);
  }
  const std::string xml = "<root " + name + R"(="value"/>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(xml);

  ASSERT_THAT(maybeDocument, NoParseError());
  XMLNode root = maybeDocument.result().root().firstChild().value();
  EXPECT_THAT(root.getAttribute(XMLQualifiedNameRef(name)), testing::Optional(RcString("value")));
}

TEST_F(XMLParserTests, RejectsInvalidUnicodeNameStartAndNameChar) {
  EXPECT_THAT(
      XMLParser::Parse("<root><" + Utf8(0x0300) + "bad/></root>"),
      ParseErrorIs("Invalid element name: Expected qualified name, found invalid character"));
  EXPECT_THAT(XMLParser::Parse("<root a" + Utf8(0xFFFE) + R"(="value"/>)"),
              ParseErrorIs("Attribute name without value, expected '=' followed by a string"));
}

TEST_F(XMLParserTests, MalformedUtf8NamesArePreservedAsNameBytes) {
  std::string invalidElementTagName;
  invalidElementTagName.push_back(static_cast<char>(0xC3));
  invalidElementTagName += "bad";
  std::string invalidElementName = "<root><";
  invalidElementName += invalidElementTagName;
  invalidElementName += "/></root>";
  ParseResult<XMLDocument> maybeElementDocument = XMLParser::Parse(invalidElementName);
  ASSERT_THAT(maybeElementDocument, NoParseError());
  XMLNode malformedElement =
      maybeElementDocument.result().root().firstChild()->firstChild().value();
  EXPECT_THAT(malformedElement.tagName(), Eq(invalidElementTagName));

  std::string invalidAttributeQualifiedName = "a";
  invalidAttributeQualifiedName.push_back(static_cast<char>(0xCC));
  std::string invalidAttributeName = "<root a";
  invalidAttributeName.push_back(static_cast<char>(0xCC));
  invalidAttributeName += R"(="value"/>)";
  ParseResult<XMLDocument> maybeAttributeDocument = XMLParser::Parse(invalidAttributeName);
  ASSERT_THAT(maybeAttributeDocument, NoParseError());
  XMLNode root = maybeAttributeDocument.result().root().firstChild().value();
  EXPECT_THAT(root.getAttribute(XMLQualifiedNameRef(invalidAttributeQualifiedName)),
              testing::Optional(RcString("value")));

  XMLParser::Options options = XMLParser::Options::ParseAll();
  std::string invalidInstructionTarget;
  invalidInstructionTarget.push_back(static_cast<char>(0xC3));
  std::string invalidProcessingInstructionTarget = "<?";
  invalidProcessingInstructionTarget += invalidInstructionTarget;
  invalidProcessingInstructionTarget += " value?><root/>";
  ParseResult<XMLDocument> maybeInstructionDocument =
      XMLParser::Parse(invalidProcessingInstructionTarget, options);
  ASSERT_THAT(maybeInstructionDocument, NoParseError());
  XMLNode instruction = maybeInstructionDocument.result().root().firstChild().value();
  EXPECT_THAT(instruction.tagName(), Eq(invalidInstructionTarget));
}

TEST_F(XMLParserTests, MalformedUtf8EntityNamesRemainLiteral) {
  std::string invalidEntityStart = "<node>&";
  invalidEntityStart.push_back(static_cast<char>(0xC3));
  invalidEntityStart += ";</node>";
  EXPECT_THAT(parseAndGetNodeContents(invalidEntityStart), ParseResultIs(RcString("&\xC3;")));

  std::string invalidEntityNameChar = "<node>&a";
  invalidEntityNameChar.push_back(static_cast<char>(0xCC));
  invalidEntityNameChar += ";</node>";
  EXPECT_THAT(parseAndGetNodeContents(invalidEntityNameChar), ParseResultIs(RcString("&a\xCC;")));
}

TEST_F(XMLParserTests, XMLIncrementalParserParsesLocalFragments) {
  ParseResult<XMLDocument> maybeAttribute =
      XMLIncrementalParser::ParseAttribute(R"(fill="a&amp;b")");
  ASSERT_THAT(maybeAttribute, NoParseError());
  XMLNode attributeElement = maybeAttribute.result().root().firstChild().value();
  EXPECT_THAT(attributeElement.getAttribute("fill"), testing::Optional(RcString("a&b")));

  ParseResult<XMLDocument> maybeOpeningTag =
      XMLIncrementalParser::ParseOpeningTag(R"(<rect fill="red">)");
  ASSERT_THAT(maybeOpeningTag, NoParseError());
  XMLDocument openingTagDocument = std::move(maybeOpeningTag.result());
  XMLNode rect = openingTagDocument.root().firstChild().value();
  EXPECT_EQ(rect.tagName(), XMLQualifiedName("rect"));
  EXPECT_THAT(rect.getAttribute("fill"), testing::Optional(RcString("red")));
  std::optional<XMLAttributeSourceLocation> fillLocation = rect.getAttributeSourceLocation("fill");
  ASSERT_TRUE(fillLocation.has_value());
  EXPECT_EQ(SourceText(openingTagDocument, fillLocation->valueRange), "red");

  ParseResult<XMLDocument> maybePcdata = XMLIncrementalParser::ParsePcdata("a&amp;b");
  ASSERT_THAT(maybePcdata, NoParseError());
  XMLNode data = maybePcdata.result().root().firstChild()->firstChild().value();
  EXPECT_EQ(data.type(), XMLNode::Type::Data);
  EXPECT_THAT(data.value(), testing::Optional(RcString("a&b")));

  ParseResult<XMLDocument> maybeTextLike =
      XMLIncrementalParser::ParseTextLikeNode("<![CDATA[x<y]]>");
  ASSERT_THAT(maybeTextLike, NoParseError());
  XMLDocument textLikeDocument = std::move(maybeTextLike.result());
  XMLNode cdata = textLikeDocument.root().firstChild().value();
  EXPECT_EQ(cdata.type(), XMLNode::Type::CData);
  EXPECT_THAT(cdata.value(), testing::Optional(RcString("x<y")));
  ASSERT_TRUE(cdata.getValueLocation().has_value());
  EXPECT_EQ(SourceText(textLikeDocument, *cdata.getValueLocation()), "x<y");

  ParseResult<XMLDocument> maybeElement =
      XMLIncrementalParser::ParseElement(R"(<g><rect id="r"/></g>)");
  ASSERT_THAT(maybeElement, NoParseError());
  XMLNode group = maybeElement.result().root().firstChild().value();
  EXPECT_EQ(group.tagName(), XMLQualifiedName("g"));
  EXPECT_THAT(group.firstChild()->getAttribute("id"), testing::Optional(RcString("r")));
}

TEST_F(XMLParserTests, NodeAtSourceOffsetFollowsSourceStoreEdits) {
  constexpr std::string_view kXml = R"(<svg><g><rect id="r"/></g><circle/></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  std::optional<XMLNode> maybeSvg = document.root().firstChild();
  ASSERT_TRUE(maybeSvg.has_value());
  std::optional<XMLNode> maybeG = maybeSvg->firstChild();
  ASSERT_TRUE(maybeG.has_value());
  std::optional<XMLNode> maybeRect = maybeG->firstChild();
  ASSERT_TRUE(maybeRect.has_value());
  std::optional<XMLNode> maybeCircle = maybeG->nextSibling();
  ASSERT_TRUE(maybeCircle.has_value());

  std::optional<SourceRange> originalRectLocation = maybeRect->getNodeLocation();
  ASSERT_TRUE(originalRectLocation.has_value());
  ASSERT_TRUE(originalRectLocation->start.offset.has_value());

  XMLSourceStore* sourceStore = document.sourceStore();
  ASSERT_NE(sourceStore, nullptr);
  ASSERT_TRUE(sourceStore->replace(*originalRectLocation->start.offset, 0, "<line/>").has_value());

  std::optional<SourceRange> updatedRectLocation = maybeRect->getNodeLocation();
  ASSERT_TRUE(updatedRectLocation.has_value());
  ASSERT_TRUE(updatedRectLocation->start.offset.has_value());
  ASSERT_TRUE(updatedRectLocation->end.offset.has_value());

  EXPECT_EQ(document.nodeAtSourceOffset(*updatedRectLocation->start.offset), maybeRect);
  EXPECT_EQ(document.nodeAtSourceOffset(*updatedRectLocation->end.offset - 1), maybeRect);

  const std::size_t circleOffset = document.source().find("<circle");
  ASSERT_NE(circleOffset, std::string_view::npos);
  EXPECT_EQ(document.nodeAtSourceOffset(circleOffset), maybeCircle);
  EXPECT_EQ(document.nodeAtSourceOffset(document.source().size()), std::nullopt);
}

TEST_F(XMLParserTests, ApplySourceEditUpdatesAttributeValue) {
  constexpr std::string_view kXml = R"(<svg><rect fill="red"/></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode rect = document.root().firstChild()->firstChild().value();

  const std::size_t valueOffset = document.source().find("red");
  ASSERT_NE(valueOffset, std::string_view::npos);

  ApplySourceEditResult result = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 3)},
      .replacement = "blue",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::AttributeValue);
  EXPECT_THAT(result.sourceDeltas, ElementsAre(XMLSourceDelta{
                                       .offset = valueOffset,
                                       .removedLength = 3,
                                       .insertedLength = 4,
                                       .sourceVersion = 1,
                                   }));
  EXPECT_THAT(result.mutations,
              ElementsAre(AttributeMutationIs(
                  XMLMutation::Kind::AttributeSet, rect, XMLQualifiedName("fill"),
                  testing::Optional(RcString("blue")), ReparseScope::AttributeValue)));
  EXPECT_EQ(result.diagnostic, std::nullopt);

  EXPECT_EQ(document.source(), R"(<svg><rect fill="blue"/></svg>)");
  EXPECT_THAT(rect.getAttribute("fill"), Eq("blue"));
}

TEST_F(XMLParserTests, ApplySourceEditReparsesAttributeEntities) {
  constexpr std::string_view kXml = R"(<svg><rect data-label="red"/></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode rect = document.root().firstChild()->firstChild().value();

  const std::size_t valueOffset = document.source().find("red");
  ASSERT_NE(valueOffset, std::string_view::npos);

  ApplySourceEditResult result = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 3)},
      .replacement = "a&amp;b",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::AttributeValue);
  EXPECT_EQ(result.diagnostic, std::nullopt);
  EXPECT_EQ(document.source(), R"(<svg><rect data-label="a&amp;b"/></svg>)");
  EXPECT_THAT(rect.getAttribute("data-label"), Eq("a&b"));
}

TEST_F(XMLParserTests, ApplySourceEditMarksAttributeEntityDirtyAndRecovers) {
  constexpr std::string_view kXml = R"(<svg><rect data-label="a&#65;b"/></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode rect = document.root().firstChild()->firstChild().value();

  const std::size_t entityOffset = document.source().find("&#65;");
  ASSERT_NE(entityOffset, std::string_view::npos);

  ApplySourceEditResult dirtyResult = document.applySourceEdit(XMLEditIntent{
      .range =
          SourceRange{FileOffset::Offset(entityOffset + 4), FileOffset::Offset(entityOffset + 5)},
      .replacement = "",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(dirtyResult.applied);
  EXPECT_EQ(dirtyResult.scope, ReparseScope::AttributeValue);
  ASSERT_TRUE(dirtyResult.diagnostic.has_value());
  EXPECT_THAT(dirtyResult.mutations,
              ElementsAre(SourceDiagnosticMutationIs(rect, Eq(dirtyResult.diagnostic),
                                                     ReparseScope::AttributeValue)));
  EXPECT_EQ(SourceText(document, dirtyResult.diagnostic->range), "a&#65b");
  EXPECT_EQ(document.source(), R"(<svg><rect data-label="a&#65b"/></svg>)");
  EXPECT_THAT(rect.getAttribute("data-label"), Eq("aAb"));

  ApplySourceEditResult recoveryResult = document.applySourceEdit(XMLEditIntent{
      .range =
          SourceRange{FileOffset::Offset(entityOffset + 4), FileOffset::Offset(entityOffset + 4)},
      .replacement = ";",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(recoveryResult.applied);
  EXPECT_EQ(recoveryResult.scope, ReparseScope::AttributeValue);
  EXPECT_EQ(recoveryResult.diagnostic, std::nullopt);
  EXPECT_THAT(
      recoveryResult.mutations,
      ElementsAre(
          AttributeMutationIs(XMLMutation::Kind::AttributeSet, rect, XMLQualifiedName("data-label"),
                              testing::Optional(RcString("aAb")), ReparseScope::AttributeValue),
          SourceDiagnosticMutationIs(rect, Eq(std::nullopt), ReparseScope::AttributeValue)));
  EXPECT_EQ(document.source(), kXml);
  EXPECT_THAT(rect.getAttribute("data-label"), Eq("aAb"));
}

TEST_F(XMLParserTests, SetAttributeUpdatesSourceThroughDocument) {
  constexpr std::string_view kXml = R"(<svg><rect fill='red' stroke="black"/></svg>)";
  constexpr std::string_view kEscapedValue = R"(Tom&apos;s &amp; "q")";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode rect = document.root().firstChild()->firstChild().value();

  const std::size_t valueOffset = document.source().find("red");
  ASSERT_NE(valueOffset, std::string_view::npos);

  ApplySourceEditResult result = document.setAttribute(rect, "fill", R"(Tom's & "q")");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::AttributeValue);
  EXPECT_THAT(result.sourceDeltas, ElementsAre(XMLSourceDelta{
                                       .offset = valueOffset,
                                       .removedLength = 3,
                                       .insertedLength = kEscapedValue.size(),
                                       .sourceVersion = 1,
                                   }));
  EXPECT_THAT(result.mutations,
              ElementsAre(AttributeMutationIs(
                  XMLMutation::Kind::AttributeSet, rect, XMLQualifiedName("fill"),
                  testing::Optional(RcString(R"(Tom's & "q")")), ReparseScope::AttributeValue)));
  EXPECT_EQ(result.diagnostic, std::nullopt);

  EXPECT_EQ(document.source(), R"(<svg><rect fill='Tom&apos;s &amp; "q"' stroke="black"/></svg>)");
  EXPECT_THAT(rect.getAttribute("fill"), Eq(R"(Tom's & "q")"));
}

TEST_F(XMLParserTests, SetAttributeInsertsMissingSourceAttributeThroughDocument) {
  constexpr std::string_view kXml = R"(<svg><rect /></svg>)";
  constexpr std::string_view kInsertedSource = R"(fill="blue &amp; white")";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode rect = document.root().firstChild()->firstChild().value();

  const std::size_t insertionOffset = document.source().find("/");
  ASSERT_NE(insertionOffset, std::string_view::npos);

  ApplySourceEditResult result = document.setAttribute(rect, "fill", "blue & white");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_THAT(result.sourceDeltas, ElementsAre(XMLSourceDelta{
                                       .offset = insertionOffset,
                                       .removedLength = 0,
                                       .insertedLength = kInsertedSource.size(),
                                       .sourceVersion = 1,
                                   }));
  EXPECT_THAT(result.mutations,
              ElementsAre(AttributeMutationIs(
                  XMLMutation::Kind::AttributeSet, rect, XMLQualifiedName("fill"),
                  testing::Optional(RcString("blue & white")), ReparseScope::OpeningTag)));
  EXPECT_EQ(result.diagnostic, std::nullopt);

  EXPECT_EQ(document.source(), R"(<svg><rect fill="blue &amp; white"/></svg>)");
  EXPECT_THAT(rect.getAttribute("fill"), Eq("blue & white"));

  std::optional<XMLAttributeSourceLocation> fillLocation = rect.getAttributeSourceLocation("fill");
  ASSERT_TRUE(fillLocation.has_value());
  EXPECT_EQ(fillLocation->quote, '"');
  EXPECT_EQ(document.source().substr(
                *fillLocation->fullRange.start.offset,
                *fillLocation->fullRange.end.offset - *fillLocation->fullRange.start.offset),
            R"(fill="blue &amp; white")");
  EXPECT_EQ(document.source().substr(
                *fillLocation->valueRange.start.offset,
                *fillLocation->valueRange.end.offset - *fillLocation->valueRange.start.offset),
            "blue &amp; white");

  std::optional<XMLAttributeAtSourceOffset> attributeAtOffset =
      document.attributeAtSourceOffset(*fillLocation->valueRange.start.offset + 1);
  ASSERT_TRUE(attributeAtOffset.has_value());
  EXPECT_EQ(attributeAtOffset->node, rect);
  EXPECT_EQ(attributeAtOffset->name, XMLQualifiedName("fill"));
  EXPECT_EQ(attributeAtOffset->valueLocation, fillLocation->valueRange);
}

TEST_F(XMLParserTests, RemoveAttributeUpdatesSourceThroughDocument) {
  constexpr std::string_view kXml = R"(<svg><rect fill="red" stroke="blue"/></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode rect = document.root().firstChild()->firstChild().value();
  std::optional<SourceRange> fillLocation = rect.getAttributeLocation(document.source(), "fill");
  ASSERT_TRUE(fillLocation.has_value());
  ASSERT_TRUE(fillLocation->start.offset.has_value());
  ASSERT_TRUE(fillLocation->end.offset.has_value());

  ApplySourceEditResult result = document.removeAttribute(rect, "fill");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_THAT(result.sourceDeltas,
              ElementsAre(XMLSourceDelta{
                  .offset = *fillLocation->start.offset - 1,
                  .removedLength = *fillLocation->end.offset - *fillLocation->start.offset + 1,
                  .insertedLength = 0,
                  .sourceVersion = 1,
              }));
  EXPECT_THAT(result.mutations,
              ElementsAre(AttributeMutationIs(XMLMutation::Kind::AttributeRemoved, rect,
                                              XMLQualifiedName("fill"), Eq(std::nullopt),
                                              ReparseScope::OpeningTag)));
  EXPECT_EQ(result.diagnostic, std::nullopt);

  EXPECT_EQ(document.source(), R"(<svg><rect stroke="blue"/></svg>)");
  EXPECT_THAT(rect.getAttribute("fill"), Eq(std::nullopt));
  EXPECT_THAT(rect.getAttribute("stroke"), Eq("blue"));
}

TEST_F(XMLParserTests, ApplySourceEditRejectsStaleSourceVersion) {
  constexpr std::string_view kXml = R"(<svg><rect fill="red"/></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode rect = document.root().firstChild()->firstChild().value();

  const std::size_t valueOffset = document.source().find("red");
  ASSERT_NE(valueOffset, std::string_view::npos);

  ApplySourceEditResult result = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 3)},
      .replacement = "blue",
      .sourceVersion = document.sourceVersion() + 1,
  });

  EXPECT_FALSE(result.applied);
  EXPECT_TRUE(result.sourceDeltas.empty());
  EXPECT_TRUE(result.mutations.empty());
  ASSERT_TRUE(result.diagnostic.has_value());
  EXPECT_EQ(result.diagnostic->reason, "Source version mismatch");
  EXPECT_EQ(document.source(), kXml);
  EXPECT_THAT(rect.getAttribute("fill"), Eq("red"));
}

TEST_F(XMLParserTests, ApplySourceEditMarksOpeningTagDirtyWhenDeletingAttributeQuote) {
  constexpr std::string_view kXml = R"(<svg><rect fill="red"/></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode rect = document.root().firstChild()->firstChild().value();

  const std::size_t quoteOffset = document.source().find(R"("/>)");
  ASSERT_NE(quoteOffset, std::string_view::npos);

  ApplySourceEditResult result = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(quoteOffset), FileOffset::Offset(quoteOffset + 1)},
      .replacement = "",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  ASSERT_TRUE(result.diagnostic.has_value());
  EXPECT_THAT(result.mutations, ElementsAre(SourceDiagnosticMutationIs(rect, Eq(result.diagnostic),
                                                                       ReparseScope::OpeningTag)));
  EXPECT_EQ(SourceText(document, result.diagnostic->range), R"(<rect fill="red/>)");
  EXPECT_EQ(document.source(), R"(<svg><rect fill="red/></svg>)");
  EXPECT_THAT(rect.getAttribute("fill"), Eq("red"));
}

TEST_F(XMLParserTests, ApplySourceEditMarksPartialOpeningTagDirty) {
  constexpr std::string_view kXml = R"(<svg><rect fill="red"/></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode rect = document.root().firstChild()->firstChild().value();

  const std::size_t closeOffset = document.source().find(R"(></svg>)");
  ASSERT_NE(closeOffset, std::string_view::npos);

  ApplySourceEditResult result = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(closeOffset), FileOffset::Offset(closeOffset + 1)},
      .replacement = "",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  ASSERT_TRUE(result.diagnostic.has_value());
  EXPECT_THAT(result.mutations, ElementsAre(SourceDiagnosticMutationIs(rect, Eq(result.diagnostic),
                                                                       ReparseScope::OpeningTag)));
  EXPECT_EQ(SourceText(document, result.diagnostic->range), R"(<rect fill="red"/)");
  EXPECT_EQ(document.source(), R"(<svg><rect fill="red"/</svg>)");
  EXPECT_THAT(rect.getAttribute("fill"), Eq("red"));
}

TEST_F(XMLParserTests, ApplySourceEditRestoresDirtyOpeningTagWithoutDocumentFallback) {
  constexpr std::string_view kXml = R"(<svg><rect fill="red"/></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode rect = document.root().firstChild()->firstChild().value();

  const std::size_t quoteOffset = document.source().find(R"("/>)");
  ASSERT_NE(quoteOffset, std::string_view::npos);

  ApplySourceEditResult dirtyResult = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(quoteOffset), FileOffset::Offset(quoteOffset + 1)},
      .replacement = "",
      .sourceVersion = document.sourceVersion(),
  });
  ASSERT_TRUE(dirtyResult.applied);
  ASSERT_TRUE(dirtyResult.diagnostic.has_value());
  ASSERT_EQ(document.source(), R"(<svg><rect fill="red/></svg>)");

  const std::size_t restoreOffset = document.source().find("/>");
  ASSERT_NE(restoreOffset, std::string_view::npos);

  ApplySourceEditResult recoveryResult = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(restoreOffset), FileOffset::Offset(restoreOffset)},
      .replacement = R"(")",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(recoveryResult.applied);
  EXPECT_EQ(recoveryResult.scope, ReparseScope::OpeningTag);
  EXPECT_EQ(recoveryResult.diagnostic, std::nullopt);
  EXPECT_THAT(recoveryResult.mutations, ElementsAre(SourceDiagnosticMutationIs(
                                            rect, Eq(std::nullopt), ReparseScope::OpeningTag)));
  EXPECT_EQ(document.source(), kXml);
  EXPECT_THAT(rect.getAttribute("fill"), Eq("red"));
}

TEST_F(XMLParserTests, ApplySourceEditOpeningTagAddsAttribute) {
  constexpr std::string_view kXml = R"(<svg><rect fill="red"/></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode rect = document.root().firstChild()->firstChild().value();

  const std::size_t insertOffset = document.source().find("/>");
  ASSERT_NE(insertOffset, std::string_view::npos);

  ApplySourceEditResult result = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(insertOffset), FileOffset::Offset(insertOffset)},
      .replacement = R"( stroke="blue")",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_EQ(result.diagnostic, std::nullopt);
  EXPECT_THAT(result.mutations,
              ElementsAre(AttributeMutationIs(
                  XMLMutation::Kind::AttributeSet, rect, XMLQualifiedName("stroke"),
                  testing::Optional(RcString("blue")), ReparseScope::OpeningTag)));

  EXPECT_EQ(document.source(), R"(<svg><rect fill="red" stroke="blue"/></svg>)");
  EXPECT_THAT(rect.getAttribute("fill"), Eq("red"));
  EXPECT_THAT(rect.getAttribute("stroke"), Eq("blue"));
}

TEST_F(XMLParserTests, ApplySourceEditOpeningTagRemovesAttribute) {
  constexpr std::string_view kXml = R"(<svg><rect fill="red" stroke="blue"/></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode rect = document.root().firstChild()->firstChild().value();
  std::optional<SourceRange> fillLocation = rect.getAttributeLocation(document.source(), "fill");
  ASSERT_TRUE(fillLocation.has_value());

  ApplySourceEditResult result = document.applySourceEdit(XMLEditIntent{
      .range = *fillLocation,
      .replacement = "",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_EQ(result.diagnostic, std::nullopt);
  EXPECT_THAT(result.mutations,
              ElementsAre(AttributeMutationIs(XMLMutation::Kind::AttributeRemoved, rect,
                                              XMLQualifiedName("fill"), Eq(std::nullopt),
                                              ReparseScope::OpeningTag)));

  EXPECT_EQ(document.source(), R"(<svg><rect  stroke="blue"/></svg>)");
  EXPECT_THAT(rect.getAttribute("fill"), Eq(std::nullopt));
  EXPECT_THAT(rect.getAttribute("stroke"), Eq("blue"));
}

TEST_F(XMLParserTests, RemoveNodeUpdatesSourceThroughDocument) {
  constexpr std::string_view kXml = R"(<svg><g id="target"><rect/></g><circle/></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode svg = document.root().firstChild().value();
  XMLNode group = svg.firstChild().value();
  std::optional<SourceRange> groupLocation = group.getNodeLocation();
  ASSERT_TRUE(groupLocation.has_value());
  ASSERT_TRUE(groupLocation->start.offset.has_value());
  ASSERT_TRUE(groupLocation->end.offset.has_value());

  ApplySourceEditResult result = document.removeNode(group);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_THAT(result.sourceDeltas,
              ElementsAre(XMLSourceDelta{
                  .offset = *groupLocation->start.offset,
                  .removedLength = *groupLocation->end.offset - *groupLocation->start.offset,
                  .insertedLength = 0,
                  .sourceVersion = 1,
              }));
  EXPECT_THAT(result.mutations, ElementsAre(MutationIs(XMLMutation::Kind::NodeRemoved, group,
                                                       ReparseScope::ElementSubtree)));
  EXPECT_EQ(result.diagnostic, std::nullopt);

  EXPECT_EQ(document.source(), R"(<svg><circle/></svg>)");
  ASSERT_TRUE(svg.firstChild().has_value());
  EXPECT_EQ(svg.firstChild()->tagName(), XMLQualifiedNameRef("circle"));
  EXPECT_EQ(group.parentElement(), std::nullopt);
}

TEST_F(XMLParserTests, RemoveNodeInvalidatesRemovedSubtreeSourceLocations) {
  constexpr std::string_view kXml = R"(<svg><g id="target"><rect id="child"/></g><circle/></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode svg = document.root().firstChild().value();
  XMLNode group = svg.firstChild().value();
  XMLNode child = group.firstChild().value();

  ASSERT_TRUE(group.getNodeLocation().has_value());
  ASSERT_TRUE(child.getNodeLocation().has_value());

  ApplySourceEditResult result = document.removeNode(group);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.diagnostic, std::nullopt);
  EXPECT_EQ(group.parentElement(), std::nullopt);
  EXPECT_EQ(group.getNodeLocation(), std::nullopt);
  EXPECT_EQ(child.getNodeLocation(), std::nullopt);
  EXPECT_EQ(group.sourceStartOffset(), std::nullopt);
  EXPECT_EQ(group.sourceEndOffset(), std::nullopt);
}

TEST_F(XMLParserTests, InsertNodeUpdatesSourceThroughDocument) {
  constexpr std::string_view kXml = R"(<svg><g id="parent"></g><circle/></svg>)";
  constexpr std::string_view kInserted = R"(<rect id="inserted"/>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode svg = document.root().firstChild().value();
  XMLNode group = svg.firstChild().value();
  XMLNode rect = XMLNode::CreateElementNode(document, "rect");
  rect.setAttribute("id", "inserted");

  const std::size_t insertionOffset = kXml.find("</g>");
  ASSERT_NE(insertionOffset, std::string_view::npos);

  ApplySourceEditResult result = document.insertNode(group, rect);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_THAT(result.sourceDeltas, ElementsAre(XMLSourceDelta{
                                       .offset = insertionOffset,
                                       .removedLength = 0,
                                       .insertedLength = kInserted.size(),
                                       .sourceVersion = 1,
                                   }));
  EXPECT_THAT(result.mutations, ElementsAre(MutationIs(XMLMutation::Kind::NodeInserted, rect,
                                                       ReparseScope::ElementSubtree)));
  EXPECT_EQ(result.diagnostic, std::nullopt);

  EXPECT_EQ(document.source(), R"(<svg><g id="parent"><rect id="inserted"/></g><circle/></svg>)");
  ASSERT_TRUE(group.firstChild().has_value());
  EXPECT_EQ(*group.firstChild(), rect);
  EXPECT_THAT(rect.getAttribute("id"), Eq("inserted"));

  std::optional<SourceRange> rectLocation = rect.getNodeLocation();
  ASSERT_TRUE(rectLocation.has_value());
  ASSERT_TRUE(rectLocation->start.offset.has_value());
  ASSERT_TRUE(rectLocation->end.offset.has_value());
  EXPECT_EQ(*rectLocation->start.offset, insertionOffset);
  EXPECT_EQ(*rectLocation->end.offset, insertionOffset + kInserted.size());
  EXPECT_TRUE(rect.getAttributeSourceLocation("id").has_value());
}

TEST_F(XMLParserTests, InsertNodeBeforeReferenceUpdatesSourceThroughDocument) {
  constexpr std::string_view kXml = R"(<svg><g><circle id="sibling"/></g></svg>)";
  constexpr std::string_view kInserted = R"(<rect id="inserted"/>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode svg = document.root().firstChild().value();
  XMLNode group = svg.firstChild().value();
  XMLNode sibling = group.firstChild().value();
  XMLNode rect = XMLNode::CreateElementNode(document, "rect");
  rect.setAttribute("id", "inserted");

  const std::size_t insertionOffset = kXml.find("<circle");
  ASSERT_NE(insertionOffset, std::string_view::npos);

  ApplySourceEditResult result = document.insertNode(group, rect, sibling);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_THAT(result.sourceDeltas, ElementsAre(XMLSourceDelta{
                                       .offset = insertionOffset,
                                       .removedLength = 0,
                                       .insertedLength = kInserted.size(),
                                       .sourceVersion = 1,
                                   }));
  EXPECT_THAT(result.mutations, ElementsAre(MutationIs(XMLMutation::Kind::NodeInserted, rect,
                                                       ReparseScope::ElementSubtree)));

  EXPECT_EQ(document.source(), R"(<svg><g><rect id="inserted"/><circle id="sibling"/></g></svg>)");
  ASSERT_TRUE(group.firstChild().has_value());
  EXPECT_EQ(*group.firstChild(), rect);
  ASSERT_TRUE(rect.nextSibling().has_value());
  EXPECT_EQ(*rect.nextSibling(), sibling);
}

TEST_F(XMLParserTests, InsertNodeRecoversStaleClosingTagLocation) {
  constexpr std::string_view kXml = R"(<svg><g id="parent"></g></svg>)";
  constexpr std::string_view kInserted = R"(<rect id="inserted"/>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode svg = document.root().firstChild().value();
  XMLNode rect = XMLNode::CreateElementNode(document, "rect");
  rect.setAttribute("id", "inserted");

  svg.setClosingTagLocation(
      SourceRange{FileOffset::Offset(kXml.size()), FileOffset::Offset(kXml.size())});

  ApplySourceEditResult result = document.insertNode(svg, rect);

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.sourceDeltas, ElementsAre(XMLSourceDelta{
                                       .offset = kXml.find("</svg>"),
                                       .removedLength = 0,
                                       .insertedLength = kInserted.size(),
                                       .sourceVersion = 1,
                                   }));
  EXPECT_EQ(document.source(), R"(<svg><g id="parent"></g><rect id="inserted"/></svg>)");
  EXPECT_LT(document.source().find(R"(<rect id="inserted"/>)"), document.source().find("</svg>"));
}

TEST_F(XMLParserTests, InsertNodeExpandsSelfClosingParentThroughDocument) {
  constexpr std::string_view kXml = R"(<svg><g id="parent"/><circle/></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode svg = document.root().firstChild().value();
  XMLNode group = svg.firstChild().value();
  XMLNode rect = XMLNode::CreateElementNode(document, "rect");
  rect.setAttribute("id", "inserted");

  ApplySourceEditResult result = document.insertNode(group, rect);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_THAT(result.sourceDeltas,
              ElementsAre(XMLSourceDelta{
                  .offset = kXml.find("/>"),
                  .removedLength = 2,
                  .insertedLength = std::string_view(R"(><rect id="inserted"/></g>)").size(),
                  .sourceVersion = 1,
              }));
  EXPECT_THAT(result.mutations, ElementsAre(MutationIs(XMLMutation::Kind::NodeInserted, rect,
                                                       ReparseScope::ElementSubtree)));
  EXPECT_EQ(result.diagnostic, std::nullopt);

  EXPECT_EQ(document.source(), R"(<svg><g id="parent"><rect id="inserted"/></g><circle/></svg>)");
  ASSERT_TRUE(group.firstChild().has_value());
  EXPECT_EQ(*group.firstChild(), rect);
  EXPECT_THAT(rect.getAttribute("id"), Eq("inserted"));

  std::optional<SourceRange> groupOpening = group.getOpeningTagLocation();
  std::optional<SourceRange> groupClosing = group.getClosingTagLocation();
  ASSERT_TRUE(groupOpening.has_value());
  ASSERT_TRUE(groupClosing.has_value());
  EXPECT_EQ(SourceText(document, *groupOpening), R"(<g id="parent">)");
  EXPECT_EQ(SourceText(document, *groupClosing), "</g>");

  std::optional<SourceRange> rectLocation = rect.getNodeLocation();
  ASSERT_TRUE(rectLocation.has_value());
  EXPECT_EQ(SourceText(document, *rectLocation), R"(<rect id="inserted"/>)");
}

TEST_F(XMLParserTests, InsertNodeMovesExistingSourceBackedChildToEnd) {
  constexpr std::string_view kXml = R"(<svg><g><rect id="a"/><circle id="b"/></g></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode svg = document.root().firstChild().value();
  XMLNode group = svg.firstChild().value();
  XMLNode rect = group.firstChild().value();
  XMLNode circle = rect.nextSibling().value();

  ApplySourceEditResult result = document.insertNode(group, rect);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  ASSERT_EQ(result.sourceDeltas.size(), 2u);
  EXPECT_THAT(result.mutations, ElementsAre(MutationKindIs(XMLMutation::Kind::NodeRemoved),
                                            MutationIs(XMLMutation::Kind::NodeInserted, rect,
                                                       ReparseScope::ElementSubtree)));
  EXPECT_EQ(result.diagnostic, std::nullopt);

  EXPECT_EQ(document.source(), R"(<svg><g><circle id="b"/><rect id="a"/></g></svg>)");
  ASSERT_TRUE(group.firstChild().has_value());
  EXPECT_EQ(*group.firstChild(), circle);
  ASSERT_TRUE(circle.nextSibling().has_value());
  EXPECT_EQ(*circle.nextSibling(), rect);
  EXPECT_EQ(rect.parentElement(), group);
  EXPECT_EQ(SourceText(document, *rect.getNodeLocation()), R"(<rect id="a"/>)");
}

TEST_F(XMLParserTests, InsertNodeMovesExistingSourceBackedChildBeforeReference) {
  constexpr std::string_view kXml = R"(<svg><g><rect id="a"/><circle id="b"/></g></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode svg = document.root().firstChild().value();
  XMLNode group = svg.firstChild().value();
  XMLNode rect = group.firstChild().value();
  XMLNode circle = rect.nextSibling().value();

  ApplySourceEditResult result = document.insertNode(group, circle, rect);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  ASSERT_EQ(result.sourceDeltas.size(), 2u);
  EXPECT_THAT(result.mutations, ElementsAre(MutationKindIs(XMLMutation::Kind::NodeRemoved),
                                            MutationIs(XMLMutation::Kind::NodeInserted, circle,
                                                       ReparseScope::ElementSubtree)));
  EXPECT_EQ(result.diagnostic, std::nullopt);

  EXPECT_EQ(document.source(), R"(<svg><g><circle id="b"/><rect id="a"/></g></svg>)");
  ASSERT_TRUE(group.firstChild().has_value());
  EXPECT_EQ(*group.firstChild(), circle);
  ASSERT_TRUE(circle.nextSibling().has_value());
  EXPECT_EQ(*circle.nextSibling(), rect);
  EXPECT_EQ(circle.parentElement(), group);
  EXPECT_EQ(SourceText(document, *circle.getNodeLocation()), R"(<circle id="b"/>)");
}

TEST_F(XMLParserTests, InsertNodeMovesExistingSourceBackedChildIntoLaterSelfClosingParent) {
  constexpr std::string_view kXml = R"(<svg><rect id="a"/><g id="target"/></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode svg = document.root().firstChild().value();
  XMLNode rect = svg.firstChild().value();
  XMLNode group = rect.nextSibling().value();

  ApplySourceEditResult result = document.insertNode(group, rect);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  ASSERT_EQ(result.sourceDeltas.size(), 2u);
  EXPECT_THAT(result.mutations, ElementsAre(MutationKindIs(XMLMutation::Kind::NodeRemoved),
                                            MutationKindIs(XMLMutation::Kind::NodeInserted)));
  EXPECT_EQ(result.diagnostic, std::nullopt);

  EXPECT_EQ(document.source(), R"(<svg><g id="target"><rect id="a"/></g></svg>)");
  EXPECT_EQ(svg.firstChild(), group);
  EXPECT_EQ(group.firstChild(), rect);

  std::optional<SourceRange> groupOpening = group.getOpeningTagLocation();
  std::optional<SourceRange> groupClosing = group.getClosingTagLocation();
  ASSERT_TRUE(groupOpening.has_value());
  ASSERT_TRUE(groupClosing.has_value());
  EXPECT_EQ(SourceText(document, *groupOpening), R"(<g id="target">)");
  EXPECT_EQ(SourceText(document, *groupClosing), "</g>");
  EXPECT_EQ(SourceText(document, *rect.getNodeLocation()), R"(<rect id="a"/>)");
}

TEST_F(XMLParserTests, InsertNodeMovesExistingSourceBackedChildIntoEarlierSelfClosingParent) {
  constexpr std::string_view kXml = R"(<svg><g id="target"/><rect id="a"/></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode svg = document.root().firstChild().value();
  XMLNode group = svg.firstChild().value();
  XMLNode rect = group.nextSibling().value();

  ApplySourceEditResult result = document.insertNode(group, rect);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  ASSERT_EQ(result.sourceDeltas.size(), 2u);
  EXPECT_THAT(result.mutations, ElementsAre(MutationKindIs(XMLMutation::Kind::NodeRemoved),
                                            MutationKindIs(XMLMutation::Kind::NodeInserted)));
  EXPECT_EQ(result.diagnostic, std::nullopt);

  EXPECT_EQ(document.source(), R"(<svg><g id="target"><rect id="a"/></g></svg>)");
  EXPECT_EQ(svg.firstChild(), group);
  EXPECT_EQ(group.firstChild(), rect);

  std::optional<SourceRange> groupOpening = group.getOpeningTagLocation();
  std::optional<SourceRange> groupClosing = group.getClosingTagLocation();
  ASSERT_TRUE(groupOpening.has_value());
  ASSERT_TRUE(groupClosing.has_value());
  EXPECT_EQ(SourceText(document, *groupOpening), R"(<g id="target">)");
  EXPECT_EQ(SourceText(document, *groupClosing), "</g>");
  EXPECT_EQ(SourceText(document, *rect.getNodeLocation()), R"(<rect id="a"/>)");
}

TEST_F(XMLParserTests, ApplySourceEditElementSubtreeInsertsChildWithoutDocumentFallback) {
  constexpr std::string_view kXml =
      R"(<svg><g id="layer"><rect id="r"/></g><circle id="outside"/></svg>)";
  constexpr std::string_view kInserted = R"(<circle id="c"/>)";
  constexpr std::string_view kExpected =
      R"(<svg><g id="layer"><rect id="r"/><circle id="c"/></g><circle id="outside"/></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode svg = document.root().firstChild().value();
  XMLNode group = svg.firstChild().value();
  const Entity groupEntity = group.entityHandle().entity();

  const std::size_t insertOffset = document.source().find("</g>");
  ASSERT_NE(insertOffset, std::string_view::npos);

  ApplySourceEditResult result = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(insertOffset), FileOffset::Offset(insertOffset)},
      .replacement = kInserted,
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_EQ(result.diagnostic, std::nullopt);
  EXPECT_THAT(
      result.mutations,
      ElementsAre(
          MutationIs(XMLMutation::Kind::SubtreeReplaced, group, ReparseScope::ElementSubtree),
          AllOf(MutationKindIs(XMLMutation::Kind::NodeInserted),
                Field("scope", &XMLMutation::scope, ReparseScope::ElementSubtree))));
  EXPECT_EQ(document.source(), kExpected);
  EXPECT_EQ(group.entityHandle().entity(), groupEntity);

  ASSERT_TRUE(group.firstChild().has_value());
  XMLNode rect = *group.firstChild();
  EXPECT_EQ(rect.tagName(), XMLQualifiedNameRef("rect"));
  ASSERT_TRUE(rect.nextSibling().has_value());
  XMLNode inserted = *rect.nextSibling();
  EXPECT_EQ(inserted.tagName(), XMLQualifiedNameRef("circle"));
  EXPECT_THAT(inserted.getAttribute("id"), testing::Optional(RcString("c")));
  EXPECT_EQ(inserted.nextSibling(), std::nullopt);

  ASSERT_TRUE(inserted.getNodeLocation().has_value());
  EXPECT_EQ(document.nodeAtSourceOffset(insertOffset + 1), inserted);
}

TEST_F(XMLParserTests, ApplySourceEditElementSubtreePreservesMatchedChildIdentity) {
  constexpr std::string_view kXml =
      R"(<svg><g id="layer"><rect id="r"/></g><circle id="outside"/></svg>)";
  constexpr std::string_view kInserted = R"(<circle id="c"/>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode svg = document.root().firstChild().value();
  XMLNode group = svg.firstChild().value();
  XMLNode rect = group.firstChild().value();
  const Entity rectEntity = rect.entityHandle().entity();

  const std::size_t insertOffset = document.source().find("</g>");
  ASSERT_NE(insertOffset, std::string_view::npos);

  ApplySourceEditResult result = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(insertOffset), FileOffset::Offset(insertOffset)},
      .replacement = kInserted,
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_EQ(result.diagnostic, std::nullopt);

  ASSERT_TRUE(group.firstChild().has_value());
  XMLNode updatedRect = *group.firstChild();
  EXPECT_EQ(updatedRect.tagName(), XMLQualifiedNameRef("rect"));
  EXPECT_EQ(updatedRect.entityHandle().entity(), rectEntity);
  EXPECT_THAT(updatedRect.getAttribute("id"), testing::Optional(RcString("r")));
}

TEST_F(XMLParserTests, ApplySourceEditElementSubtreeDeletionInvalidatesRemovedChildLocation) {
  constexpr std::string_view kXml =
      R"(<svg><g id="layer"><rect id="r"/><circle id="c"/></g></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode group = document.root().firstChild()->firstChild().value();
  XMLNode rect = group.firstChild().value();
  XMLNode circle = rect.nextSibling().value();

  std::optional<SourceRange> rectLocation = rect.getNodeLocation();
  ASSERT_TRUE(rectLocation.has_value());
  ASSERT_TRUE(rectLocation->start.offset.has_value());
  ASSERT_TRUE(rectLocation->end.offset.has_value());
  ASSERT_TRUE(circle.getNodeLocation().has_value());

  ApplySourceEditResult result = document.applySourceEdit(XMLEditIntent{
      .range = *rectLocation,
      .replacement = "",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_EQ(result.diagnostic, std::nullopt);
  EXPECT_THAT(
      result.mutations,
      ElementsAre(
          MutationIs(XMLMutation::Kind::SubtreeReplaced, group, ReparseScope::ElementSubtree),
          MutationIs(XMLMutation::Kind::NodeRemoved, rect, ReparseScope::ElementSubtree)));
  EXPECT_EQ(rect.parentElement(), std::nullopt);
  EXPECT_EQ(rect.getNodeLocation(), std::nullopt);
  ASSERT_TRUE(group.firstChild().has_value());
  EXPECT_EQ(*group.firstChild(), circle);
  EXPECT_TRUE(circle.getNodeLocation().has_value());
}

TEST_F(XMLParserTests, ApplySourceEditOpeningTagRenamesAttribute) {
  constexpr std::string_view kXml = R"(<svg><rect fill="red"/></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode rect = document.root().firstChild()->firstChild().value();

  const std::size_t nameOffset = document.source().find("fill");
  ASSERT_NE(nameOffset, std::string_view::npos);

  ApplySourceEditResult result = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(nameOffset), FileOffset::Offset(nameOffset + 4)},
      .replacement = "stroke",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_EQ(result.diagnostic, std::nullopt);
  EXPECT_THAT(
      result.mutations,
      ElementsAre(
          AttributeMutationIs(XMLMutation::Kind::AttributeRemoved, rect, XMLQualifiedName("fill"),
                              Eq(std::nullopt), ReparseScope::OpeningTag),
          AttributeMutationIs(XMLMutation::Kind::AttributeSet, rect, XMLQualifiedName("stroke"),
                              testing::Optional(RcString("red")), ReparseScope::OpeningTag)));

  EXPECT_EQ(document.source(), R"(<svg><rect stroke="red"/></svg>)");
  EXPECT_THAT(rect.getAttribute("fill"), Eq(std::nullopt));
  EXPECT_THAT(rect.getAttribute("stroke"), Eq("red"));
}

TEST_F(XMLParserTests, ParsedDataNodeLocationsFollowSourceStoreEdits) {
  constexpr std::string_view kXml = R"(<svg><text>Hello world</text></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode text = document.root().firstChild()->firstChild().value();
  XMLNode data = text.firstChild().value();
  ASSERT_EQ(data.type(), XMLNode::Type::Data);

  std::optional<SourceRange> dataLocation = data.getNodeLocation();
  ASSERT_TRUE(dataLocation.has_value());
  ASSERT_TRUE(dataLocation->start.offset.has_value());
  ASSERT_TRUE(dataLocation->end.offset.has_value());
  EXPECT_EQ(document.source().substr(*dataLocation->start.offset,
                                     *dataLocation->end.offset - *dataLocation->start.offset),
            "Hello world");

  XMLSourceStore* sourceStore = document.sourceStore();
  ASSERT_NE(sourceStore, nullptr);
  ASSERT_TRUE(sourceStore->replace(*dataLocation->start.offset, 0, "Say: ").has_value());

  std::optional<SourceRange> updatedDataLocation = data.getNodeLocation();
  ASSERT_TRUE(updatedDataLocation.has_value());
  ASSERT_TRUE(updatedDataLocation->start.offset.has_value());
  ASSERT_TRUE(updatedDataLocation->end.offset.has_value());
  EXPECT_EQ(document.source().substr(
                *updatedDataLocation->start.offset,
                *updatedDataLocation->end.offset - *updatedDataLocation->start.offset),
            "Say: Hello world");
  EXPECT_EQ(document.nodeAtSourceOffset(*updatedDataLocation->start.offset), data);
}

TEST_F(XMLParserTests, ApplySourceEditUpdatesTextNode) {
  constexpr std::string_view kXml = R"(<svg><text>Hello world</text></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode text = document.root().firstChild()->firstChild().value();
  XMLNode data = text.firstChild().value();

  const std::size_t valueOffset = document.source().find("world");
  ASSERT_NE(valueOffset, std::string_view::npos);

  ApplySourceEditResult result = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 5)},
      .replacement = "there",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  EXPECT_EQ(result.diagnostic, std::nullopt);
  EXPECT_THAT(result.mutations,
              ElementsAre(NodeValueMutationIs(data, testing::Optional(RcString("Hello there")),
                                              ReparseScope::TextNode)));

  EXPECT_EQ(document.source(), R"(<svg><text>Hello there</text></svg>)");
  EXPECT_THAT(data.value(), testing::Optional(RcString("Hello there")));
  EXPECT_THAT(text.value(), testing::Optional(RcString("Hello there")));
}

TEST_F(XMLParserTests, ApplySourceEditInsertsAtTextNodeEnd) {
  constexpr std::string_view kXml = R"(<svg><text>Hello</text></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode text = document.root().firstChild()->firstChild().value();
  XMLNode data = text.firstChild().value();

  const std::size_t valueOffset = document.source().find("</text>");
  ASSERT_NE(valueOffset, std::string_view::npos);

  ApplySourceEditResult result = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset)},
      .replacement = "!",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  EXPECT_EQ(result.diagnostic, std::nullopt);
  EXPECT_THAT(result.mutations,
              ElementsAre(NodeValueMutationIs(data, testing::Optional(RcString("Hello!")),
                                              ReparseScope::TextNode)));

  EXPECT_EQ(document.source(), R"(<svg><text>Hello!</text></svg>)");
  EXPECT_THAT(data.value(), testing::Optional(RcString("Hello!")));
  EXPECT_THAT(text.value(), testing::Optional(RcString("Hello!")));
}

TEST_F(XMLParserTests, ApplySourceEditReparsesTextEntities) {
  constexpr std::string_view kXml = R"(<svg><text>Hello world</text></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode text = document.root().firstChild()->firstChild().value();
  XMLNode data = text.firstChild().value();

  const std::size_t valueOffset = document.source().find("world");
  ASSERT_NE(valueOffset, std::string_view::npos);

  ApplySourceEditResult result = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 5)},
      .replacement = "a&amp;b",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  EXPECT_EQ(result.diagnostic, std::nullopt);
  EXPECT_EQ(document.source(), R"(<svg><text>Hello a&amp;b</text></svg>)");
  EXPECT_THAT(data.value(), testing::Optional(RcString("Hello a&b")));
  EXPECT_THAT(text.value(), testing::Optional(RcString("Hello a&b")));
}

TEST_F(XMLParserTests, ApplySourceEditMarksTextEntityDirtyAndRecovers) {
  constexpr std::string_view kXml = R"(<svg><text>a&#65;b</text></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode text = document.root().firstChild()->firstChild().value();
  XMLNode data = text.firstChild().value();

  const std::size_t entityOffset = document.source().find("&#65;");
  ASSERT_NE(entityOffset, std::string_view::npos);

  ApplySourceEditResult dirtyResult = document.applySourceEdit(XMLEditIntent{
      .range =
          SourceRange{FileOffset::Offset(entityOffset + 4), FileOffset::Offset(entityOffset + 5)},
      .replacement = "",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(dirtyResult.applied);
  EXPECT_EQ(dirtyResult.scope, ReparseScope::TextNode);
  ASSERT_TRUE(dirtyResult.diagnostic.has_value());
  EXPECT_THAT(dirtyResult.mutations,
              ElementsAre(SourceDiagnosticMutationIs(data, Eq(dirtyResult.diagnostic),
                                                     ReparseScope::TextNode)));
  EXPECT_EQ(SourceText(document, dirtyResult.diagnostic->range), "a&#65b");
  EXPECT_EQ(document.source(), R"(<svg><text>a&#65b</text></svg>)");
  EXPECT_THAT(data.value(), testing::Optional(RcString("aAb")));
  EXPECT_THAT(text.value(), testing::Optional(RcString("aAb")));

  ApplySourceEditResult recoveryResult = document.applySourceEdit(XMLEditIntent{
      .range =
          SourceRange{FileOffset::Offset(entityOffset + 4), FileOffset::Offset(entityOffset + 4)},
      .replacement = ";",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(recoveryResult.applied);
  EXPECT_EQ(recoveryResult.scope, ReparseScope::TextNode);
  EXPECT_EQ(recoveryResult.diagnostic, std::nullopt);
  EXPECT_THAT(
      recoveryResult.mutations,
      ElementsAre(
          NodeValueMutationIs(data, testing::Optional(RcString("aAb")), ReparseScope::TextNode),
          SourceDiagnosticMutationIs(data, Eq(std::nullopt), ReparseScope::TextNode)));
  EXPECT_EQ(document.source(), kXml);
  EXPECT_THAT(data.value(), testing::Optional(RcString("aAb")));
  EXPECT_THAT(text.value(), testing::Optional(RcString("aAb")));
}

TEST_F(XMLParserTests, ApplySourceEditUpdatesCDataValueLocally) {
  constexpr std::string_view kXml = R"(<svg><![CDATA[x<y]]></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode cdata = document.root().firstChild()->firstChild().value();

  const std::size_t valueOffset = document.source().find("x<y");
  ASSERT_NE(valueOffset, std::string_view::npos);

  ApplySourceEditResult result = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 3)},
      .replacement = "a&b",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  EXPECT_EQ(result.diagnostic, std::nullopt);
  EXPECT_THAT(result.mutations,
              ElementsAre(NodeValueMutationIs(cdata, testing::Optional(RcString("a&b")),
                                              ReparseScope::TextNode)));

  EXPECT_EQ(document.source(), R"(<svg><![CDATA[a&b]]></svg>)");
  EXPECT_THAT(cdata.value(), testing::Optional(RcString("a&b")));
  ASSERT_TRUE(cdata.getValueLocation().has_value());
  EXPECT_EQ(SourceText(document, *cdata.getValueLocation()), "a&b");
}

TEST_F(XMLParserTests, ApplySourceEditUpdatesCommentValueLocally) {
  constexpr std::string_view kXml = R"(<svg><!-- hello --></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml, XMLParser::Options::ParseAll());
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode comment = document.root().firstChild()->firstChild().value();

  const std::size_t valueOffset = document.source().find("hello");
  ASSERT_NE(valueOffset, std::string_view::npos);

  ApplySourceEditResult result = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 5)},
      .replacement = "bye",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  EXPECT_EQ(result.diagnostic, std::nullopt);
  EXPECT_THAT(result.mutations,
              ElementsAre(NodeValueMutationIs(comment, testing::Optional(RcString(" bye ")),
                                              ReparseScope::TextNode)));

  EXPECT_EQ(document.source(), R"(<svg><!-- bye --></svg>)");
  EXPECT_THAT(comment.value(), testing::Optional(RcString(" bye ")));
  ASSERT_TRUE(comment.getValueLocation().has_value());
  EXPECT_EQ(SourceText(document, *comment.getValueLocation()), " bye ");
}

TEST_F(XMLParserTests, ApplySourceEditUpdatesProcessingInstructionValueLocally) {
  constexpr std::string_view kXml = R"(<svg><?target hello?></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml, XMLParser::Options::ParseAll());
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode processingInstruction = document.root().firstChild()->firstChild().value();

  const std::size_t valueOffset = document.source().find("hello");
  ASSERT_NE(valueOffset, std::string_view::npos);

  ApplySourceEditResult result = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 5)},
      .replacement = "world",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  EXPECT_EQ(result.diagnostic, std::nullopt);
  EXPECT_THAT(result.mutations, ElementsAre(NodeValueMutationIs(
                                    processingInstruction, testing::Optional(RcString("world")),
                                    ReparseScope::TextNode)));

  EXPECT_EQ(document.source(), R"(<svg><?target world?></svg>)");
  EXPECT_THAT(processingInstruction.value(), testing::Optional(RcString("world")));
  ASSERT_TRUE(processingInstruction.getValueLocation().has_value());
  EXPECT_EQ(SourceText(document, *processingInstruction.getValueLocation()), "world");
}

TEST_F(XMLParserTests, ApplySourceEditKeepsTextValueWhenTextEditChangesStructure) {
  constexpr std::string_view kXml = R"(<svg><text>Hello world</text></svg>)";

  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(kXml);
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode text = document.root().firstChild()->firstChild().value();
  XMLNode data = text.firstChild().value();

  const std::size_t valueOffset = document.source().find("world");
  ASSERT_NE(valueOffset, std::string_view::npos);

  ApplySourceEditResult result = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 5)},
      .replacement = "<tspan/>",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  ASSERT_TRUE(result.diagnostic.has_value());
  EXPECT_THAT(result.mutations, ElementsAre(SourceDiagnosticMutationIs(data, Eq(result.diagnostic),
                                                                       ReparseScope::TextNode)));
  EXPECT_EQ(document.source(), R"(<svg><text>Hello <tspan/></text></svg>)");
  EXPECT_THAT(data.value(), testing::Optional(RcString("Hello world")));
  EXPECT_THAT(text.value(), testing::Optional(RcString("Hello world")));
}

TEST_F(XMLParserTests, ProgrammaticDocumentDoesNotRequireSourceStore) {
  XMLDocument document;

  EXPECT_FALSE(document.hasSourceStore());
  EXPECT_EQ(document.source(), "");
  EXPECT_EQ(document.sourceVersion(), 0u);
  EXPECT_EQ(document.sourceStore(), nullptr);
}

TEST_F(XMLParserTests, Simple) {
  auto result = XMLParser::Parse(
      R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg">
      </svg>)");

  EXPECT_THAT(result, NoParseError());
}

TEST_F(XMLParserTests, WithOptions) {
  auto result = XMLParser::Parse(
      R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg">
      </svg>)",
      XMLParser::Options::ParseAll());

  EXPECT_THAT(result, NoParseError());
}

TEST_F(XMLParserTests, Empty) {
  auto maybeDocument = XMLParser::Parse("");
  ASSERT_THAT(maybeDocument, NoParseError());

  XMLDocument document = std::move(maybeDocument.result());
  XMLNode root = document.root();
  EXPECT_EQ(root.type(), XMLNode::Type::Document);
  EXPECT_THAT(root.nextSibling(), Eq(std::nullopt));
  EXPECT_THAT(root.firstChild(), Eq(std::nullopt));
}

TEST_F(XMLParserTests, InvalidNode) {
  EXPECT_THAT(XMLParser::Parse("abc"),
              AllOf(ParseErrorIs("Expected '<' to start a node"), ParseErrorPos(1, 0)));
  EXPECT_THAT(XMLParser::Parse("<node />abc"),
              AllOf(ParseErrorIs("Expected '<' to start a node"), ParseErrorPos(1, 8)));
  EXPECT_THAT(XMLParser::Parse("<node></node>\nabc"),
              AllOf(ParseErrorIs("Expected '<' to start a node"), ParseErrorPos(2, 0)));
  EXPECT_THAT(XMLParser::Parse("<node><!BADNODE></node>"),
              AllOf(ParseErrorIs("Unrecognized node starting with '<!'"), ParseErrorPos(1, 7)));
}

TEST_F(XMLParserTests, Namespace) {
  {
    auto maybeDocument = XMLParser::Parse(
        R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"></svg>)");
    ASSERT_THAT(maybeDocument, NoParseError());

    XMLDocument document = std::move(maybeDocument.result());
    XMLNode root = document.root();
    EXPECT_EQ(root.type(), XMLNode::Type::Document);
    EXPECT_THAT(root.nextSibling(), Eq(std::nullopt))
        << "XML must contain only a single element, such as <node></node>";

    std::optional<XMLNode> node = root.firstChild();
    ASSERT_TRUE(node.has_value()) << "XML must contain a single element, such as <node></node>";

    EXPECT_EQ(node->getNamespaceUri(""), "http://www.w3.org/2000/svg");
    EXPECT_EQ(node->getNamespaceUri("xlink"), "http://www.w3.org/1999/xlink");
    EXPECT_THAT(node->getNamespaceUri("unknown"), Eq(std::nullopt));
  }
}

TEST_F(XMLParserTests, ParseElement) {
  {
    auto maybeNode = parseAndGetFirstNode(R"(<node />)");
    ASSERT_TRUE(maybeNode.has_value());

    XMLNode node = std::move(maybeNode.value());

    EXPECT_EQ(node.type(), XMLNode::Type::Element);
    EXPECT_THAT(node.tagName(), Eq("node"));
  }
}

TEST_F(XMLParserTests, ParseElementErrorsClosingTag) {
  {
    auto result = XMLParser::Parse(
        R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg")");

    EXPECT_THAT(result,
                AllOf(ParseErrorIs("Node not closed with '>' or '/>'"), ParseErrorPos(1, 71)));
  }

  {
    auto result = XMLParser::Parse(
        R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg>
        </svg")");

    EXPECT_THAT(result,
                AllOf(ParseErrorIs("Node not closed with '>' or '/>'"), ParseErrorPos(2, 14)));
  }

  // Mismatched closing tag
  EXPECT_THAT(XMLParser::Parse(R"(<svg></node>)"),
              AllOf(ParseErrorIs("Mismatched closing tag"), ParseErrorPos(1, 7)));

  // Missing '>' on closing tag
  EXPECT_THAT(XMLParser::Parse(R"(<svg></svg)"),
              AllOf(ParseErrorIs("Expected '>' for closing tag"), ParseErrorPos(1, 10)));
}

TEST_F(XMLParserTests, ParseAttribute) {
  {
    auto maybeNode = parseAndGetFirstNode(
        R"(<node attr="value" xmlns:namespace="http://example.org" namespace:attr2="123" />)");
    ASSERT_TRUE(maybeNode.has_value());

    XMLNode node = std::move(maybeNode.value());

    EXPECT_EQ(node.type(), XMLNode::Type::Element);
    EXPECT_THAT(node.tagName(), Eq("node"));
    EXPECT_THAT(node.getAttribute("attr"), Eq("value"));
    EXPECT_THAT(node.getAttribute("invalid"), Eq(std::nullopt));

    // Test namespaces.
    EXPECT_THAT(node.getAttribute(XMLQualifiedNameRef("", "attr")), Eq("value"));
    EXPECT_THAT(node.getAttribute(XMLQualifiedNameRef("attr2")), Eq(std::nullopt));
    EXPECT_THAT(node.getAttribute(XMLQualifiedNameRef("namespace", "attr2")), Eq("123"));

    EXPECT_THAT(node.attributes(), ElementsAre("attr", XMLQualifiedNameRef("namespace", "attr2"),
                                               XMLQualifiedNameRef("xmlns", "namespace")));
  }

  // With whitespace
  {
    auto maybeNode = parseAndGetFirstNode(R"(<node attr = "value" />)");
    ASSERT_TRUE(maybeNode.has_value());

    XMLNode node = std::move(maybeNode.value());

    EXPECT_EQ(node.type(), XMLNode::Type::Element);
    EXPECT_THAT(node.tagName(), Eq("node"));
    EXPECT_THAT(node.getAttribute("attr"), Eq("value"));
  }
}

TEST_F(XMLParserTests, ParseAttributeErrors) {
  EXPECT_THAT(XMLParser::Parse(R"(<node attr />)"),
              AllOf(ParseErrorIs("Attribute name without value, expected '=' followed by a string"),
                    ParseErrorPos(1, 11)));
  EXPECT_THAT(XMLParser::Parse(R"(<node ns:attr />)"),
              AllOf(ParseErrorIs("Attribute name without value, expected '=' followed by a string"),
                    ParseErrorPos(1, 14)));

  // '=' with no string
  EXPECT_THAT(XMLParser::Parse(R"(<node attr= />)"),
              AllOf(ParseErrorIs("Attribute value not enclosed in quotes, expected \" or '"),
                    ParseErrorPos(1, 12)));

  // Invalid opening quotes
  EXPECT_THAT(XMLParser::Parse(R"(<node attr=$test$)"),
              AllOf(ParseErrorIs("Attribute value not enclosed in quotes, expected \" or '"),
                    ParseErrorPos(1, 11)));

  // No closing quotes
  EXPECT_THAT(XMLParser::Parse(R"(<node attr="value)"),
              AllOf(ParseErrorIs("Attribute value not closed with '\"'"), ParseErrorPos(1, 17)));
  EXPECT_THAT(XMLParser::Parse(R"(<node attr='value)"),
              AllOf(ParseErrorIs("Attribute value not closed with \"'\""), ParseErrorPos(1, 17)));
}

TEST_F(XMLParserTests, ParseData) {
  auto maybeNode = parseAndGetFirstNode(R"(<node>abcd</node>)");
  ASSERT_TRUE(maybeNode.has_value());

  const XMLNode node = std::move(maybeNode.value());
  EXPECT_THAT(node.value(), Eq("abcd"));

  auto maybeChild = node.firstChild();
  ASSERT_TRUE(maybeChild.has_value());

  XMLNode child = std::move(maybeChild.value());

  EXPECT_EQ(child.type(), XMLNode::Type::Data);
  EXPECT_THAT(child.value(), Eq("abcd"));
}

TEST_F(XMLParserTests, WhitespaceOnlyDataBetweenChildrenPreserved) {
  auto maybeNode = parseAndGetFirstNode(R"(<node>  <a/>  <b/>  </node>)");
  ASSERT_TRUE(maybeNode.has_value());

  const XMLNode node = std::move(maybeNode.value());

  auto child = node.firstChild();
  ASSERT_TRUE(child.has_value());
  EXPECT_EQ(child->type(), XMLNode::Type::Data);
  EXPECT_THAT(child->value(), testing::Optional(Eq("  ")));

  child = child->nextSibling();
  ASSERT_TRUE(child.has_value());
  EXPECT_EQ(child->type(), XMLNode::Type::Element);
  EXPECT_THAT(child->tagName(), Eq("a"));

  child = child->nextSibling();
  ASSERT_TRUE(child.has_value());
  EXPECT_EQ(child->type(), XMLNode::Type::Data);
  EXPECT_THAT(child->value(), testing::Optional(Eq("  ")));

  child = child->nextSibling();
  ASSERT_TRUE(child.has_value());
  EXPECT_EQ(child->type(), XMLNode::Type::Element);
  EXPECT_THAT(child->tagName(), Eq("b"));

  child = child->nextSibling();
  ASSERT_TRUE(child.has_value());
  EXPECT_EQ(child->type(), XMLNode::Type::Data);
  EXPECT_THAT(child->value(), testing::Optional(Eq("  ")));

  child = child->nextSibling();
  ASSERT_FALSE(child.has_value());
}

TEST_F(XMLParserTests, ParseCData) {
  auto maybeNode = parseAndGetFirstNode(R"(<![CDATA[abcd]]>)");
  ASSERT_TRUE(maybeNode.has_value());

  XMLNode node = std::move(maybeNode.value());

  EXPECT_EQ(node.type(), XMLNode::Type::CData);
  EXPECT_THAT(node.tagName(), Eq(""));
  EXPECT_THAT(node.value(), Eq("abcd"));
}

TEST_F(XMLParserTests, ParseCDataErrors) {
  EXPECT_THAT(XMLParser::Parse(R"(<![CDATA[abcd>)"),
              ParseErrorIs("CDATA node does not end with ']]>'"));
  EXPECT_THAT(XMLParser::Parse(R"(<![CDATA[abcd]>)"),
              ParseErrorIs("CDATA node does not end with ']]>'"));
}

TEST_F(XMLParserTests, ParseNodeErrors) {
  EXPECT_THAT(XMLParser::Parse(R"(<!INVALID>)"),
              ParseErrorIs("Unrecognized node starting with '<!'"));

  EXPECT_THAT(
      XMLParser::Parse(R"(<=badname>)"),
      ParseErrorIs("Invalid element name: Expected qualified name, found invalid character"));

  EXPECT_THAT(XMLParser::Parse(R"(<node>contents have eof)"),
              AllOf(ParseErrorIs("Unexpected end of data parsing node contents")));
}

TEST_F(XMLParserTests, ParseComment) {
  // By default comment parsing is disabled
  {
    auto maybeModeDefault = parseAndGetFirstNode(R"(<!-- hello world -->)");
    EXPECT_THAT(maybeModeDefault, Eq(std::nullopt));
  }

  XMLParser::Options options;
  options.parseComments = true;

  auto maybeNode = parseAndGetFirstNode(R"(<!-- hello world -->)", options);
  ASSERT_TRUE(maybeNode.has_value());

  XMLNode node = std::move(maybeNode.value());

  EXPECT_EQ(node.type(), XMLNode::Type::Comment);
  EXPECT_THAT(node.tagName(), Eq(""));
  EXPECT_THAT(node.value(), Eq(" hello world "));
}

TEST_F(XMLParserTests, ParseCommentInvalid) {
  EXPECT_THAT(XMLParser::Parse(R"(<!-- test)"),
              ParseErrorIs("Comment node does not end with '-->'"));
  EXPECT_THAT(XMLParser::Parse(R"(<!-- test ->)"),
              ParseErrorIs("Comment node does not end with '-->'"));
}

TEST_F(XMLParserTests, ParseDoctype) {
  // Check behavior when doctype parsing is disabled
  {
    XMLParser::Options options;
    options.parseDoctype = false;

    auto maybeModeDefault = parseAndGetFirstNode(R"(<!DOCTYPE html>)", options);
    EXPECT_THAT(maybeModeDefault, Eq(std::nullopt));
  }

  auto maybeNode = parseAndGetFirstNode(R"(<!DOCTYPE html>)");
  ASSERT_TRUE(maybeNode.has_value());

  XMLNode node = std::move(maybeNode.value());

  EXPECT_EQ(node.type(), XMLNode::Type::DocType);
  EXPECT_THAT(node.tagName(), Eq(""));
  EXPECT_THAT(node.value(), Eq("html"));
}

TEST_F(XMLParserTests, ParseDoctypeNested) {
  XMLParser::Options options;
  options.parseDoctype = true;

  auto maybeNode = parseAndGetFirstNode(R"(
      <!DOCTYPE html [[ nested [] values ]]>)",
                                        options);
  ASSERT_TRUE(maybeNode.has_value());

  XMLNode node = std::move(maybeNode.value());

  EXPECT_EQ(node.type(), XMLNode::Type::DocType);
  EXPECT_THAT(node.tagName(), Eq(""));
  EXPECT_THAT(node.value(), Eq("html [[ nested [] values ]]"));
}

TEST_F(XMLParserTests, ParseDoctypeDecls) {
  XMLParser::Options options;
  options.parseDoctype = true;

  auto maybeNode = parseAndGetFirstNode(R"(
      <!DOCTYPE html [
        <!ELEMENT html (head, body)>
        <!ELEMENT head (title)>
        <!ELEMENT title (#PCDATA)>
        <!ELEMENT body (p)>
        <!ELEMENT p (#PCDATA)>
      ]>
      <root></root>
      )",
                                        options);

  ASSERT_TRUE(maybeNode.has_value());

  XMLNode node = std::move(maybeNode.value());

  EXPECT_EQ(node.type(), XMLNode::Type::DocType);
  EXPECT_THAT(node.tagName(), Eq(""));
  EXPECT_THAT(node.value(), testing::Optional(testing::StartsWith("html [")));
  EXPECT_THAT(node.value(), testing::Optional(testing::EndsWith("]")));

  // Verify the next sibling is the root element
  auto nextNode = node.nextSibling();
  ASSERT_TRUE(nextNode.has_value());
  EXPECT_EQ(nextNode->type(), XMLNode::Type::Element);
  EXPECT_THAT(nextNode->tagName(), Eq("root"));
}

/// @test Doctype parsing errors
TEST_F(XMLParserTests, ParseDoctypeErrors) {
  using std::string_view_literals::operator""sv;

  EXPECT_THAT(XMLParser::Parse(R"(<!DOCTYPE>)"),
              ParseErrorIs("Expected whitespace after '<!DOCTYPE'"));
  EXPECT_THAT(XMLParser::Parse(R"(<!DOCTYPE )"), ParseErrorIs("Doctype node missing closing '>'"));
  EXPECT_THAT(XMLParser::Parse(R"(<!DOCTYPE html [>)"),
              ParseErrorIs("Doctype node missing closing ']'"));

  EXPECT_THAT(XMLParser::Parse("<!DOCTYPE html \0>"sv),
              ParseErrorIs("Unexpected end of data, found embedded null character"));
  EXPECT_THAT(XMLParser::Parse("<!DOCTYPE test [\0]><root></root>"sv),
              ParseErrorIs("Unexpected end of data, found embedded null character"));
}

/// @test Invalid doctype declarations that don't generate errors
TEST_F(XMLParserTests, ParseDoctypeMalformed) {
  EXPECT_THAT(XMLParser::Parse(R"(<!DOCTYPE html []]>)"), NoParseError());
}

/// @test PUBLIC entity declarations with two quoted strings are parsed correctly.
TEST_F(XMLParserTests, ParseDoctypePublicEntity) {
  XMLParser::Options options;
  options.parseDoctype = true;
  options.parseCustomEntities = true;

  // PUBLIC declarations have both a public ID and a system literal.
  auto result = parseAndGetFirstNode(R"(
      <!DOCTYPE svg [
        <!ENTITY logo PUBLIC "-//W3C//ENTITIES Logo//EN" "http://www.w3.org/logo.svg">
      ]>
      <root></root>
      )",
                                     options);
  ASSERT_TRUE(result.has_value()) << "PUBLIC entity declaration should parse without error";
}

/// @test SYSTEM entity declarations parse correctly.
TEST_F(XMLParserTests, ParseDoctypeSystemEntity) {
  XMLParser::Options options;
  options.parseDoctype = true;
  options.parseCustomEntities = true;

  auto result = parseAndGetFirstNode(R"(
      <!DOCTYPE svg [
        <!ENTITY logo SYSTEM "logo.svg">
      ]>
      <root></root>
      )",
                                     options);
  ASSERT_TRUE(result.has_value()) << "SYSTEM entity declaration should parse without error";
}

TEST_F(XMLParserTests, ParseDoctypeEntityEndSkipsQuotedGreaterThan) {
  auto result = XMLParser::Parse(R"(<!DOCTYPE test [<!ENTITY custom 'a>b'>]><node>&custom;</node>)",
                                 optionsCustomEntities());

  ASSERT_THAT(result, NoParseError());
  XMLNode node = result.result().root().firstChild()->nextSibling().value();
  ASSERT_TRUE(node.firstChild().has_value());
  EXPECT_THAT(node.firstChild()->value(), testing::Optional(RcString("a>b")));
}

TEST_F(XMLParserTests, ParseDoctypePublicEntityAcceptsSingleQuotedSystemLiteral) {
  XMLParser::Options options = optionsCustomEntities();

  auto result = XMLParser::Parse(
      R"(<!DOCTYPE svg [<!ENTITY logo PUBLIC "-//W3C//ENTITIES Logo//EN" 'logo.svg'>]><root/>)",
      options);

  EXPECT_THAT(result, NoParseError());
}

TEST_F(XMLParserTests, ParseProcessingInstructions) {
  // By default PI parsing is disabled
  {
    auto maybeModeDefault = parseAndGetFirstNode(R"(<?php contents ?>)");
    EXPECT_THAT(maybeModeDefault, Eq(std::nullopt));
  }

  XMLParser::Options options;
  options.parseProcessingInstructions = true;

  auto maybeNode = parseAndGetFirstNode(R"(<?php contents ?>)", options);
  ASSERT_TRUE(maybeNode.has_value());

  XMLNode node = std::move(maybeNode.value());

  EXPECT_EQ(node.type(), XMLNode::Type::ProcessingInstruction);
  EXPECT_THAT(node.tagName(), Eq("php"));
  EXPECT_THAT(node.value(), Eq("contents "));
}

TEST_F(XMLParserTests, ParseProcessingInstructionTargetAllowsColonName) {
  XMLParser::Options options = XMLParser::Options::ParseAll();

  ParseResult<XMLDocument> result = XMLParser::Parse(R"(<?p:q value?><root/>)", options);

  ASSERT_THAT(result, NoParseError());
  XMLNode instruction = result.result().root().firstChild().value();
  EXPECT_EQ(instruction.type(), XMLNode::Type::ProcessingInstruction);
  EXPECT_THAT(instruction.tagName(), Eq("p:q"));
  EXPECT_THAT(instruction.value(), Eq("value"));
}

TEST_F(XMLParserTests, ParseProcessingInstructionTargetAllowsLeadingColonName) {
  XMLParser::Options options = XMLParser::Options::ParseAll();

  ParseResult<XMLDocument> result = XMLParser::Parse(R"(<?:target value?><root/>)", options);

  ASSERT_THAT(result, NoParseError());
  XMLNode instruction = result.result().root().firstChild().value();
  EXPECT_EQ(instruction.type(), XMLNode::Type::ProcessingInstruction);
  EXPECT_THAT(instruction.tagName(), Eq(":target"));
  EXPECT_THAT(instruction.value(), Eq("value"));
}

TEST_F(XMLParserTests, ParseProcessingInstructionTargetAllowsUnicodeName) {
  XMLParser::Options options = XMLParser::Options::ParseAll();

  ParseResult<XMLDocument> result = XMLParser::Parse("<?\xC3\xA9 value?><root/>", options);

  ASSERT_THAT(result, NoParseError());
  XMLNode instruction = result.result().root().firstChild().value();
  EXPECT_EQ(instruction.type(), XMLNode::Type::ProcessingInstruction);
  EXPECT_THAT(instruction.tagName(), Eq("\xC3\xA9"));
  EXPECT_THAT(instruction.value(), Eq("value"));
}

TEST_F(XMLParserTests, ParseProcessingInstructionTargetAllowsUnicodeNameChar) {
  XMLParser::Options options = XMLParser::Options::ParseAll();

  ParseResult<XMLDocument> result = XMLParser::Parse("<?a\xCC\x80 value?><root/>", options);

  ASSERT_THAT(result, NoParseError());
  XMLNode instruction = result.result().root().firstChild().value();
  EXPECT_EQ(instruction.type(), XMLNode::Type::ProcessingInstruction);
  EXPECT_THAT(instruction.tagName(), Eq("a\xCC\x80"));
  EXPECT_THAT(instruction.value(), Eq("value"));
}

TEST_F(XMLParserTests, ParseProcessingInstructionsErrors) {
  XMLParser::Options options;
  options.parseProcessingInstructions = true;

  EXPECT_THAT(XMLParser::Parse("<?", options),
              ParseErrorIs("PI target does not begin with a name, e.g. '<?tag'"));
  EXPECT_THAT(XMLParser::Parse(R"(<?)", options),
              ParseErrorIs("PI target does not begin with a name, e.g. '<?tag'"));
  EXPECT_THAT(XMLParser::Parse(R"(<?php)", options),
              ParseErrorIs("PI node does not end with '?>'"));
  EXPECT_THAT(XMLParser::Parse(R"(<?php contents)", options),
              ParseErrorIs("PI node does not end with '?>'"));
}

TEST_F(XMLParserTests, ParseXMLDeclaration) {
  auto maybeNode = parseAndGetFirstNode(R"(<?xml version="1.0" ?>)");
  ASSERT_TRUE(maybeNode.has_value());

  XMLNode node = std::move(maybeNode.value());

  EXPECT_EQ(node.type(), XMLNode::Type::XMLDeclaration);
  EXPECT_THAT(node.tagName(), Eq(""));
  EXPECT_THAT(node.value(), Eq(std::nullopt));

  EXPECT_THAT(node.getAttribute("version"), Eq("1.0"));
}

TEST_F(XMLParserTests, ParseXMLDeclarationErrors) {
  EXPECT_THAT(XMLParser::Parse(R"(<?xml version="1.0")"),
              ParseErrorIs("XML declaration missing closing '?>'"));
  EXPECT_THAT(XMLParser::Parse(R"(<?xml version="1.0)"),
              ParseErrorIs("Attribute value not closed with '\"'"));
}

TEST_F(XMLParserTests, EntitiesBuiltin) {
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&amp;</node>)"), ParseResultIs(RcString("&")));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&amp;</node>)", optionsDisableEntityTranslation()),
              ParseResultIs(RcString("&amp;")));

  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&apos;</node>)"), ParseResultIs(RcString("\'")));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&apos;</node>)", optionsDisableEntityTranslation()),
              ParseResultIs(RcString("&apos;")));

  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&quot;</node>)"), ParseResultIs(RcString("\"")));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&quot;</node>)", optionsDisableEntityTranslation()),
              ParseResultIs(RcString("&quot;")));

  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&lt;</node>)"), ParseResultIs(RcString("<")));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&lt;</node>)", optionsDisableEntityTranslation()),
              ParseResultIs(RcString("&lt;")));

  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&gt;</node>)"), ParseResultIs(RcString(">")));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&gt;</node>)", optionsDisableEntityTranslation()),
              ParseResultIs(RcString("&gt;")));

  // Invalid entities are not parsed.
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&invalid;</node>)"),
              ParseResultIs(RcString("&invalid;")));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&;</node>)"), ParseResultIs(RcString("&;")));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&invalid</node>)"),
              ParseResultIs(RcString("&invalid")));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&bad-name;</node>)"),
              ParseResultIs(RcString("&bad-name;")));
}

TEST_F(XMLParserTests, EntitiesNumeric) {
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#48;</node>)"), ParseResultIs(RcString("0")));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#48;</node>)", optionsDisableEntityTranslation()),
              ParseResultIs(RcString("&#48;")));

  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#x20;</node>)"), ParseResultIs(RcString(" ")));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#x20;</node>)", optionsDisableEntityTranslation()),
              ParseResultIs(RcString("&#x20;")));

  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#x20;</node>)"), ParseResultIs(RcString(" ")));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#x20;</node>)", optionsDisableEntityTranslation()),
              ParseResultIs(RcString("&#x20;")));
}

TEST_F(XMLParserTests, EntitiesNumericErrors) {
  // Invalid characters
  EXPECT_THAT(XMLParser::Parse(R"(<node>&#abc;</node>)"),
              AllOf(ParseErrorIs("Unexpected character parsing integer"), ParseErrorPos(1, 8)));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#abc;</node>)", optionsDisableEntityTranslation()),
              ParseResultIs(RcString("&#abc;")));

  EXPECT_THAT(XMLParser::Parse("&#xfffe;"), ParseErrorIs("Invalid numeric character entity"));

  EXPECT_THAT(
      XMLParser::Parse(R"(<node>&#xhello;</node>)"),
      AllOf(ParseErrorIs("Invalid numeric entity syntax (missing digits)"), ParseErrorPos(1, 6)));
  EXPECT_THAT(
      parseAndGetNodeContents(R"(<node>&#xhello;</node>)", optionsDisableEntityTranslation()),
      ParseResultIs(RcString("&#xhello;")));

  EXPECT_THAT(XMLParser::Parse(R"(<node>&#a;</node>)"),
              AllOf(ParseErrorIs("Unexpected character parsing integer"), ParseErrorPos(1, 8)));

  // Note that line number information for this error is not available
  EXPECT_THAT(XMLParser::Parse(R"(
      <!DOCTYPE test [
        <!ENTITY num "&#a;">
      ]>
      <node>&num;</node>
    )",
                               optionsCustomEntities()),
              ParseErrorIs("Unexpected character parsing integer"));

  // Missing semicolon
  EXPECT_THAT(XMLParser::Parse(R"(<node>&#x20</node>)"),
              ParseErrorIs("Numeric character entity missing closing ';'"));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#x20</node>)", optionsDisableEntityTranslation()),
              ParseResultIs(RcString("&#x20")));

  EXPECT_THAT(XMLParser::Parse(R"(<node>&#65</node>)"),
              ParseErrorIs("Numeric character entity missing closing ';'"));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#65</node>)", optionsDisableEntityTranslation()),
              ParseResultIs(RcString("&#65")));

  //
  // Invalid unicode characters
  //

  // Above maximum allowed codepoint (0x10FFFF)
  EXPECT_THAT(XMLParser::Parse(R"(<node>&#xffffffff;</node>)"),
              ParseErrorIs("Invalid numeric character entity"));
  EXPECT_THAT(
      parseAndGetNodeContents(R"(<node>&#xffffffff;</node>)", optionsDisableEntityTranslation()),
      ParseResultIs(RcString("&#xffffffff;")));

  EXPECT_THAT(XMLParser::Parse(R"(<node>&#x110000;</node>)"),
              ParseErrorIs("Invalid numeric character entity"));
  EXPECT_THAT(
      parseAndGetNodeContents(R"(<node>&#x110000;</node>)", optionsDisableEntityTranslation()),
      ParseResultIs(RcString("&#x110000;")));

  // Surrogate codepoint (0xD800-0xDFFF)
  EXPECT_THAT(XMLParser::Parse(R"(<node>&#xd800;</node>)"),
              ParseErrorIs("Invalid numeric character entity"));
  EXPECT_THAT(
      parseAndGetNodeContents(R"(<node>&#xd800;</node>)", optionsDisableEntityTranslation()),
      ParseResultIs(RcString("&#xd800;")));

  // Non-character codepoint (0xFFFE, 0xFFFF)
  EXPECT_THAT(XMLParser::Parse(R"(<node>&#xfffe;</node>)"),
              ParseErrorIs("Invalid numeric character entity"));
  EXPECT_THAT(
      parseAndGetNodeContents(R"(<node>&#xfffe;</node>)", optionsDisableEntityTranslation()),
      ParseResultIs(RcString("&#xfffe;")));

  EXPECT_THAT(XMLParser::Parse(R"(<node>&#xffff;</node>)"),
              ParseErrorIs("Invalid numeric character entity"));
  EXPECT_THAT(
      parseAndGetNodeContents(R"(<node>&#xffff;</node>)", optionsDisableEntityTranslation()),
      ParseResultIs(RcString("&#xffff;")));

  // Same check for base-10 path
  EXPECT_THAT(XMLParser::Parse(R"(<node>&#65535;</node>)"),
              ParseErrorIs("Invalid numeric character entity"));
  EXPECT_THAT(
      parseAndGetNodeContents(R"(<node>&#65535;</node>)", optionsDisableEntityTranslation()),
      ParseResultIs(RcString("&#65535;")));

  // Invalid within an attribute
  EXPECT_THAT(XMLParser::Parse(R"(<node attrib="&#xfffe;" />)"),
              ParseErrorIs("Invalid numeric character entity"));
  EXPECT_THAT(XMLParser::Parse(R"(<node attrib="&#xfffe;" />)", optionsDisableEntityTranslation()),
              NoParseError());

  // Test nested parse errors
  EXPECT_THAT(XMLParser::Parse(R"(
    <!DOCTYPE test [
      <!ENTITY err "&#xfffe;">
    ]>
    <node>&err;</node>
  )",
                               optionsCustomEntities()),
              ParseErrorIs("Invalid numeric character entity"));
}

TEST_F(XMLParserTests, EntitiesCustom) {
  auto result = XMLParser::Parse(
      R"(<!DOCTYPE test [<!ENTITY custom "replacement text">]><node>&custom;</node>)",
      optionsCustomEntities());
  ASSERT_THAT(result, NoParseError());
  XMLDocument doc = std::move(result.result());

  // Check if the entity declaration was properly stored
  const auto& entityCtx = doc.registry().ctx().get<components::EntityDeclarationsContext>();
  auto maybeEntity =
      entityCtx.getEntityDeclaration(components::EntityType::General, RcString("custom"));
  ASSERT_TRUE(maybeEntity.has_value()) << "Entity 'custom' not found in entity declarations";
  EXPECT_EQ(maybeEntity->first, "replacement text") << "Entity value doesn't match expected";

  // Get the document node
  XMLNode root = doc.root();
  EXPECT_EQ(root.type(), XMLNode::Type::Document);

  // Get DOCTYPE node
  auto firstChild = root.firstChild();
  ASSERT_TRUE(firstChild.has_value());
  EXPECT_EQ(firstChild->type(), XMLNode::Type::DocType);

  // Get element node
  auto elementNode = firstChild->nextSibling();
  ASSERT_TRUE(elementNode.has_value());
  EXPECT_EQ(elementNode->type(), XMLNode::Type::Element);

  // Check data node content
  auto dataNode = elementNode->firstChild();
  ASSERT_TRUE(dataNode.has_value());
  EXPECT_EQ(dataNode->type(), XMLNode::Type::Data);

  EXPECT_EQ(dataNode->value(), "replacement text");
}

TEST_F(XMLParserTests, CustomEntityNamesAllowUnicodeStartAndNameChars) {
  auto result = XMLParser::Parse(
      "<!DOCTYPE test ["
      "<!ENTITY \xC3\xA9 \"acute\">"
      "<!ENTITY n\xCC\x80 \"combining\">"
      "]><node>&\xC3\xA9; &n\xCC\x80;</node>",
      optionsCustomEntities());

  ASSERT_THAT(result, NoParseError());
  XMLNode node = result.result().root().firstChild()->nextSibling().value();
  ASSERT_TRUE(node.firstChild().has_value());
  EXPECT_THAT(node.firstChild()->value(), testing::Optional(RcString("acute combining")));
}

TEST_F(XMLParserTests, EntitiesCustomErrors) {
  using std::string_view_literals::operator""sv;

  EXPECT_THAT(XMLParser::Parse("<!DOCTYPE test[<!ENTITY ]", optionsCustomEntities()),
              ParseErrorIs("Unterminated <!ENTITY declaration in DOCTYPE"));
  EXPECT_THAT(XMLParser::Parse("<!DOCTYPE test[<!ENTITY ]>", optionsCustomEntities()),
              ParseErrorIs("Expected entity name"));
  EXPECT_THAT(XMLParser::Parse("<!DOCTYPE test[<!ENTITY\0]"sv, optionsCustomEntities()),
              ParseErrorIs("Unterminated <!ENTITY declaration in DOCTYPE"));

  EXPECT_THAT(XMLParser::Parse("<!DOCTYPE test[<!ENTITY>]>", optionsCustomEntities()),
              ParseErrorIs("Expected entity name"));

  EXPECT_THAT(
      XMLParser::Parse(R"(<!DOCTYPE test [<!ENTITY ext SYSTEM]>)", XMLParser::Options::ParseAll()),
      ParseErrorIs("Expected quoted string in entity decl"));

  EXPECT_THAT(XMLParser::Parse(R"(<!DOCTYPE test [<!ENTITY ext SYSTEM "]>)",
                               XMLParser::Options::ParseAll()),
              ParseErrorIs("Unterminated <!ENTITY declaration in DOCTYPE"));

  EXPECT_THAT(
      XMLParser::Parse(R"(<!DOCTYPE test [<!ENTITY ext    ]>)", XMLParser::Options::ParseAll()),
      ParseErrorIs("Expected quoted string in entity decl"));

  EXPECT_THAT(XMLParser::Parse(R"(<!DOCTYPE test [<!ENTITY ext  PUBLIC  ]>)",
                               XMLParser::Options::ParseAll()),
              ParseErrorIs("Expected quoted string in entity decl"));

  EXPECT_THAT(
      XMLParser::Parse(R"(<!DOCTYPE test [<!ENTITY ext OTHER]>)", XMLParser::Options::ParseAll()),
      ParseErrorIs("Expected quoted string in entity decl"));

  EXPECT_THAT(XMLParser::Parse(R"(<!DOCTYPE test [<!ENTITY ext "value" junk>]><node/>)",
                               optionsCustomEntities()),
              ParseErrorIs("Expected '>' at end of entity declaration"));

  EXPECT_THAT(XMLParser::Parse(R"(<!DOCTYPE [<!ENTITY "
">]>)",
                               XMLParser::Options::ParseAll()),
              ParseErrorIs("Expected entity name"));

  EXPECT_THAT(XMLParser::Parse(
                  "\xef\xbb\xbf<!DOCTYPE Ca [<!ENTITY % [&'\b SYSTEM \"http://example.com/ext\">"
                  "<!ENTITY % a[ '&[&'\b;&[&'\b;&[&'\b;&[&'\b;&[&'\b;'><!ENTITY & "
                  "'&[&'\b;&[&'\b;&[&'\b;&[&'\b;'>"
                  "<!ENTITY a '&[&'\b;&[&'\b;&[&'\b;&[&'\b;&[&'\b;'><!ENTITY a ''><!ENTITY a "
                  "''><!ENTITY a ''>]>"
                  "<a></a>",
                  optionsCustomEntities()),
              ParseErrorIs("Expected entity name"));
}

TEST_F(XMLParserTests, EntitiesExternalSecurity) {
  // By default, external entities should not be resolved
  auto result = XMLParser::Parse(R"(
    <!DOCTYPE test [
      <!ENTITY external SYSTEM "http://example.com/entity.txt">
    ]>
    <node>&external;</node>
  )",
                                 optionsCustomEntities());

  ASSERT_THAT(result, NoParseError());
  XMLDocument doc = std::move(result.result());

  // Get the document node
  XMLNode root = doc.root();
  auto firstChild = root.firstChild();
  auto elementNode = firstChild->nextSibling();
  auto dataNode = elementNode->firstChild();
  ASSERT_TRUE(dataNode.has_value());
  ASSERT_TRUE(dataNode->value().has_value());

  // With external entities disabled (default), it should not be expanded
  EXPECT_EQ(dataNode->value(), "&external;");

  {
    // Single quotes are also valid

    auto result = XMLParser::Parse(R"(
      <!DOCTYPE test [
        <!ENTITY external SYSTEM 'http://example.com/entity.txt'>
      ]>
      <node>&external;</node>
    )",
                                   optionsCustomEntities());

    ASSERT_THAT(result, NoParseError());
  }
}

TEST_F(XMLParserTests, EntitiesRecursionLimits) {
  // Test recursive entity definition - should be caught and limited
  auto result = XMLParser::Parse(R"(
    <!DOCTYPE test [
      <!ENTITY recursive "&recursive;">
    ]>
    <node>&recursive;</node>
  )",
                                 optionsCustomEntities());

  // The parse should succeed, but when we try to access the content with the recursive entity,
  // we should get an error
  if (result.hasError()) {
    FAIL() << "Parsing should succeed, but entity resolution should fail when accessed";
  } else {
    XMLDocument doc = std::move(result.result());
    std::optional<XMLNode> dtdNode = doc.root().firstChild();
    ASSERT_TRUE(dtdNode.has_value());
    EXPECT_EQ(dtdNode->type(), XMLNode::Type::DocType);

    std::optional<XMLNode> elementNode = dtdNode->nextSibling();
    ASSERT_TRUE(elementNode.has_value());
    EXPECT_EQ(elementNode->type(), XMLNode::Type::Element);

    // The recursive entity should have been left unresolved
    EXPECT_THAT(elementNode->value(), Eq("&recursive;"));
  }
}

TEST_F(XMLParserTests, EntitySubstitutionLimitExceeded) {
  XMLParser::Options options = optionsCustomEntities();
  options.maxEntitySubstitutions = 2;

  auto result = XMLParser::Parse(R"(
    <!DOCTYPE test [
      <!ENTITY a "A">
    ]>
    <node>&a;&a;&a;</node>
  )",
                                 options);

  EXPECT_THAT(result, ParseErrorIs("Entity substitution limit exceeded"));
}

TEST_F(XMLParserTests, BuiltInEntitySubstitutionLimitExceeded) {
  XMLParser::Options options;
  options.maxEntitySubstitutions = 0;

  EXPECT_THAT(XMLParser::Parse("<node>&amp;</node>", options),
              ParseErrorIs("Entity substitution limit exceeded"));
}

TEST_F(XMLParserTests, OversizedCustomEntityExpansionRemainsLiteral) {
  XMLParser::Options options = optionsCustomEntities();
  std::string xml = R"(<!DOCTYPE test [<!ENTITY big ")";
  xml.append(70 * 1024, 'a');
  xml += R"(">]><node>&big;</node>)";

  ParseResult<XMLDocument> result = XMLParser::Parse(xml, options);

  ASSERT_THAT(result, NoParseError());
  XMLNode doctype = result.result().root().firstChild().value();
  ASSERT_EQ(doctype.type(), XMLNode::Type::DocType);
  XMLNode node = doctype.nextSibling().value();
  EXPECT_EQ(node.value(), "&big;");
}

TEST_F(XMLParserTests, MaxElementsLimitExceeded) {
  // Set a tiny element cap so the test input immediately overruns it. The
  // cap counts every tree-building node (element, CDATA, comment, doctype,
  // PI, XML declaration) so an attacker can't sidestep it with non-element
  // tree nodes.
  XMLParser::Options options;
  options.maxElements = 3;

  auto result = XMLParser::Parse("<a><b/><c/><d/></a>", options);

  // Expect: root <a> (1), <b/> (2), <c/> (3), <d/> (exceeds) → error on <d/>.
  EXPECT_THAT(result, ParseErrorIs("Maximum element count exceeded"));
}

TEST_F(XMLParserTests, MaxElementsLimitAppliesToNonElementNodes) {
  XMLParser::Options options = XMLParser::Options::ParseAll();
  options.maxElements = 0;

  EXPECT_THAT(XMLParser::Parse("<?xml version=\"1.0\"?>", options),
              ParseErrorIs("Maximum element count exceeded"));
  EXPECT_THAT(XMLParser::Parse("<!--x-->", options),
              ParseErrorIs("Maximum element count exceeded"));
  EXPECT_THAT(XMLParser::Parse("<![CDATA[x]]>", options),
              ParseErrorIs("Maximum element count exceeded"));
  EXPECT_THAT(XMLParser::Parse("<!DOCTYPE html>", options),
              ParseErrorIs("Maximum element count exceeded"));
  EXPECT_THAT(XMLParser::Parse("<?pi value?>", options),
              ParseErrorIs("Maximum element count exceeded"));
}

TEST_F(XMLParserTests, MaxElementsLimitAllowsRealisticDocuments) {
  // The default cap (100k) is far above any realistic SVG; this test
  // documents that a 50-element input parses cleanly without bumping caps.
  auto result = XMLParser::Parse(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g><rect/><rect/><rect/><rect/><rect/></g>
      <g><rect/><rect/><rect/><rect/><rect/></g>
      <g><rect/><rect/><rect/><rect/><rect/></g>
    </svg>
  )");
  EXPECT_TRUE(result.hasResult()) << result.error();
}

TEST_F(XMLParserTests, MaxAttributesPerElementLimitExceeded) {
  XMLParser::Options options;
  options.maxAttributesPerElement = 2;

  auto result = XMLParser::Parse(R"(<node a="1" b="2" c="3" d="4"/>)", options);

  EXPECT_THAT(result, ParseErrorIs("Maximum attributes-per-element count exceeded"));
}

TEST_F(XMLParserTests, MaxAttributesCapIsPerElement) {
  // The attribute cap is **per element**, not cumulative - two elements
  // each with the maximum attributes must both parse.
  XMLParser::Options options;
  options.maxAttributesPerElement = 3;

  auto result = XMLParser::Parse(R"(
    <root>
      <a x="1" y="2" z="3"/>
      <b x="1" y="2" z="3"/>
    </root>
  )",
                                 options);
  EXPECT_TRUE(result.hasResult()) << result.error();
}

TEST_F(XMLParserTests, MaxNestingDepthLimitExceeded) {
  XMLParser::Options options;
  options.maxNestingDepth = 3;

  // The root element sits at depth 0; each child pushes depth by one.
  // With maxNestingDepth=3, <a><b><c><d></d></c></b></a> is rejected
  // at the point where we try to enter <d> (depth 3 → 4 would exceed).
  auto result = XMLParser::Parse("<a><b><c><d><e/></d></c></b></a>", options);

  EXPECT_THAT(result, ParseErrorIs("Maximum element nesting depth exceeded"));
}

TEST_F(XMLParserTests, MaxNestingDepthAllowsSiblingSpread) {
  // A wide-but-shallow document should parse fine under a tight nesting
  // cap - the cap is about call-stack depth, not total element count.
  XMLParser::Options options;
  options.maxNestingDepth = 2;

  auto result = XMLParser::Parse(R"(
    <root>
      <a/><a/><a/><a/><a/>
      <a/><a/><a/><a/><a/>
    </root>
  )",
                                 options);
  EXPECT_TRUE(result.hasResult()) << result.error();
}

TEST_F(XMLParserTests, EntitiesComposition) {
  // Test entity composition (one entity referencing another)
  auto result = XMLParser::Parse(R"(
    <!DOCTYPE test [
      <!ENTITY part1 "Hello">
      <!ENTITY part2 "World">
      <!ENTITY message "&part1;, &part2;!">
    ]>
    <node>&message;</node>
  )",
                                 optionsCustomEntities());

  ASSERT_THAT(result, NoParseError());
  XMLDocument doc = std::move(result.result());
  XMLNode root = doc.root();
  auto firstChild = root.firstChild();
  auto elementNode = firstChild->nextSibling();
  auto dataNode = elementNode->firstChild();
  ASSERT_TRUE(dataNode.has_value());

  EXPECT_EQ(dataNode->value(), "Hello, World!");
}

TEST_F(XMLParserTests, ParameterEntities) {
  // Test parameter entity declarations
  auto result = XMLParser::Parse(R"(
    <!DOCTYPE test [
      <!ENTITY % common "INCLUDE">
      <!ENTITY % final "Complete">
      <!ENTITY doc "Document is %final;">
    ]>
    <node>&doc;</node>
  )",
                                 optionsCustomEntities());

  ASSERT_THAT(result, NoParseError());
  XMLDocument doc = std::move(result.result());
  XMLNode root = doc.root();
  auto firstChild = root.firstChild();
  auto elementNode = firstChild->nextSibling();
  auto dataNode = elementNode->firstChild();
  ASSERT_TRUE(dataNode.has_value());
  ASSERT_TRUE(dataNode->value().has_value());

  EXPECT_EQ(dataNode->value().value(), "Document is Complete");

  // Parameter entities should only be usable within DTD
  auto result2 = XMLParser::Parse(R"(
    <!DOCTYPE test [
      <!ENTITY % param "Parameter Content">
    ]>
    <node>Test: &param;</node>
  )",
                                  optionsCustomEntities());

  ASSERT_THAT(result2, NoParseError());
  XMLDocument doc2 = std::move(result2.result());
  XMLNode root2 = doc2.root();
  auto firstChild2 = root2.firstChild();
  auto elementNode2 = firstChild2->nextSibling();
  auto dataNode2 = elementNode2->firstChild();
  ASSERT_TRUE(dataNode2.has_value());
  ASSERT_TRUE(dataNode2->value().has_value());

  // Parameter entities should not be expanded in content
  EXPECT_EQ(dataNode2->value().value(), "Test: &param;");
}

TEST_F(XMLParserTests, ParseQualifiedNameErrors) {
  EXPECT_THAT(XMLParser::Parse(R"(<node attr:="value" />)"),
              AllOf(ParseErrorIs("Invalid attribute name: Expected local part of name after ':', "
                                 "found invalid character"),
                    ParseErrorPos(1, 11)));

  EXPECT_THAT(XMLParser::Parse(R"(<node: />)"),
              AllOf(ParseErrorIs("Invalid element name: Expected local part of name after ':', "
                                 "found invalid character"),
                    ParseErrorPos(1, 6)));

  EXPECT_THAT(XMLParser::Parse(R"(<node></node:>)"),
              AllOf(ParseErrorIs("Invalid closing tag name: Expected local part of name after ':', "
                                 "found invalid character"),
                    ParseErrorPos(1, 13)));
}

// Tests for XML-conforming Name validation per XML 1.0 productions [4], [4a], [5].
// NameStartChar ::= ":" | [A-Z] | "_" | [a-z] | [#xC0-#xD6] | ...
// NameChar ::= NameStartChar | "-" | "." | [0-9] | #xB7 | [#x0300-#x036F] | [#x203F-#x2040]
// Name ::= NameStartChar (NameChar)*

TEST_F(XMLParserTests, NameStartCharRejectsDigit) {
  // Names cannot start with a digit
  EXPECT_THAT(
      XMLParser::Parse(R"(<1tag />)"),
      ParseErrorIs("Invalid element name: Expected qualified name, found invalid character"));
}

TEST_F(XMLParserTests, NameStartCharRejectsHyphen) {
  // Names cannot start with a hyphen (NameChar but not NameStartChar)
  EXPECT_THAT(
      XMLParser::Parse(R"(<-tag />)"),
      ParseErrorIs("Invalid element name: Expected qualified name, found invalid character"));
}

TEST_F(XMLParserTests, NameStartCharRejectsDot) {
  // Names cannot start with a dot (NameChar but not NameStartChar)
  EXPECT_THAT(
      XMLParser::Parse(R"(<.tag />)"),
      ParseErrorIs("Invalid element name: Expected qualified name, found invalid character"));
}

TEST_F(XMLParserTests, NameCharAllowsDigitHyphenDot) {
  // Digits, hyphens, and dots are valid NameChar (after the start)
  auto maybeNode = parseAndGetFirstNode(R"(<tag-1.2 />)");
  ASSERT_TRUE(maybeNode.has_value());
  EXPECT_THAT(maybeNode->tagName(), Eq("tag-1.2"));
}

TEST_F(XMLParserTests, NameStartCharAllowsUnderscore) {
  auto maybeNode = parseAndGetFirstNode(R"(<_tag />)");
  ASSERT_TRUE(maybeNode.has_value());
  EXPECT_THAT(maybeNode->tagName(), Eq("_tag"));
}

TEST_F(XMLParserTests, NameRejectsSpecialChars) {
  // Characters like @, #, $, %, etc. are not valid in names
  EXPECT_THAT(
      XMLParser::Parse(R"(<@tag />)"),
      ParseErrorIs("Invalid element name: Expected qualified name, found invalid character"));
  EXPECT_THAT(
      XMLParser::Parse(R"(<#tag />)"),
      ParseErrorIs("Invalid element name: Expected qualified name, found invalid character"));
  EXPECT_THAT(
      XMLParser::Parse(R"(<$tag />)"),
      ParseErrorIs("Invalid element name: Expected qualified name, found invalid character"));
}

TEST_F(XMLParserTests, NameAllowsUnicodeLetters) {
  // Unicode letters in the NameStartChar range should be accepted
  // \xC3\xA9 = U+00E9 (é), in range [#xC0-#xD6] | [#xD8-#xF6] | [#xF8-#x2FF]
  auto maybeNode = parseAndGetFirstNode("<\xC3\xA9l\xC3\xA9ment />");
  ASSERT_TRUE(maybeNode.has_value());
  EXPECT_THAT(maybeNode->tagName(), Eq("\xC3\xA9l\xC3\xA9ment"));
}

TEST_F(XMLParserTests, NameAllowsCJKCharacters) {
  // U+4E2D = 中 (CJK), in range [#x3001-#xD7FF]
  // UTF-8: \xE4\xB8\xAD
  auto maybeNode = parseAndGetFirstNode("<\xE4\xB8\xAD\xE6\x96\x87 />");
  ASSERT_TRUE(maybeNode.has_value());
  EXPECT_THAT(maybeNode->tagName(), Eq("\xE4\xB8\xAD\xE6\x96\x87"));
}

TEST_F(XMLParserTests, NameAllowsSupplementaryPlaneStartCharacter) {
  // U+10000 is the first supplementary-plane code point allowed by NameStartChar.
  auto maybeNode = parseAndGetFirstNode("<\xF0\x90\x80\x80tag />");
  ASSERT_TRUE(maybeNode.has_value());
  EXPECT_THAT(maybeNode->tagName(), Eq("\xF0\x90\x80\x80tag"));
}

TEST_F(XMLParserTests, NameStartCharAllowsXmlRangeBoundaries) {
  const std::string_view names[] = {
      "\xC3\x80",          // U+00C0
      "\xC3\x96",          // U+00D6
      "\xC3\x98",          // U+00D8
      "\xC3\xB6",          // U+00F6
      "\xC3\xB8",          // U+00F8
      "\xCB\xBF",          // U+02FF
      "\xCD\xB0",          // U+0370
      "\xCD\xBD",          // U+037D
      "\xCD\xBF",          // U+037F
      "\xE1\xBF\xBF",      // U+1FFF
      "\xE2\x80\x8C",      // U+200C
      "\xE2\x80\x8D",      // U+200D
      "\xE2\x81\xB0",      // U+2070
      "\xE2\x86\x8F",      // U+218F
      "\xE2\xB0\x80",      // U+2C00
      "\xE2\xBF\xAF",      // U+2FEF
      "\xE3\x80\x81",      // U+3001
      "\xED\x9F\xBF",      // U+D7FF
      "\xEF\xA4\x80",      // U+F900
      "\xEF\xB7\x8F",      // U+FDCF
      "\xEF\xB7\xB0",      // U+FDF0
      "\xEF\xBF\xBD",      // U+FFFD
      "\xF3\xAF\xBF\xBF",  // U+EFFFF
  };

  for (std::string_view name : names) {
    SCOPED_TRACE(testing::Message() << "name bytes size=" << name.size());
    std::string xml = "<";
    xml.append(name);
    xml.append("tag />");

    auto maybeNode = parseAndGetFirstNode(xml);
    ASSERT_TRUE(maybeNode.has_value());

    std::string expected(name);
    expected.append("tag");
    EXPECT_EQ(std::string(maybeNode->tagName().name), expected);
  }
}

TEST_F(XMLParserTests, NameRejectsInvalidUnicodeRange) {
  // U+00D7 (×, multiplication sign) is NOT in NameStartChar ranges
  // (it's between [#xC0-#xD6] and [#xD8-#xF6])
  // UTF-8: \xC3\x97
  EXPECT_THAT(
      XMLParser::Parse("<\xC3\x97tag />"),
      ParseErrorIs("Invalid element name: Expected qualified name, found invalid character"));
}

TEST_F(XMLParserTests, NameParsingHandlesMalformedUtf8Sequences) {
  const auto makeXml = [](std::string_view nameStartBytes) {
    std::string xml = "<";
    xml.append(nameStartBytes);
    xml.append("tag />");
    return xml;
  };

  // Malformed code points are decoded as U+FFFD, which XML names accept.
  EXPECT_THAT(XMLParser::Parse(makeXml(std::string_view("\xC3", 1))), NoParseError());
  EXPECT_THAT(XMLParser::Parse(makeXml(std::string_view("\xC0\xAF", 2))), NoParseError());
  EXPECT_THAT(XMLParser::Parse(makeXml(std::string_view("\xE0\x80\x80", 3))), NoParseError());
  EXPECT_THAT(XMLParser::Parse(makeXml(std::string_view("\xF0\x80\x80\x80", 4))), NoParseError());
  EXPECT_THAT(XMLParser::Parse(makeXml(std::string_view("\xF4\x90\x80\x80", 4))), NoParseError());

  constexpr std::string_view kUnclosedNodeError = "Node not closed with '>' or '/>'";

  EXPECT_THAT(XMLParser::Parse(makeXml(std::string_view("\xC3\x28", 2))),
              ParseErrorIs(kUnclosedNodeError));
  EXPECT_THAT(XMLParser::Parse(makeXml(std::string_view("\xE2\x28\xA1", 3))),
              ParseErrorIs(kUnclosedNodeError));
  EXPECT_THAT(XMLParser::Parse(makeXml(std::string_view("\xF0\x28\x8C\xBC", 4))),
              ParseErrorIs(kUnclosedNodeError));
}

TEST_F(XMLParserTests, NameCharAllowsMiddleDotB7) {
  // U+00B7 (·, middle dot) is valid as NameChar but not NameStartChar
  // UTF-8: \xC2\xB7
  auto maybeNode = parseAndGetFirstNode("<tag\xC2\xB7name />");
  ASSERT_TRUE(maybeNode.has_value());
  EXPECT_THAT(maybeNode->tagName(), Eq("tag\xC2\xB7name"));
}

TEST_F(XMLParserTests, NameCharAllowsCombiningMarkAfterStart) {
  // U+0300 is allowed as NameChar after a valid starter.
  auto maybeNode = parseAndGetFirstNode("<tag\xCC\x80 />");
  ASSERT_TRUE(maybeNode.has_value());
  EXPECT_THAT(maybeNode->tagName(), Eq("tag\xCC\x80"));
}

TEST_F(XMLParserTests, NameCharAllowsXmlRangeBoundariesAfterAsciiStart) {
  const std::string_view suffixes[] = {
      "\xCC\x80",      // U+0300
      "\xCD\xAF",      // U+036F
      "\xE2\x80\xBF",  // U+203F
      "\xE2\x81\x80",  // U+2040
  };

  for (std::string_view suffix : suffixes) {
    SCOPED_TRACE(testing::Message() << "suffix bytes size=" << suffix.size());
    std::string xml = "<tag";
    xml.append(suffix);
    xml.append(" />");

    auto maybeNode = parseAndGetFirstNode(xml);
    ASSERT_TRUE(maybeNode.has_value());

    std::string expected = "tag";
    expected.append(suffix);
    EXPECT_EQ(std::string(maybeNode->tagName().name), expected);
  }
}

TEST_F(XMLParserTests, NameStartCharRejectsMiddleDotB7) {
  // U+00B7 is only valid as NameChar, not NameStartChar
  EXPECT_THAT(
      XMLParser::Parse("<\xC2\xB7tag />"),
      ParseErrorIs("Invalid element name: Expected qualified name, found invalid character"));
}

TEST_F(XMLParserTests, AttributeNameConformance) {
  // Attribute names follow the same Name rules
  EXPECT_THAT(XMLParser::Parse(R"(<node 1attr="value" />)"),
              ParseErrorIs("Node not closed with '>' or '/>'"));
  EXPECT_THAT(XMLParser::Parse(R"(<node -attr="value" />)"),
              ParseErrorIs("Node not closed with '>' or '/>'"));
}

TEST_F(XMLParserTests, GetAttributeLocationBasic) {
  // Setup test XML.
  std::string_view xml = R"(<root><child attr="Hello, world!"></child></root>)";

  // Hardcode the offset of '<child' in this sample string.
  // For quick determination, count characters or use a parser that returns element offsets.
  const auto kChildOffset = FileOffset::Offset(6);

  // Call the function under test.
  auto location =
      XMLParser::GetAttributeLocation(xml, kChildOffset, XMLQualifiedNameRef("", "attr"));
  ASSERT_TRUE(location.has_value());

  // Extract substring from the returned offsets.
  const size_t start = location->start.offset.value();
  const size_t end = location->end.offset.value();
  EXPECT_LT(start, end);
  std::string_view foundAttribute = xml.substr(start, end - start);

  // Verify correctness.
  EXPECT_THAT(foundAttribute, testing::Eq(R"(attr="Hello, world!")"));
}

TEST_F(XMLParserTests, GetAttributeLocationNoSuchAttribute) {
  std::string_view xml = R"(<root><child attr="Hello, world!"></child></root>)";

  // Offset for <child>.
  const auto kChildOffset = FileOffset::Offset(6);

  // Ask for a non-existent attribute.
  EXPECT_THAT(
      XMLParser::GetAttributeLocation(xml, kChildOffset, XMLQualifiedNameRef("", "missing")),
      testing::Eq(std::nullopt));
}

TEST_F(XMLParserTests, GetAttributeLocationWithNamespace) {
  // Example with namespace usage.
  std::string_view xml = R"(<root><child ns:attr="namespaced value" another="value"/></root>)";

  // Offset for <child>.
  const auto kChildOffset = FileOffset::Offset(6);

  // Attempt retrieval with the namespace prefix "ns".
  auto location =
      XMLParser::GetAttributeLocation(xml, kChildOffset, XMLQualifiedNameRef("ns", "attr"));
  ASSERT_TRUE(location.has_value());

  const size_t start = location->start.offset.value();
  const size_t end = location->end.offset.value();
  const std::string_view foundAttribute = xml.substr(start, end - start);
  EXPECT_THAT(foundAttribute, testing::Eq(R"(ns:attr="namespaced value")"));
}

TEST_F(XMLParserTests, GetAttributeLocationInvalidOffset) {
  std::string_view xml = R"(<root><child attr="Hello, world!"></child></root>)";

  // Offset for <child>.
  const auto kChildOffset = FileOffset::EndOfString();

  EXPECT_THAT(XMLParser::GetAttributeLocation(xml, kChildOffset, "attr"),
              testing::Eq(std::nullopt));
}

TEST_F(XMLParserTests, GetAttributeLocationOutOfBoundsOffset) {
  // Regression for the structured-editing M−1 prerequisite: the editor's
  // text-edit fast path may call GetAttributeLocation with an offset
  // derived from a prior parse that no longer matches the current source
  // (the user has typed bytes since). An offset past the end of the
  // string must return std::nullopt, not crash or read past the buffer.
  std::string_view xml = R"(<root><child attr="v"/></root>)";
  EXPECT_THAT(XMLParser::GetAttributeLocation(xml, FileOffset::Offset(9999), "attr"),
              testing::Eq(std::nullopt));
  EXPECT_THAT(XMLParser::GetAttributeLocation(xml, FileOffset::Offset(xml.size()), "attr"),
              testing::Eq(std::nullopt));
  EXPECT_THAT(XMLParser::GetAttributeLocation(xml, FileOffset::Offset(xml.size() + 1), "attr"),
              testing::Eq(std::nullopt));
}

TEST_F(XMLParserTests, GetAttributeLocationMalformedInputAtOffset) {
  // Offset points inside a well-formed region of the outer parse, but
  // the element at that offset is no longer well-formed - the user just
  // typed a stray character. Historically this release-asserted; now
  // it must return std::nullopt cleanly.

  // Offset points to a '<' that introduces a bogus element name.
  std::string_view badName = R"(<root><1notaname attr="v"/></root>)";
  const auto offsetToBadElement = FileOffset::Offset(6);  // '<' of the bogus element
  EXPECT_THAT(XMLParser::GetAttributeLocation(badName, offsetToBadElement, "attr"),
              testing::Eq(std::nullopt));

  // Offset points at text content, not a '<'.
  std::string_view textContent = R"(<root>plain text</root>)";
  EXPECT_THAT(XMLParser::GetAttributeLocation(textContent, FileOffset::Offset(6), "attr"),
              testing::Eq(std::nullopt));

  // Offset points at a partially-typed attribute (unterminated quote).
  std::string_view partialAttr = R"(<root><child attr="unterm)";
  EXPECT_THAT(XMLParser::GetAttributeLocation(partialAttr, FileOffset::Offset(6), "attr"),
              testing::Eq(std::nullopt));
}

TEST_F(XMLParserTests, ParameterEntitiesRecursionLimits) {
  auto result = XMLParser::Parse(R"(
    <!DOCTYPE test [
      <!ENTITY % recursive "%recursive;">
      <!ENTITY doc "Document is %recursive;">
    ]>
    <node>&doc;</node>
  )",
                                 optionsCustomEntities());

  // Ensure that the parser did not crash and no parse error occurred.
  ASSERT_THAT(result, NoParseError())
      << "Parsing should succeed without crashing for recursive parameter entities";

  XMLDocument doc = std::move(result.result());

  // The first child should be the DOCTYPE node.
  std::optional<XMLNode> dtdNode = doc.root().firstChild();
  ASSERT_TRUE(dtdNode.has_value());
  EXPECT_EQ(dtdNode->type(), XMLNode::Type::DocType);

  // The next sibling should be the element node.
  std::optional<XMLNode> elementNode = dtdNode->nextSibling();
  ASSERT_TRUE(elementNode.has_value());
  EXPECT_EQ(elementNode->type(), XMLNode::Type::Element);

  // The recursive parameter entity (%recursive;) should not be expanded.
  // Therefore, the general entity "doc" remains with the literal "%recursive;" in its value.
  std::optional<XMLNode> dataNode = elementNode->firstChild();
  ASSERT_TRUE(dataNode.has_value());
  ASSERT_TRUE(dataNode->value().has_value());
  EXPECT_EQ(dataNode->value().value(), "Document is %recursive;");
}

/// @test entity without semicolon
TEST_F(XMLParserTests, EntityWithNoSemicolon) {
  XMLParser::Options options;
  options.parseDoctype = true;

  auto result = XMLParser::Parse(R"(
    <!DOCTYPE test [
      <!ENTITY entity "replacement text">
    ]>
    <node>&entity</node>
  )",
                                 options);

  ASSERT_THAT(result, NoParseError());
  XMLDocument doc = std::move(result.result());
  XMLNode root = doc.root();
  auto firstChild = root.firstChild();
  auto elementNode = firstChild->nextSibling();
  auto dataNode = elementNode->firstChild();
  ASSERT_TRUE(dataNode.has_value());
  ASSERT_TRUE(dataNode->value().has_value());

  // The entity won't be expanded because there's no semicolon
  EXPECT_EQ(dataNode->value().value(), "&entity");
}

/// @test the case where PCData starts with an entity reference that causes an error
TEST_F(XMLParserTests, PCDataStartsWithErrorEntity) {
  EXPECT_THAT(XMLParser::Parse(R"(<node>&#xfffe;text</node>)"),
              ParseErrorIs("Invalid numeric character entity"));
}

/// @test with a single quote attribute having entities
TEST_F(XMLParserTests, SingleQuoteAttributeWithEntity) {
  auto result = XMLParser::Parse(R"(
    <!DOCTYPE test [
      <!ENTITY custom "replacement">
    ]>
    <node attr='&custom; value' />
  )",
                                 optionsCustomEntities());

  ASSERT_THAT(result, NoParseError());
  XMLDocument doc = std::move(result.result());
  XMLNode root = doc.root();
  auto firstChild = root.firstChild();
  auto elementNode = firstChild->nextSibling();

  EXPECT_THAT(elementNode->getAttribute("attr"), Eq("replacement value"));
}

/// Validate that the parser can handle the "Billion Laughs" attack.
/// @see https://en.wikipedia.org/wiki/Billion_laughs_attack
TEST_F(XMLParserTests, BillionLaughs) {
  XMLParser::Options options;
  options.parseDoctype = true;

  auto result = XMLParser::Parse(R"(
    <!DOCTYPE lolz [
    <!ENTITY lol "lol">
    <!ELEMENT lolz (#PCDATA)>
    <!ENTITY lol1 "&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;">
    <!ENTITY lol2 "&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;">
    <!ENTITY lol3 "&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;">
    <!ENTITY lol4 "&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;">
    <!ENTITY lol5 "&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;">
    <!ENTITY lol6 "&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;">
    <!ENTITY lol7 "&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;">
    <!ENTITY lol8 "&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;">
    <!ENTITY lol9 "&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;">
    ]>
    <lolz>&lol9;</lolz>
  )",
                                 options);

  // Ensure that the parser did not crash and no parse error occurred.
  ASSERT_THAT(result, NoParseError())
      << "Parsing should succeed without crashing for recursive entities";

  XMLDocument doc = std::move(result.result());

  // The first child should be the DOCTYPE node.
  std::optional<XMLNode> dtdNode = doc.root().firstChild();
  ASSERT_TRUE(dtdNode.has_value());
  EXPECT_EQ(dtdNode->type(), XMLNode::Type::DocType);

  // The next sibling should be the element node.
  std::optional<XMLNode> elementNode = dtdNode->nextSibling();
  ASSERT_TRUE(elementNode.has_value());
  EXPECT_EQ(elementNode->type(), XMLNode::Type::Element);

  // The recursive parameter entity (%recursive;) should not be expanded.
  // Therefore, the general entity "doc" remains with the literal "%recursive;" in its value.
  std::optional<XMLNode> dataNode = elementNode->firstChild();
  ASSERT_TRUE(dataNode.has_value());
  ASSERT_TRUE(dataNode->value().has_value());
  EXPECT_LE(dataNode->value().value().size(), 64 * 1024)
      << "Size should be less than 64kb, per internal XMLParser constant";
}

TEST_F(XMLParserTests, EntityContainingNode) {
  auto result = XMLParser::Parse(R"(
    <!DOCTYPE test [
      <!ENTITY rect "<rect />">
    ]>
    &rect;
  )",
                                 optionsCustomEntities());

  ASSERT_THAT(result, NoParseError());

  XMLDocument doc = std::move(result.result());

  // The first child should be the DOCTYPE node.
  std::optional<XMLNode> dtdNode = doc.root().firstChild();
  ASSERT_TRUE(dtdNode.has_value());
  EXPECT_EQ(dtdNode->type(), XMLNode::Type::DocType);

  // The next sibling should be the <rect> node.
  std::optional<XMLNode> elementNode = dtdNode->nextSibling();
  ASSERT_TRUE(elementNode.has_value());
  EXPECT_EQ(elementNode->type(), XMLNode::Type::Element);
  EXPECT_EQ(elementNode->tagName(), "rect");
}

}  // namespace donner::xml
