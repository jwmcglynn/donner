#include "donner/base/xml/XMLNode.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/ParseResult.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/xml/XMLParser.h"
#include "donner/base/xml/XMLQualifiedName.h"

using testing::AllOf;
using testing::ElementsAre;
using testing::Eq;
using testing::Ne;
using testing::Optional;

// TODO(jwmcglynn): Add an ErrorHighlightsText matcher

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

TEST_F(XMLNodeTests, TypeOstreamOutput) {
  EXPECT_THAT(XMLNode::Type::Document, ToStringIs("Document"));
  EXPECT_THAT(XMLNode::Type::Element, ToStringIs("Element"));
  EXPECT_THAT(XMLNode::Type::Data, ToStringIs("Data"));
  EXPECT_THAT(XMLNode::Type::CData, ToStringIs("CData"));
  EXPECT_THAT(XMLNode::Type::Comment, ToStringIs("Comment"));
  EXPECT_THAT(XMLNode::Type::DocType, ToStringIs("DocType"));
  EXPECT_THAT(XMLNode::Type::ProcessingInstruction, ToStringIs("ProcessingInstruction"));
  EXPECT_THAT(XMLNode::Type::XMLDeclaration, ToStringIs("XMLDeclaration"));
}

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

TEST_F(XMLNodeTests, TryCast) {
  XMLDocument doc;
  XMLNode node = XMLNode::CreateElementNode(doc, "test");

  EntityHandle handle = node.entityHandle();
  EXPECT_THAT(XMLNode::TryCast(handle), Optional(Eq(node)));

  // Create an unrelated entity and try to cast.
  EntityHandle unrelatedHandle(doc.registry(), doc.registry().create());
  EXPECT_THAT(XMLNode::TryCast(unrelatedHandle), Eq(std::nullopt));
}

