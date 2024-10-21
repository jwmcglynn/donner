#include "donner/base/xml/XMLParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/RcString.h"
#include "donner/base/parser/ParseResult.h"
#include "donner/base/parser/tests/ParseResultTestUtils.h"
#include "donner/base/xml/XMLQualifiedName.h"

using namespace donner::base::parser;
using testing::AllOf;
using testing::ElementsAre;
using testing::Eq;

// TODO: Add an ErrorHighlightsText matcher

namespace donner::xml {

class XMLParserTests : public testing::Test {
protected:
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

TEST_F(XMLParserTests, InvalidNode) {
  EXPECT_THAT(XMLParser::Parse("abc"),
              AllOf(ParseErrorIs("Expected '<' to start a node"), ParseErrorPos(1, 0)));
  EXPECT_THAT(XMLParser::Parse("<node />abc"),
              AllOf(ParseErrorIs("Expected '<' to start a node"), ParseErrorPos(1, 8)));
  EXPECT_THAT(XMLParser::Parse("<node></node>\nabc"),
              AllOf(ParseErrorIs("Expected '<' to start a node"), ParseErrorPos(2, 0)));
}

TEST_F(XMLParserTests, Namespace) {
  {
    auto maybeDocument = XMLParser::Parse(
        R"(<svg id="svg1" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"></svg>)");
    ASSERT_THAT(maybeDocument, NoParseError());

    if (maybeDocument.hasResult()) {
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
  // By default doctype parsing is disabled
  {
    auto maybeModeDefault = parseAndGetFirstNode(R"(<!DOCTYPE html>)");
    EXPECT_THAT(maybeModeDefault, Eq(std::nullopt));
  }

  XMLParser::Options options;
  options.parseDoctype = true;

  auto maybeNode = parseAndGetFirstNode(R"(<!DOCTYPE html>)", options);
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
      ]>)",
                                        options);
  ASSERT_TRUE(maybeNode.has_value());

  XMLNode node = std::move(maybeNode.value());

  EXPECT_EQ(node.type(), XMLNode::Type::DocType);
  EXPECT_THAT(node.tagName(), Eq(""));
  EXPECT_THAT(node.value(), testing::Optional(testing::StartsWith("html [")));
  EXPECT_THAT(node.value(), testing::Optional(testing::EndsWith("]")));
}

TEST_F(XMLParserTests, ParseDoctypeErrors) {
  EXPECT_THAT(XMLParser::Parse(R"(<!DOCTYPE>)"),
              ParseErrorIs("Expected whitespace after '<!DOCTYPE'"));
  EXPECT_THAT(XMLParser::Parse(R"(<!DOCTYPE )"), ParseErrorIs("Doctype node missing closing '>'"));
  EXPECT_THAT(XMLParser::Parse(R"(<!DOCTYPE html [>)"),
              ParseErrorIs("Doctype node missing closing ']'"));
  EXPECT_THAT(XMLParser::Parse(std::string_view("<!DOCTYPE html \0>", 18)),
              ParseErrorIs("Unexpected end of data, found embedded null character"));
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
  XMLParser::Options optionsDisableEntity;
  optionsDisableEntity.disableEntityTranslation = true;

  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&amp;</node>)"), ParseResultIs(RcString("&")));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&amp;</node>)", optionsDisableEntity),
              ParseResultIs(RcString("&amp;")));

  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&apos;</node>)"), ParseResultIs(RcString("\'")));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&apos;</node>)", optionsDisableEntity),
              ParseResultIs(RcString("&apos;")));

  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&quot;</node>)"), ParseResultIs(RcString("\"")));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&quot;</node>)", optionsDisableEntity),
              ParseResultIs(RcString("&quot;")));

  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&lt;</node>)"), ParseResultIs(RcString("<")));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&lt;</node>)", optionsDisableEntity),
              ParseResultIs(RcString("&lt;")));

  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&gt;</node>)"), ParseResultIs(RcString(">")));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&gt;</node>)", optionsDisableEntity),
              ParseResultIs(RcString("&gt;")));
}

