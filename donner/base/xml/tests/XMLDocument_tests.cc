#include "donner/base/xml/XMLDocument.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "donner/base/ParseResult.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/base/xml/XMLParser.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/base/xml/components/XMLDocumentContext.h"

using testing::Eq;
using testing::IsEmpty;
using testing::IsNull;
using testing::NotNull;
using testing::Optional;

namespace donner::xml {

namespace {

/// Parse \p xml and return the resulting source-backed document, asserting no parse error.
XMLDocument ParseDocument(std::string_view xml) {
  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(xml);
  EXPECT_THAT(maybeDocument, NoParseError());
  return std::move(maybeDocument.result());
}

/// Returns true when \p result carries a diagnostic whose reason contains \p needle.
MATCHER_P(DiagnosticReasonContains, needle, "") {
  if (!arg.diagnostic.has_value()) {
    *result_listener << "result has no diagnostic";
    return false;
  }

  const std::string reason(arg.diagnostic->reason);
  *result_listener << "diagnostic reason is \"" << reason << "\"";
  return reason.find(std::string(needle)) != std::string::npos;
}

}  // namespace

class XMLDocumentTests : public testing::Test {};

//
// Construction / accessors.
//

TEST_F(XMLDocumentTests, DefaultConstructedDocumentHasDocumentRootAndNoSource) {
  XMLDocument doc;

  EXPECT_EQ(doc.root().type(), XMLNode::Type::Document);
  EXPECT_FALSE(doc.hasSourceStore());
  EXPECT_THAT(doc.source(), IsEmpty());
  EXPECT_EQ(doc.sourceVersion(), 0u);
  EXPECT_THAT(doc.sourceStore(), IsNull());

  // const overload also returns nullptr.
  const XMLDocument& constDoc = doc;
  EXPECT_THAT(constDoc.sourceStore(), IsNull());
}

TEST_F(XMLDocumentTests, RootEntityHandleMatchesRootNode) {
  XMLDocument doc;
  EXPECT_EQ(doc.rootEntityHandle(), doc.root().entityHandle());
}

TEST_F(XMLDocumentTests, SharedRegistryIsStable) {
  XMLDocument doc;
  EXPECT_EQ(doc.sharedRegistry().get(), &doc.registry());
}

TEST_F(XMLDocumentTests, CreateFromRegistryRehydratesSameTree) {
  XMLDocument original;
  XMLNode element = XMLNode::CreateElementNode(original, "svg");
  original.root().appendChild(element);

  XMLDocument rehydrated = XMLDocument::CreateFromRegistry(original.sharedRegistry());

  // Both facades wrap the same registry, so they see the same root and children.
  EXPECT_EQ(rehydrated.sharedRegistry().get(), original.sharedRegistry().get());
  EXPECT_EQ(rehydrated.root(), original.root());
  ASSERT_TRUE(rehydrated.root().firstChild().has_value());
  EXPECT_EQ(rehydrated.root().firstChild()->tagName(), XMLQualifiedNameRef("svg"));
}

TEST_F(XMLDocumentTests, CreateFromRegistryNullRegistryAsserts) {
  EXPECT_DEATH({ XMLDocument::CreateFromRegistry(nullptr); }, "null registry");
}

TEST_F(XMLDocumentTests, CreateFromRegistryWithoutDocumentContextAsserts) {
  auto registry = std::make_shared<Registry>();
  EXPECT_DEATH({ XMLDocument::CreateFromRegistry(registry); }, "XMLDocumentContext");
}

//
// setSource / source store.
//

TEST_F(XMLDocumentTests, SetSourceInstallsSourceStore) {
  XMLDocument doc;
  EXPECT_FALSE(doc.hasSourceStore());

  doc.setSource("<svg/>");

  EXPECT_TRUE(doc.hasSourceStore());
  EXPECT_EQ(doc.source(), "<svg/>");
  EXPECT_THAT(doc.sourceStore(), NotNull());
}

TEST_F(XMLDocumentTests, SetSourceClearsSourceDiagnostic) {
  XMLDocument doc;
  doc.setSource("<svg></svg>");

  // Seed a stale diagnostic and confirm setSource() clears it.
  auto& context = doc.registry().ctx().get<donner::xml::components::XMLDocumentContext>();
  context.sourceDiagnostic =
      ParseDiagnostic::Error("stale", SourceRange{FileOffset::Offset(0), FileOffset::Offset(1)});

  doc.setSource("<svg/>");

  EXPECT_FALSE(context.sourceDiagnostic.has_value());
}

//
// nodeAtSourceOffset / attributeAtSourceOffset.
//

TEST_F(XMLDocumentTests, NodeAtSourceOffsetWithoutSourceStoreReturnsNullopt) {
  XMLDocument doc;
  EXPECT_THAT(doc.nodeAtSourceOffset(0), Eq(std::nullopt));
}

TEST_F(XMLDocumentTests, NodeAtSourceOffsetOutOfRangeReturnsNullopt) {
  XMLDocument doc = ParseDocument(R"(<svg><rect/></svg>)");
  EXPECT_THAT(doc.nodeAtSourceOffset(doc.source().size()), Eq(std::nullopt));
  EXPECT_THAT(doc.nodeAtSourceOffset(doc.source().size() + 100), Eq(std::nullopt));
}

TEST_F(XMLDocumentTests, NodeAtSourceOffsetReturnsDeepestNode) {
  XMLDocument doc = ParseDocument(R"(<svg><rect/></svg>)");
  const std::size_t rectOffset = doc.source().find("rect");
  ASSERT_NE(rectOffset, std::string_view::npos);

  std::optional<XMLNode> node = doc.nodeAtSourceOffset(rectOffset);
  ASSERT_TRUE(node.has_value());
  EXPECT_EQ(node->tagName(), XMLQualifiedNameRef("rect"));
}

TEST_F(XMLDocumentTests, AttributeAtSourceOffsetWithoutSourceStoreReturnsNullopt) {
  XMLDocument doc;
  EXPECT_THAT(doc.attributeAtSourceOffset(0), Eq(std::nullopt));
}

TEST_F(XMLDocumentTests, AttributeAtSourceOffsetOutOfRangeReturnsNullopt) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red"/></svg>)");
  EXPECT_THAT(doc.attributeAtSourceOffset(doc.source().size()), Eq(std::nullopt));
}