TEST_F(XMLNodeTests, CopyAndMove) {
  XMLDocument doc;
  XMLNode node = XMLNode::CreateElementNode(doc, "test");
  XMLNode node2 = XMLNode::CreateElementNode(doc, "test2");

  // Test copy constructor
  XMLNode copy(node);
  EXPECT_EQ(copy, node);

  // Test move constructor
  XMLNode move(std::move(copy));
  EXPECT_EQ(move, node);

  // Test copy assignment
  XMLNode copyAssign = node;
  EXPECT_EQ(copyAssign, node);
  copyAssign = node2;
  EXPECT_EQ(copyAssign, node2);

  // Test move assignment
  XMLNode moveAssign = std::move(copyAssign);
  EXPECT_EQ(moveAssign, node2);
  moveAssign = node;
  EXPECT_EQ(moveAssign, node);
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

  // With an empty referenceNode
  XMLNode child4 = XMLNode::CreateElementNode(doc, "child4");
  parent.insertBefore(child4, std::nullopt);

  EXPECT_THAT(parent.lastChild(), Optional(Eq(child4)));

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

TEST_F(XMLNodeTests, TreeTraversalEmpty) {
  XMLDocument doc;
  XMLNode node = XMLNode::CreateElementNode(doc, "test");

  EXPECT_THAT(node.parentElement(), Eq(std::nullopt));
  EXPECT_THAT(node.firstChild(), Eq(std::nullopt));
  EXPECT_THAT(node.lastChild(), Eq(std::nullopt));
  EXPECT_THAT(node.previousSibling(), Eq(std::nullopt));
  EXPECT_THAT(node.nextSibling(), Eq(std::nullopt));
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

TEST_F(XMLNodeTests, SourceOffsetsNotSet) {
  XMLDocument doc;
  XMLNode element = XMLNode::CreateElementNode(doc, "test");

  EXPECT_THAT(element.sourceStartOffset(), Eq(std::nullopt));
  EXPECT_THAT(element.sourceEndOffset(), Eq(std::nullopt));
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

TEST_F(XMLNodeTests, GetNodeLocationInvalid) {
  XMLDocument doc;
  XMLNode node = XMLNode::CreateElementNode(doc, "child");
  EXPECT_THAT(node.getNodeLocation(), Eq(std::nullopt));
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

  // Test attribute not found
  auto missingLocation = child.getAttributeLocation(xml, "missing");
  EXPECT_THAT(missingLocation, Eq(std::nullopt));
}

TEST_F(XMLNodeTests, GetAttributeLocationInvalid) {
  std::string_view mismatchedXml = R"(<child attr="Hello, world!"></child>)";

  XMLDocument doc;
  XMLNode node = XMLNode::CreateElementNode(doc, "child");

  auto location = node.getAttributeLocation(mismatchedXml, "attr");
  EXPECT_THAT(location, Eq(std::nullopt));

  // Try with a source offset set
  node.setSourceEndOffset(FileOffset::Offset(42));
  EXPECT_THAT(node.getAttributeLocation(mismatchedXml, "attr"), Eq(std::nullopt));
}

// ---------------------------------------------------------------------------
// serializeToString tests
// ---------------------------------------------------------------------------

TEST_F(XMLNodeTests, SerializeToString_SelfClosingElement) {
  XMLDocument doc;
  XMLNode el = XMLNode::CreateElementNode(doc, "br");
  EXPECT_EQ(el.serializeToString(), "<br/>");
}

TEST_F(XMLNodeTests, SerializeToString_ElementWithAttributes) {
  XMLDocument doc;
  XMLNode el = XMLNode::CreateElementNode(doc, "rect");
  el.setAttribute("width", "100");
  el.setAttribute("height", "200");
  const std::string s(std::string_view(el.serializeToString()));
  // Must start with <rect, contain both attributes, and self-close.
  EXPECT_THAT(s, testing::StartsWith("<rect "));
  EXPECT_THAT(s, testing::HasSubstr("width=\"100\""));
  EXPECT_THAT(s, testing::HasSubstr("height=\"200\""));
  EXPECT_THAT(s, testing::EndsWith("/>"));
}

TEST_F(XMLNodeTests, SerializeToString_AttributeEscaping) {
  XMLDocument doc;
  XMLNode el = XMLNode::CreateElementNode(doc, "node");
  el.setAttribute("val", "a&b<c>d\"e");
  const std::string s(std::string_view(el.serializeToString()));
  EXPECT_THAT(s, testing::HasSubstr("val=\"a&amp;b&lt;c&gt;d&quot;e\""));
}

TEST_F(XMLNodeTests, SerializeToString_ElementWithTextChild) {
  XMLDocument doc;
  XMLNode el = XMLNode::CreateElementNode(doc, "text");
  XMLNode data = XMLNode::CreateDataNode(doc, "Hello, world!");
  el.appendChild(data);
  EXPECT_EQ(el.serializeToString(), "<text>Hello, world!</text>");
}

TEST_F(XMLNodeTests, SerializeToString_TextDataEscaping) {
  XMLDocument doc;
  XMLNode el = XMLNode::CreateElementNode(doc, "p");
  XMLNode data = XMLNode::CreateDataNode(doc, "a<b>&c");
  el.appendChild(data);
  EXPECT_EQ(el.serializeToString(), "<p>a&lt;b&gt;&amp;c</p>");
}

TEST_F(XMLNodeTests, SerializeToString_NestedElements) {
  XMLDocument doc;
  XMLNode parent = XMLNode::CreateElementNode(doc, "svg");
  XMLNode child = XMLNode::CreateElementNode(doc, "g");
  XMLNode grandchild = XMLNode::CreateElementNode(doc, "rect");
  child.appendChild(grandchild);
  parent.appendChild(child);

  const std::string s(std::string_view(parent.serializeToString()));
  EXPECT_EQ(s,
            "<svg>\n"
            "  <g>\n"
            "    <rect/>\n"
            "  </g>\n"
            "</svg>");
}

TEST_F(XMLNodeTests, SerializeToString_IndentLevel) {
  XMLDocument doc;
  XMLNode el = XMLNode::CreateElementNode(doc, "rect");
  // At indent level 2 the element should have 4 leading spaces.
  const std::string s(std::string_view(el.serializeToString(2)));
  EXPECT_EQ(s, "    <rect/>");
}

TEST_F(XMLNodeTests, SerializeToString_Comment) {
  XMLDocument doc;
  XMLNode comment = XMLNode::CreateCommentNode(doc, " this is a comment ");
  EXPECT_EQ(comment.serializeToString(), "<!-- this is a comment -->");
}

TEST_F(XMLNodeTests, SerializeToString_CData) {
  XMLDocument doc;
  XMLNode cdata = XMLNode::CreateCDataNode(doc, "raw <data> & more");
  EXPECT_EQ(cdata.serializeToString(), "<![CDATA[raw <data> & more]]>");
}

TEST_F(XMLNodeTests, SerializeToString_DocType) {
  XMLDocument doc;
  XMLNode dt = XMLNode::CreateDocTypeNode(doc, "svg");
  EXPECT_EQ(dt.serializeToString(), "<!DOCTYPE svg>");
}

TEST_F(XMLNodeTests, SerializeToString_ProcessingInstruction) {
  XMLDocument doc;
  XMLNode pi = XMLNode::CreateProcessingInstructionNode(doc, "xml-stylesheet",
                                                        "type=\"text/css\" href=\"style.css\"");
  EXPECT_EQ(pi.serializeToString(),
            "<?xml-stylesheet type=\"text/css\" href=\"style.css\"?>");
}

TEST_F(XMLNodeTests, SerializeToString_ProcessingInstructionNoValue) {
  XMLDocument doc;
  XMLNode pi = XMLNode::CreateProcessingInstructionNode(doc, "foo", "");
  EXPECT_EQ(pi.serializeToString(), "<?foo?>");
}

TEST_F(XMLNodeTests, SerializeToString_XMLDeclaration) {
  XMLDocument doc;
  XMLNode decl = XMLNode::CreateXMLDeclarationNode(doc);
  decl.setAttribute("version", "1.0");
  decl.setAttribute("encoding", "UTF-8");
  const std::string s(std::string_view(decl.serializeToString()));
  EXPECT_THAT(s, testing::StartsWith("<?xml "));
  EXPECT_THAT(s, testing::HasSubstr("version=\"1.0\""));
  EXPECT_THAT(s, testing::HasSubstr("encoding=\"UTF-8\""));
  EXPECT_THAT(s, testing::EndsWith("?>"));
}

TEST_F(XMLNodeTests, SerializeToString_MixedContent) {
  // Mixed content: text nodes only → inline, no block indentation.
  XMLDocument doc;
  XMLNode el = XMLNode::CreateElementNode(doc, "span");
  el.appendChild(XMLNode::CreateDataNode(doc, "Hello "));
  el.appendChild(XMLNode::CreateDataNode(doc, "world"));
  EXPECT_EQ(el.serializeToString(), "<span>Hello world</span>");
}

TEST_F(XMLNodeTests, SerializeToString_NamespacedElement) {
  XMLDocument doc;
  XMLNode el = XMLNode::CreateElementNode(doc, XMLQualifiedNameRef("xlink", "href"));
  EXPECT_EQ(el.serializeToString(), "<xlink:href/>");
}

TEST_F(XMLNodeTests, SerializeToString_NamespacedAttribute) {
  XMLDocument doc;
  XMLNode el = XMLNode::CreateElementNode(doc, "use");
  el.setAttribute(XMLQualifiedNameRef("xlink", "href"), "#icon");
  const std::string s(std::string_view(el.serializeToString()));
  EXPECT_THAT(s, testing::HasSubstr("xlink:href=\"#icon\""));
}

// ---------------------------------------------------------------------------
// Round-trip test
// ---------------------------------------------------------------------------

namespace {

/// Find the first child of `parent` that is an Element node, skipping whitespace Data nodes that
/// are injected by the serializer's block indentation.
std::optional<XMLNode> FirstElementChild(const XMLNode& parent) {
  for (std::optional<XMLNode> child = parent.firstChild(); child.has_value();
       child = child->nextSibling()) {
    if (child->type() == XMLNode::Type::Element) {
      return child;
    }
  }
  return std::nullopt;
}

/// Find the next sibling that is an Element node.
std::optional<XMLNode> NextElementSibling(const XMLNode& node) {
  for (std::optional<XMLNode> sib = node.nextSibling(); sib.has_value();
       sib = sib->nextSibling()) {
    if (sib->type() == XMLNode::Type::Element) {
      return sib;
    }
  }
  return std::nullopt;
}

}  // namespace

TEST_F(XMLNodeTests, SerializeToString_RoundTrip) {
  // Build a tree programmatically, serialize it, then re-parse and verify structure.
  XMLDocument doc;
  XMLNode root = XMLNode::CreateElementNode(doc, "svg");
  root.setAttribute("xmlns", "http://www.w3.org/2000/svg");
  root.setAttribute("width", "100");

  XMLNode g = XMLNode::CreateElementNode(doc, "g");
  g.setAttribute("id", "layer1");

  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");
  rect.setAttribute("x", "10");
  rect.setAttribute("y", "20");

  XMLNode text = XMLNode::CreateElementNode(doc, "text");
  text.appendChild(XMLNode::CreateDataNode(doc, "Hello & <world>"));

  g.appendChild(rect);
  g.appendChild(text);
  root.appendChild(g);

  const RcString serialized = root.serializeToString();

  // Re-parse the serialized string.
  ParseResult<XMLDocument> maybeDoc = XMLParser::Parse(serialized);
  ASSERT_THAT(maybeDoc, NoParseError());

  XMLDocument reparsed = std::move(maybeDoc.result());
  XMLNode reparsedRoot = reparsed.root();

  // The document root should have one child (the <svg> element).
  // Note: block indentation adds whitespace Data nodes around element children, so we
  // use FirstElementChild / NextElementSibling to skip them.
  ASSERT_THAT(reparsedRoot.firstChild(), Ne(std::nullopt));
  XMLNode svgEl = *reparsedRoot.firstChild();
  EXPECT_THAT(svgEl.tagName(), Eq("svg"));
  EXPECT_THAT(svgEl.getAttribute("width"), Optional(Eq("100")));

  // <svg> → first element child is <g>
  std::optional<XMLNode> maybeGEl = FirstElementChild(svgEl);
  ASSERT_THAT(maybeGEl, Ne(std::nullopt));
  XMLNode gEl = *maybeGEl;
  EXPECT_THAT(gEl.tagName(), Eq("g"));
  EXPECT_THAT(gEl.getAttribute("id"), Optional(Eq("layer1")));

  // <g> → first element child is <rect>
  std::optional<XMLNode> maybeRectEl = FirstElementChild(gEl);
  ASSERT_THAT(maybeRectEl, Ne(std::nullopt));
  XMLNode rectEl = *maybeRectEl;
  EXPECT_THAT(rectEl.tagName(), Eq("rect"));
  EXPECT_THAT(rectEl.getAttribute("x"), Optional(Eq("10")));
  EXPECT_THAT(rectEl.getAttribute("y"), Optional(Eq("20")));

  // <g> → second element child is <text>
  std::optional<XMLNode> maybeTextEl = NextElementSibling(rectEl);
  ASSERT_THAT(maybeTextEl, Ne(std::nullopt));
  XMLNode textEl = *maybeTextEl;
  EXPECT_THAT(textEl.tagName(), Eq("text"));

  // <text> → first child is a Data node with unescaped value (no block indent — text-only children)
  ASSERT_THAT(textEl.firstChild(), Ne(std::nullopt));
  XMLNode dataNode = *textEl.firstChild();
  EXPECT_EQ(dataNode.type(), XMLNode::Type::Data);
  EXPECT_THAT(dataNode.value(), Optional(Eq("Hello & <world>")));
}

}  // namespace donner::xml
