#include "donner/base/xml/XMLNode.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/ParseResult.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/xml/XMLParser.h"
#include "donner/base/xml/XMLQualifiedName.h"

using testing::AllOf;
using testing::ElementsAre;
using testing::Eq;
using testing::Ne;
using testing::Optional;

// TODO: Add an ErrorHighlightsText matcher

namespace donner::xml {

class XMLNodeTests : public testing::Test {
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

  // Helper to create a basic document with a single element
  XMLDocument createTestDocument() {
    XMLDocument doc;
    XMLNode element = XMLNode::CreateElementNode(doc, "test");
    doc.root().appendChild(element);
    return doc;
  }

private:
  XMLDocument document_;
};

TEST_F(XMLNodeTests, CreateElementNode) {
  XMLDocument doc;
  XMLNode element = XMLNode::CreateElementNode(doc, "test");

  EXPECT_EQ(element.type(), XMLNode::Type::Element);
  EXPECT_THAT(element.tagName(), Eq("test"));
  EXPECT_THAT(element.value(), Eq(std::nullopt));
}

TEST_F(XMLNodeTests, CreateDataNode) {
  XMLDocument doc;
  const char* testValue = "Hello, world!";
  XMLNode node = XMLNode::CreateDataNode(doc, testValue);

  EXPECT_EQ(node.type(), XMLNode::Type::Data);
  EXPECT_THAT(node.tagName(), Eq(""));
  EXPECT_THAT(node.value(), Optional(Eq(testValue)));
}

TEST_F(XMLNodeTests, TreeManipulation) {
  XMLDocument doc;
  XMLNode parent = XMLNode::CreateElementNode(doc, "parent");
  XMLNode child1 = XMLNode::CreateElementNode(doc, "child1");
  XMLNode child2 = XMLNode::CreateElementNode(doc, "child2");

  // Test appendChild
  parent.appendChild(child1);
  parent.appendChild(child2);

  EXPECT_THAT(parent.firstChild(), Optional(Eq(child1)));
  EXPECT_THAT(parent.lastChild(), Optional(Eq(child2)));
  EXPECT_THAT(child1.nextSibling(), Optional(Eq(child2)));
  EXPECT_THAT(child2.previousSibling(), Optional(Eq(child1)));
  EXPECT_THAT(child1.parentElement(), Optional(Eq(parent)));
  EXPECT_THAT(child2.parentElement(), Optional(Eq(parent)));

  // Test insertBefore
  XMLNode child3 = XMLNode::CreateElementNode(doc, "child3");
  parent.insertBefore(child3, child2);

  EXPECT_THAT(child1.nextSibling(), Optional(Eq(child3)));
  EXPECT_THAT(child3.nextSibling(), Optional(Eq(child2)));

  // Test removeChild
  parent.removeChild(child3);
  EXPECT_THAT(child1.nextSibling(), Optional(Eq(child2)));
  EXPECT_THAT(child3.parentElement(), Eq(std::nullopt));

  // Test replaceChild
  XMLNode replacement = XMLNode::CreateElementNode(doc, "replacement");
  parent.replaceChild(replacement, child1);

  EXPECT_THAT(parent.firstChild(), Optional(Eq(replacement)));
  EXPECT_THAT(child1.parentElement(), Eq(std::nullopt));
}

TEST_F(XMLNodeTests, AttributeHandling) {
  auto maybeRoot = parseAndGetFirstNode(R"(<root attr1="value1" attr2="value2"></root>)");
  ASSERT_TRUE(maybeRoot.has_value());
  XMLNode root = std::move(maybeRoot.value());

  // Test attribute existence
  EXPECT_TRUE(root.hasAttribute("attr1"));
  EXPECT_TRUE(root.hasAttribute("attr2"));
  EXPECT_FALSE(root.hasAttribute("attr3"));

  // Test attribute values
  EXPECT_THAT(root.getAttribute("attr1"), Optional(Eq("value1")));
  EXPECT_THAT(root.getAttribute("attr2"), Optional(Eq("value2")));
  EXPECT_THAT(root.getAttribute("attr3"), Eq(std::nullopt));

  // Test setting attributes
  root.setAttribute("attr3", "value3");
  EXPECT_THAT(root.getAttribute("attr3"), Optional(Eq("value3")));

  // Test removing attributes
  root.removeAttribute("attr1");
  EXPECT_FALSE(root.hasAttribute("attr1"));
}

TEST_F(XMLNodeTests, NodeTypes) {
  XMLDocument doc;

  // Test various node types
  XMLNode element = XMLNode::CreateElementNode(doc, "element");
  EXPECT_EQ(element.type(), XMLNode::Type::Element);

  XMLNode data = XMLNode::CreateDataNode(doc, "data");
  EXPECT_EQ(data.type(), XMLNode::Type::Data);

  XMLNode cdata = XMLNode::CreateCDataNode(doc, "cdata");
  EXPECT_EQ(cdata.type(), XMLNode::Type::CData);

  XMLNode comment = XMLNode::CreateCommentNode(doc, "comment");
  EXPECT_EQ(comment.type(), XMLNode::Type::Comment);

  XMLNode doctype = XMLNode::CreateDocTypeNode(doc, "doctype");
  EXPECT_EQ(doctype.type(), XMLNode::Type::DocType);

  XMLNode pi = XMLNode::CreateProcessingInstructionNode(doc, "target", "value");
  EXPECT_EQ(pi.type(), XMLNode::Type::ProcessingInstruction);

  XMLNode xmlDecl = XMLNode::CreateXMLDeclarationNode(doc);
  EXPECT_EQ(xmlDecl.type(), XMLNode::Type::XMLDeclaration);
}

TEST_F(XMLNodeTests, SourceOffsets) {
  XMLDocument doc;
  XMLNode element = XMLNode::CreateElementNode(doc, "test");

  auto start = FileOffset::Offset(42);
  auto end = FileOffset::Offset(100);

  element.setSourceStartOffset(start);
  element.setSourceEndOffset(end);

  EXPECT_THAT(element.sourceStartOffset(), Optional(Eq(start)));
  EXPECT_THAT(element.sourceEndOffset(), Optional(Eq(end)));
}

TEST_F(XMLNodeTests, NodeEquality) {
  XMLDocument doc;
  XMLNode node1 = XMLNode::CreateElementNode(doc, "test");
  XMLNode node2 = node1;  // NOLINT, Create a copy
  XMLNode node3 = XMLNode::CreateElementNode(doc, "test");

  EXPECT_EQ(node1, node2);  // Same underlying node
  EXPECT_NE(node1, node3);  // Different nodes
}

TEST_F(XMLNodeTests, GetNodeLocation) {
  std::string_view xml = R"(<root><child attr="Hello, world!"></child></root>)";

  auto maybeRoot = parseAndGetFirstNode(xml);
  ASSERT_TRUE(maybeRoot.has_value());

  XMLNode root = std::move(maybeRoot.value());
  {
    EXPECT_EQ(root.type(), XMLNode::Type::Element);
    EXPECT_THAT(root.tagName(), Eq("root"));

    auto location = root.getNodeLocation();
    ASSERT_THAT(location, Ne(std::nullopt));

    // Extract substring from the returned offsets.
    const size_t start = location->start.offset.value();
    const size_t end = location->end.offset.value();
    EXPECT_LT(start, end);
    std::string_view nodeSubstr = xml.substr(start, end - start);

    EXPECT_THAT(nodeSubstr, Eq(xml));
  }

  auto maybeChild = root.firstChild();
  ASSERT_TRUE(maybeChild.has_value());

  XMLNode child = std::move(maybeChild.value());
  {
    EXPECT_EQ(child.type(), XMLNode::Type::Element);
    EXPECT_THAT(child.tagName(), Eq("child"));
    auto childLocation = child.getNodeLocation();
    ASSERT_THAT(childLocation, Ne(std::nullopt));

    const size_t childStart = childLocation->start.offset.value();
    const size_t childEnd = childLocation->end.offset.value();
    EXPECT_LT(childStart, childEnd);
    std::string_view childSubstr = xml.substr(childStart, childEnd - childStart);

    EXPECT_THAT(childSubstr, Eq(R"(<child attr="Hello, world!"></child>)"));
  }
}

TEST_F(XMLNodeTests, GetAttributeLocation) {
  std::string_view xml = R"(<child attr="Hello, world!"></child>)";

  auto maybeChild = parseAndGetFirstNode(xml);
  ASSERT_TRUE(maybeChild.has_value());

  XMLNode child = std::move(maybeChild.value());
  auto location = child.getAttributeLocation(xml, "attr");
  ASSERT_THAT(location, Ne(std::nullopt));

  const size_t start = location->start.offset.value();
  const size_t end = location->end.offset.value();
  EXPECT_LT(start, end);
  std::string_view foundAttribute = xml.substr(start, end - start);

  EXPECT_THAT(foundAttribute, Eq(R"(attr="Hello, world!")"));
}

}  // namespace donner::xml