TEST_F(XMLDocumentTests, AttributeAtSourceOffsetLocatesAttribute) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red"/></svg>)");
  const std::size_t fillOffset = doc.source().find("fill");
  ASSERT_NE(fillOffset, std::string_view::npos);

  std::optional<XMLAttributeAtSourceOffset> attribute = doc.attributeAtSourceOffset(fillOffset);
  ASSERT_TRUE(attribute.has_value());
  EXPECT_EQ(attribute->name, XMLQualifiedName("fill"));
}

TEST_F(XMLDocumentTests, AttributeAtSourceOffsetInWhitespaceReturnsNullopt) {
  XMLDocument doc = ParseDocument(R"(<svg> <rect/> </svg>)");
  // Offset inside the whitespace text node between <svg> and <rect> is not on an attribute.
  std::optional<XMLAttributeAtSourceOffset> attribute = doc.attributeAtSourceOffset(5);
  EXPECT_THAT(attribute, Eq(std::nullopt));
}

//
// applySourceEdit — error paths.
//

TEST_F(XMLDocumentTests, ApplySourceEditWithoutSourceStoreFails) {
  XMLDocument doc;
  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(0), FileOffset::Offset(1)},
      .replacement = "x",
      .sourceVersion = 0,
  });

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("without source text"));
}

TEST_F(XMLDocumentTests, ApplySourceEditVersionMismatchFails) {
  XMLDocument doc = ParseDocument(R"(<svg/>)");
  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(0), FileOffset::Offset(1)},
      .replacement = "x",
      .sourceVersion = doc.sourceVersion() + 99,
  });

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("Source version mismatch"));
}

TEST_F(XMLDocumentTests, ApplySourceEditInvalidRangeFails) {
  XMLDocument doc = ParseDocument(R"(<svg/>)");
  // end < start is an invalid edit range.
  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(3), FileOffset::Offset(1)},
      .replacement = "x",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("Invalid source edit range"));
}