TEST_F(XMLParserTests, EntitiesNumeric) {
  XMLParser::Options optionsDisableEntity;
  optionsDisableEntity.disableEntityTranslation = true;

  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#48;</node>)"), ParseResultIs(RcString("0")));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#48;</node>)", optionsDisableEntity),
              ParseResultIs(RcString("&#48;")));

  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#x20;</node>)"), ParseResultIs(RcString(" ")));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#x20;</node>)", optionsDisableEntity),
              ParseResultIs(RcString("&#x20;")));

  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#x20;</node>)"), ParseResultIs(RcString(" ")));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#x20;</node>)", optionsDisableEntity),
              ParseResultIs(RcString("&#x20;")));
}

TEST_F(XMLParserTests, EntitiesNumericErrors) {
  XMLParser::Options optionsDisableEntity;
  optionsDisableEntity.disableEntityTranslation = true;

  // Invalid characters
  EXPECT_THAT(XMLParser::Parse(R"(<node>&#abc;</node>)"),
              AllOf(ParseErrorIs("Unexpected character parsing integer"), ParseErrorPos(1, 8)));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#abc;</node>)", optionsDisableEntity),
              ParseResultIs(RcString("&#abc;")));

  EXPECT_THAT(XMLParser::Parse(R"(<node>&#xhello;</node>)"),
              AllOf(ParseErrorIs("Unexpected character parsing hex integer"), ParseErrorPos(1, 9)));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#xhello;</node>)", optionsDisableEntity),
              ParseResultIs(RcString("&#xhello;")));

  // Missing semicolon
  EXPECT_THAT(XMLParser::Parse(R"(<node>&#x20</node>)"),
              ParseErrorIs("Numeric character entity missing closing ';'"));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#x20</node>)", optionsDisableEntity),
              ParseResultIs(RcString("&#x20")));

  EXPECT_THAT(XMLParser::Parse(R"(<node>&#65</node>)"),
              ParseErrorIs("Numeric character entity missing closing ';'"));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#65</node>)", optionsDisableEntity),
              ParseResultIs(RcString("&#65")));

  //
  // Invalid unicode characters
  //

  // Above maximum allowed codepoint (0x10FFFF)
  EXPECT_THAT(XMLParser::Parse(R"(<node>&#xffffffff;</node>)"),
              ParseErrorIs("Invalid numeric character entity"));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#xffffffff;</node>)", optionsDisableEntity),
              ParseResultIs(RcString("&#xffffffff;")));

  EXPECT_THAT(XMLParser::Parse(R"(<node>&#x110000;</node>)"),
              ParseErrorIs("Invalid numeric character entity"));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#x110000;</node>)", optionsDisableEntity),
              ParseResultIs(RcString("&#x110000;")));

  // Surrogate codepoint (0xD800-0xDFFF)
  EXPECT_THAT(XMLParser::Parse(R"(<node>&#xd800;</node>)"),
              ParseErrorIs("Invalid numeric character entity"));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#xd800;</node>)", optionsDisableEntity),
              ParseResultIs(RcString("&#xd800;")));

  // Non-character codepoint (0xFFFE, 0xFFFF)
  EXPECT_THAT(XMLParser::Parse(R"(<node>&#xfffe;</node>)"),
              ParseErrorIs("Invalid numeric character entity"));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#xfffe;</node>)", optionsDisableEntity),
              ParseResultIs(RcString("&#xfffe;")));

  EXPECT_THAT(XMLParser::Parse(R"(<node>&#xffff;</node>)"),
              ParseErrorIs("Invalid numeric character entity"));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#xffff;</node>)", optionsDisableEntity),
              ParseResultIs(RcString("&#xffff;")));

  // Same check for base-10 path
  EXPECT_THAT(XMLParser::Parse(R"(<node>&#65535;</node>)"),
              ParseErrorIs("Invalid numeric character entity"));
  EXPECT_THAT(parseAndGetNodeContents(R"(<node>&#65535;</node>)", optionsDisableEntity),
              ParseResultIs(RcString("&#65535;")));

  // Invalid within an attribute
  EXPECT_THAT(XMLParser::Parse(R"(<node attrib="&#xfffe;" />)"),
              ParseErrorIs("Invalid numeric character entity"));
  EXPECT_THAT(XMLParser::Parse(R"(<node attrib="&#xfffe;" />)", optionsDisableEntity),
              NoParseError());
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

}  // namespace donner::xml
