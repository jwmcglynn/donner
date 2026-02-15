#include "donner/base/xml/XMLParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string_view>

#include "donner/base/ParseResult.h"
#include "donner/base/RcString.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/base/xml/components/EntityDeclarationsContext.h"

using testing::AllOf;
using testing::ElementsAre;
using testing::Eq;

// TODO: Add an ErrorHighlightsText matcher

namespace donner::xml {

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

TEST_F(XMLParserTests, ParseProcessingInstructionsErrors) {
  XMLParser::Options options;
  options.parseProcessingInstructions = true;

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

TEST_F(XMLParserTests, EntitiesCustomErrors) {
  using std::string_view_literals::operator""sv;

  EXPECT_THAT(XMLParser::Parse("<!DOCTYPE test[<!ENTITY ]", optionsCustomEntities()),
              ParseErrorIs("Unterminated <!ENTITY declaration in DOCTYPE"));
  EXPECT_THAT(XMLParser::Parse("<!DOCTYPE test[<!ENTITY ]>", optionsCustomEntities()),
              ParseErrorIs("Expected quoted string in entity decl"));
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
              ParseErrorIs("Expected '>' at end of entity declaration"));
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

TEST_F(XMLParserTests, CapturesSourceOffsetsForNodesAndAttributes) {
  const std::string_view xml = R"(<root attr="one">text<!--c--><![CDATA[raw]]></root>)";

  XMLParser::Options options = XMLParser::Options::ParseAll();
  auto result = XMLParser::Parse(xml, options);
  ASSERT_THAT(result, NoParseError());

  XMLDocument document = std::move(result.result());
  std::optional<XMLNode> root = document.root().firstChild();
  ASSERT_TRUE(root.has_value());

  auto attributeLocation = root->getAttributeLocation(xml, XMLQualifiedNameRef("", "attr"));
  ASSERT_TRUE(attributeLocation.has_value());
  EXPECT_EQ(
      xml.substr(attributeLocation->start.offset.value(),
                 attributeLocation->end.offset.value() - attributeLocation->start.offset.value()),
      R"(attr="one")");

  auto attributeValueLocation = root->getAttributeValueLocation(XMLQualifiedNameRef("", "attr"));
  ASSERT_TRUE(attributeValueLocation.has_value());
  EXPECT_EQ(xml.substr(attributeValueLocation->start.offset.value(),
                       attributeValueLocation->end.offset.value() -
                           attributeValueLocation->start.offset.value()),
            "one");

  std::optional<XMLNode> data = root->firstChild();
  ASSERT_TRUE(data.has_value());
  EXPECT_EQ(data->type(), XMLNode::Type::Data);
  auto dataLocation = data->getNodeLocation();
  ASSERT_TRUE(dataLocation.has_value());
  EXPECT_EQ(xml.substr(dataLocation->start.offset.value(),
                       dataLocation->end.offset.value() - dataLocation->start.offset.value()),
            "text");
  auto dataValue = data->getValueLocation();
  ASSERT_TRUE(dataValue.has_value());
  EXPECT_EQ(xml.substr(dataValue->start.offset.value(),
                       dataValue->end.offset.value() - dataValue->start.offset.value()),
            "text");

  std::optional<XMLNode> comment = data->nextSibling();
  ASSERT_TRUE(comment.has_value());
  EXPECT_EQ(comment->type(), XMLNode::Type::Comment);
  auto commentLocation = comment->getNodeLocation();
  ASSERT_TRUE(commentLocation.has_value());
  EXPECT_EQ(xml.substr(commentLocation->start.offset.value(),
                       commentLocation->end.offset.value() - commentLocation->start.offset.value()),
            "<!--c-->");
  auto commentValue = comment->getValueLocation();
  ASSERT_TRUE(commentValue.has_value());
  EXPECT_EQ(xml.substr(commentValue->start.offset.value(),
                       commentValue->end.offset.value() - commentValue->start.offset.value()),
            "c");

  std::optional<XMLNode> cdata = comment->nextSibling();
  ASSERT_TRUE(cdata.has_value());
  EXPECT_EQ(cdata->type(), XMLNode::Type::CData);
  auto cdataLocation = cdata->getNodeLocation();
  ASSERT_TRUE(cdataLocation.has_value());
  EXPECT_EQ(xml.substr(cdataLocation->start.offset.value(),
                       cdataLocation->end.offset.value() - cdataLocation->start.offset.value()),
            "<![CDATA[raw]]>");
  auto cdataValue = cdata->getValueLocation();
  ASSERT_TRUE(cdataValue.has_value());
  EXPECT_EQ(xml.substr(cdataValue->start.offset.value(),
                       cdataValue->end.offset.value() - cdataValue->start.offset.value()),
            "raw");
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