TEST_F(XMLDocumentTests, ApplySourceEditUpdatesAttributeValueScope) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red"/></svg>)");
  const std::size_t valueOffset = doc.source().find("red");
  ASSERT_NE(valueOffset, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 3)},
      .replacement = "blue",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::AttributeValue);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_EQ(doc.source(), R"(<svg><rect fill="blue"/></svg>)");
}

TEST_F(XMLDocumentTests, ApplySourceEditTextNodeScopeUpdatesValue) {
  XMLDocument doc = ParseDocument(R"(<svg><text>hello</text></svg>)");
  const std::size_t textOffset = doc.source().find("hello");
  ASSERT_NE(textOffset, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(textOffset), FileOffset::Offset(textOffset + 5)},
      .replacement = "world",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_EQ(doc.source(), R"(<svg><text>world</text></svg>)");
}

//
// setAttribute — DOM-side structured edit.
//

TEST_F(XMLDocumentTests, SetAttributeUpdatesExistingValue) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();

  ApplySourceEditResult result = doc.setAttribute(rect, "fill", "blue");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::AttributeValue);
  EXPECT_THAT(rect.getAttribute("fill"), Optional(Eq("blue")));
  EXPECT_EQ(doc.source(), R"(<svg><rect fill="blue"/></svg>)");
}

TEST_F(XMLDocumentTests, SetAttributeInsertsNewAttribute) {
  XMLDocument doc = ParseDocument(R"(<svg><rect/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();

  ApplySourceEditResult result = doc.setAttribute(rect, "fill", "green");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_THAT(rect.getAttribute("fill"), Optional(Eq("green")));
  EXPECT_THAT(std::string(doc.source()), testing::HasSubstr(R"(fill="green")"));
}

TEST_F(XMLDocumentTests, SetAttributeOnNodeFromOtherDocumentFails) {
  XMLDocument doc = ParseDocument(R"(<svg><rect/></svg>)");
  XMLDocument other;
  XMLNode foreign = XMLNode::CreateElementNode(other, "rect");

  ApplySourceEditResult result = doc.setAttribute(foreign, "fill", "blue");

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("another document"));
}

TEST_F(XMLDocumentTests, SetAttributeWithoutSourceStoreFails) {
  XMLDocument doc;
  XMLNode element = XMLNode::CreateElementNode(doc, "rect");
  doc.root().appendChild(element);

  ApplySourceEditResult result = doc.setAttribute(element, "fill", "blue");

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("without source text"));
}

TEST_F(XMLDocumentTests, SetAttributeOnNonElementNodeFails) {
  XMLDocument doc = ParseDocument(R"(<svg>text</svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode text = svg.firstChild().value();
  ASSERT_EQ(text.type(), XMLNode::Type::Data);

  ApplySourceEditResult result = doc.setAttribute(text, "fill", "blue");

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("does not support attributes"));
}

//
// removeAttribute.
//

TEST_F(XMLDocumentTests, RemoveAttributeRemovesFromSourceAndDom) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();

  ApplySourceEditResult result = doc.removeAttribute(rect, "fill");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_FALSE(rect.hasAttribute("fill"));
  EXPECT_THAT(std::string(doc.source()), testing::Not(testing::HasSubstr("fill")));
}

TEST_F(XMLDocumentTests, RemoveAttributeNotPresentIsNoOp) {
  XMLDocument doc = ParseDocument(R"(<svg><rect/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();

  ApplySourceEditResult result = doc.removeAttribute(rect, "fill");

  // No attribute to remove: not applied, but not an error either.
  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_EQ(doc.source(), R"(<svg><rect/></svg>)");
}

TEST_F(XMLDocumentTests, RemoveAttributeOnNodeFromOtherDocumentFails) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red"/></svg>)");
  XMLDocument other;
  XMLNode foreign = XMLNode::CreateElementNode(other, "rect");

  ApplySourceEditResult result = doc.removeAttribute(foreign, "fill");

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("another document"));
}

TEST_F(XMLDocumentTests, RemoveAttributeWithoutSourceStoreFails) {
  XMLDocument doc;
  XMLNode element = XMLNode::CreateElementNode(doc, "rect");
  element.setAttribute("fill", "red");
  doc.root().appendChild(element);

  ApplySourceEditResult result = doc.removeAttribute(element, "fill");

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("without source text"));
}

//
// insertNode.
//

TEST_F(XMLDocumentTests, InsertNodeAppendsSourcelessChild) {
  XMLDocument doc = ParseDocument(R"(<svg></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");

  ApplySourceEditResult result = doc.insertNode(svg, rect);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_THAT(std::string(doc.source()), testing::HasSubstr("<rect"));
  ASSERT_TRUE(svg.firstChild().has_value());
  EXPECT_EQ(svg.firstChild()->tagName(), XMLQualifiedNameRef("rect"));
}

TEST_F(XMLDocumentTests, InsertNodeBeforeReferenceChild) {
  XMLDocument doc = ParseDocument(R"(<svg><circle/></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode circle = svg.firstChild().value();
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");

  ApplySourceEditResult result = doc.insertNode(svg, rect, circle);

  EXPECT_TRUE(result.applied);
  ASSERT_TRUE(svg.firstChild().has_value());
  EXPECT_EQ(svg.firstChild()->tagName(), XMLQualifiedNameRef("rect"));
  const std::size_t rectPos = doc.source().find("<rect");
  const std::size_t circlePos = doc.source().find("<circle");
  ASSERT_NE(rectPos, std::string_view::npos);
  ASSERT_NE(circlePos, std::string_view::npos);
  EXPECT_LT(rectPos, circlePos);
}

TEST_F(XMLDocumentTests, InsertNodeFromOtherDocumentFails) {
  XMLDocument doc = ParseDocument(R"(<svg></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLDocument other;
  XMLNode foreign = XMLNode::CreateElementNode(other, "rect");

  ApplySourceEditResult result = doc.insertNode(svg, foreign);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("another document"));
}

TEST_F(XMLDocumentTests, InsertNodeWithoutSourceStoreFails) {
  XMLDocument doc;
  XMLNode svg = XMLNode::CreateElementNode(doc, "svg");
  doc.root().appendChild(svg);
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");

  ApplySourceEditResult result = doc.insertNode(svg, rect);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("without source text"));
}

TEST_F(XMLDocumentTests, InsertNodeUnderNonElementParentFails) {
  XMLDocument doc = ParseDocument(R"(<svg/>)");
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");

  // The document root is not an element.
  ApplySourceEditResult result = doc.insertNode(doc.root(), rect);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("non-element"));
}

TEST_F(XMLDocumentTests, InsertNodeRejectsDocumentRootAsChild) {
  XMLDocument doc = ParseDocument(R"(<svg></svg>)");
  XMLNode svg = doc.root().firstChild().value();

  ApplySourceEditResult result = doc.insertNode(svg, doc.root());

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("document root"));
}

TEST_F(XMLDocumentTests, InsertNodeIntoOwnDescendantFails) {
  XMLDocument doc = ParseDocument(R"(<svg><g><inner/></g></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode g = svg.firstChild().value();
  XMLNode inner = g.firstChild().value();

  // Try to move `g` under its descendant `inner`.
  ApplySourceEditResult result = doc.insertNode(inner, g);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("descendant"));
}

TEST_F(XMLDocumentTests, InsertNodeMovesSourceBackedElement) {
  XMLDocument doc = ParseDocument(R"(<svg><a></a><b><moved/></b></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode a = svg.firstChild().value();
  XMLNode b = a.nextSibling().value();
  XMLNode moved = b.firstChild().value();
  ASSERT_EQ(moved.tagName(), XMLQualifiedNameRef("moved"));

  // Move <moved> from <b> into <a>.
  ApplySourceEditResult result = doc.insertNode(a, moved);

  EXPECT_TRUE(result.applied);
  ASSERT_TRUE(a.firstChild().has_value());
  EXPECT_EQ(a.firstChild()->tagName(), XMLQualifiedNameRef("moved"));
  EXPECT_FALSE(b.firstChild().has_value());
}

TEST_F(XMLDocumentTests, InsertNodeNoOpWhenAlreadyAtTargetPosition) {
  XMLDocument doc = ParseDocument(R"(<svg><a><child/></a></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode a = svg.firstChild().value();
  XMLNode child = a.firstChild().value();

  // child is already the last child of `a`; appending it again is a no-op.
  ApplySourceEditResult result = doc.insertNode(a, child);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
}

//
// removeNode.
//

TEST_F(XMLDocumentTests, RemoveNodeDetachesAndUpdatesSource) {
  XMLDocument doc = ParseDocument(R"(<svg><rect/></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode rect = svg.firstChild().value();

  ApplySourceEditResult result = doc.removeNode(rect);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_FALSE(svg.firstChild().has_value());
  EXPECT_THAT(std::string(doc.source()), testing::Not(testing::HasSubstr("<rect")));
  ASSERT_EQ(result.mutations.size(), 1u);
  EXPECT_EQ(result.mutations[0].kind, XMLMutation::Kind::NodeRemoved);
}

TEST_F(XMLDocumentTests, RemoveNodeFromOtherDocumentFails) {
  XMLDocument doc = ParseDocument(R"(<svg><rect/></svg>)");
  XMLDocument other;
  XMLNode foreign = XMLNode::CreateElementNode(other, "rect");

  ApplySourceEditResult result = doc.removeNode(foreign);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("another document"));
}

TEST_F(XMLDocumentTests, RemoveNodeWithoutSourceStoreFails) {
  XMLDocument doc;
  XMLNode svg = XMLNode::CreateElementNode(doc, "svg");
  doc.root().appendChild(svg);
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");
  svg.appendChild(rect);

  ApplySourceEditResult result = doc.removeNode(rect);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("without source text"));
}

TEST_F(XMLDocumentTests, RemoveDocumentRootFails) {
  XMLDocument doc = ParseDocument(R"(<svg/>)");

  ApplySourceEditResult result = doc.removeNode(doc.root());

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("document root"));
}

TEST_F(XMLDocumentTests, RemoveNodeWithoutSourceRangeFails) {
  // A source-backed document, but a freshly created node that was never serialized into source
  // has no source range, so removeNode() cannot find a span to delete.
  XMLDocument doc = ParseDocument(R"(<svg></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");
  svg.appendChild(rect);

  ApplySourceEditResult result = doc.removeNode(rect);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("without a source range"));
}

//
// ReparseScope ostream operator.
//

TEST_F(XMLDocumentTests, ReparseScopeOstreamOutput) {
  EXPECT_THAT(ReparseScope::AttributeValue, ToStringIs("AttributeValue"));
  EXPECT_THAT(ReparseScope::OpeningTag, ToStringIs("OpeningTag"));
  EXPECT_THAT(ReparseScope::TextNode, ToStringIs("TextNode"));
  EXPECT_THAT(ReparseScope::ElementSubtree, ToStringIs("ElementSubtree"));
  EXPECT_THAT(ReparseScope::Document, ToStringIs("Document"));
}

TEST_F(XMLDocumentTests, XMLMutationKindOstreamOutput) {
  EXPECT_THAT(XMLMutation::Kind::AttributeSet, ToStringIs("AttributeSet"));
  EXPECT_THAT(XMLMutation::Kind::AttributeRemoved, ToStringIs("AttributeRemoved"));
  EXPECT_THAT(XMLMutation::Kind::NodeValueChanged, ToStringIs("NodeValueChanged"));
  EXPECT_THAT(XMLMutation::Kind::NodeInserted, ToStringIs("NodeInserted"));
  EXPECT_THAT(XMLMutation::Kind::NodeRemoved, ToStringIs("NodeRemoved"));
  EXPECT_THAT(XMLMutation::Kind::SubtreeReplaced, ToStringIs("SubtreeReplaced"));
  EXPECT_THAT(XMLMutation::Kind::SourceDiagnosticChanged, ToStringIs("SourceDiagnosticChanged"));
}

}  // namespace donner::xml
