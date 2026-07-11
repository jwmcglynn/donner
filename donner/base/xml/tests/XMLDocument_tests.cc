#include "donner/base/xml/XMLDocument.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseResult.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/base/xml/XMLParser.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/base/xml/components/TreeMutationContext.h"
#include "donner/base/xml/components/XMLDocumentContext.h"

using testing::Eq;
using testing::IsEmpty;
using testing::IsNull;
using testing::NotNull;
using testing::Optional;

namespace donner::xml {

namespace internal {

bool IsXmlWhitespace(char ch);
void AppendQualifiedName(std::string& out, const XMLQualifiedNameRef& name);
bool ContainsOffset(const SourceRange& range, std::size_t offset);
std::optional<std::size_t> FindOpeningTagEnd(std::string_view source, std::size_t tagStart);
std::optional<std::size_t> FindDirtyOpeningTagEnd(std::string_view source, std::size_t tagStart,
                                                  std::size_t nodeEnd);
std::optional<std::size_t> FindClosingTagStart(std::string_view source, std::size_t nodeEnd);
bool IsRawTextLikeNode(XMLNode::Type type);
std::string SerializeAttributeInsertion(std::string_view source, std::size_t insertionOffset,
                                        const XMLQualifiedName& name,
                                        std::string_view escapedValue);
std::optional<SourceRange> ShiftRangeLeft(SourceRange range, std::size_t amount);
bool IsXmlSpace(char ch);
std::string ClosingTagFor(const XMLNode& node);
bool SourceHasClosingTagAt(std::string_view source, std::size_t offset,
                           std::string_view closingTag);
std::optional<std::size_t> FindParentClosingTagInsertionOffset(const XMLDocument& document,
                                                               const XMLNode& parent);
SourceRange OffsetRange(SourceRange range, std::size_t sourceOffsetBase);
std::optional<RcString> ElementId(const XMLNode& node);
std::optional<std::size_t> FindReusableChild(const XMLNode& parsedChild,
                                             const std::vector<XMLNode>& oldChildren,
                                             const std::vector<bool>& usedChildren);
bool HasCompatibleNodeIdentity(const XMLNode& target, const XMLNode& parsedNode);

}  // namespace internal

namespace {

/// Parse \p xml and return the resulting source-backed document, asserting no parse error.
XMLDocument ParseDocument(std::string_view xml) {
  ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(xml);
  EXPECT_THAT(maybeDocument, NoParseError());
  return std::move(maybeDocument.result());
}

/// The \p index-th element child of \p parent, skipping any whitespace/text nodes. Used by
/// the source-formatting tests, whose fixtures are laid out across indented lines and thus
/// carry inter-element whitespace text nodes.
XMLNode ElementChild(const XMLNode& parent, std::size_t index) {
  std::size_t seen = 0;
  for (std::optional<XMLNode> child = parent.firstChild(); child.has_value();
       child = child->nextSibling()) {
    if (child->type() != XMLNode::Type::Element) {
      continue;
    }
    if (seen == index) {
      return *child;
    }
    ++seen;
  }
  ADD_FAILURE() << "no element child at index " << index;
  return parent;
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

std::vector<XMLMutation::Kind> MutationKinds(const ApplySourceEditResult& result) {
  std::vector<XMLMutation::Kind> kinds;
  kinds.reserve(result.mutations.size());
  for (const XMLMutation& mutation : result.mutations) {
    kinds.push_back(mutation.kind);
  }
  return kinds;
}

void ExpectSourceRangeOffsets(const SourceRange& range, std::size_t expectedStart,
                              std::size_t expectedEnd) {
  ASSERT_TRUE(range.start.offset.has_value());
  ASSERT_TRUE(range.end.offset.has_value());
  EXPECT_EQ(*range.start.offset, expectedStart);
  EXPECT_EQ(*range.end.offset, expectedEnd);
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

TEST_F(XMLDocumentTests, CreateFromRegistryInstallsMissingTreeMutationContext) {
  XMLDocument original;
  XMLNode element = XMLNode::CreateElementNode(original, "svg");
  original.root().appendChild(element);
  original.registry().ctx().erase<donner::components::TreeMutationContext>();

  XMLDocument rehydrated = XMLDocument::CreateFromRegistry(original.sharedRegistry());

  EXPECT_TRUE(rehydrated.registry().ctx().contains<donner::components::TreeMutationContext>());
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

TEST_F(XMLDocumentTests, InternalWhitespaceAndNameHelpersHandleBoundaryCases) {
  EXPECT_TRUE(internal::IsXmlWhitespace(' '));
  EXPECT_TRUE(internal::IsXmlWhitespace('\t'));
  EXPECT_TRUE(internal::IsXmlWhitespace('\n'));
  EXPECT_TRUE(internal::IsXmlWhitespace('\r'));
  EXPECT_FALSE(internal::IsXmlWhitespace('\f'));

  EXPECT_TRUE(internal::IsXmlSpace(' '));
  EXPECT_TRUE(internal::IsXmlSpace('\t'));
  EXPECT_TRUE(internal::IsXmlSpace('\n'));
  EXPECT_TRUE(internal::IsXmlSpace('\r'));
  EXPECT_FALSE(internal::IsXmlSpace('x'));

  std::string name;
  internal::AppendQualifiedName(name, XMLQualifiedNameRef("rect"));
  EXPECT_EQ(name, "rect");

  name.clear();
  internal::AppendQualifiedName(name, XMLQualifiedNameRef("xlink", "href"));
  EXPECT_EQ(name, "xlink:href");
}

TEST_F(XMLDocumentTests, InternalOffsetHelpersHandleMissingAndShiftedOffsets) {
  const SourceRange missingStart{FileOffset::EndOfString(), FileOffset::Offset(4)};
  const SourceRange missingEnd{FileOffset::Offset(2), FileOffset::EndOfString()};
  const SourceRange range{FileOffset::Offset(2), FileOffset::Offset(5)};

  EXPECT_FALSE(internal::ContainsOffset(missingStart, 2));
  EXPECT_FALSE(internal::ContainsOffset(missingEnd, 2));
  EXPECT_TRUE(internal::ContainsOffset(range, 2));
  EXPECT_FALSE(internal::ContainsOffset(range, 5));

  EXPECT_THAT(internal::ShiftRangeLeft(missingStart, 1), Eq(std::nullopt));
  EXPECT_THAT(internal::ShiftRangeLeft(range, 3), Eq(std::nullopt));
  std::optional<SourceRange> shifted = internal::ShiftRangeLeft(range, 1);
  ASSERT_TRUE(shifted.has_value());
  ExpectSourceRangeOffsets(*shifted, 1, 4);

  EXPECT_EQ(internal::OffsetRange(missingStart, 10), missingStart);
  ExpectSourceRangeOffsets(internal::OffsetRange(range, 10), 12, 15);
}

TEST_F(XMLDocumentTests, InternalTagScannersHandleMalformedAndQuotedTags) {
  EXPECT_THAT(internal::FindOpeningTagEnd(R"(<rect data='a>b' label="c>d"/>)", 0),
              Optional(Eq(30u)));
  EXPECT_THAT(internal::FindOpeningTagEnd("rect/>", 0), Eq(std::nullopt));
  EXPECT_THAT(internal::FindOpeningTagEnd("<rect <bad>", 0), Eq(std::nullopt));
  EXPECT_THAT(internal::FindOpeningTagEnd("<rect", 0), Eq(std::nullopt));
  EXPECT_THAT(internal::FindOpeningTagEnd("<rect/>", 99), Eq(std::nullopt));

  EXPECT_THAT(internal::FindDirtyOpeningTagEnd("<rect><tail", 0, 6), Optional(Eq(6u)));
  EXPECT_THAT(internal::FindDirtyOpeningTagEnd("", 0, 0), Eq(std::nullopt));
  EXPECT_THAT(internal::FindDirtyOpeningTagEnd("<rect>", 0, 0), Eq(std::nullopt));
  EXPECT_THAT(internal::FindDirtyOpeningTagEnd("<rect", 0, 5), Eq(std::nullopt));

  EXPECT_THAT(internal::FindClosingTagStart("<a></a>", 0), Eq(std::nullopt));
  EXPECT_THAT(internal::FindClosingTagStart("<a></a>", 99), Eq(std::nullopt));
  EXPECT_THAT(internal::FindClosingTagStart("<a><b/></a>", 11), Optional(Eq(7u)));
  EXPECT_TRUE(internal::SourceHasClosingTagAt("<a></a>", 3, "</a>"));
  EXPECT_FALSE(internal::SourceHasClosingTagAt("<a></a>", 99, "</a>"));
  EXPECT_FALSE(internal::SourceHasClosingTagAt("<a></a>", 3, "</b>"));
}

TEST_F(XMLDocumentTests, InternalAttributeAndClosingTagSerializersUseXmlNames) {
  EXPECT_EQ(internal::SerializeAttributeInsertion("<rect", 5, XMLQualifiedName("fill"), "red"),
            R"( fill="red")");
  EXPECT_EQ(internal::SerializeAttributeInsertion("<rect ", 6, XMLQualifiedName("fill"), "red"),
            R"(fill="red")");
  EXPECT_EQ(internal::SerializeAttributeInsertion(
                "<use", 4, XMLQualifiedName(RcString("xlink"), RcString("href")), "#icon"),
            R"( xlink:href="#icon")");

  XMLDocument doc;
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");
  XMLNode use =
      XMLNode::CreateElementNode(doc, XMLQualifiedNameRef(RcString("xlink"), RcString("use")));

  EXPECT_EQ(internal::ClosingTagFor(rect), "</rect>");
  EXPECT_EQ(internal::ClosingTagFor(use), "</xlink:use>");
}

TEST_F(XMLDocumentTests, InternalClosingTagInsertionOffsetUsesCachedOrScannedTag) {
  XMLDocument doc = ParseDocument(R"(<svg><g></g></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  const std::size_t closingOffset = doc.source().find("</svg>");
  ASSERT_NE(closingOffset, std::string_view::npos);

  EXPECT_THAT(internal::FindParentClosingTagInsertionOffset(doc, svg), Optional(Eq(closingOffset)));

  svg.setClosingTagLocation(SourceRange{FileOffset::Offset(0), FileOffset::Offset(0)});
  EXPECT_THAT(internal::FindParentClosingTagInsertionOffset(doc, svg), Optional(Eq(closingOffset)));

  svg.setSourceEndOffset(FileOffset::Offset(3));
  EXPECT_THAT(internal::FindParentClosingTagInsertionOffset(doc, svg), Eq(std::nullopt));
}

TEST_F(XMLDocumentTests, InternalNodeIdentityHelpersMatchReusableElementsByStableId) {
  XMLDocument document;
  XMLNode rect = XMLNode::CreateElementNode(document, "rect");
  rect.setAttribute("id", "keep");
  XMLNode duplicate = XMLNode::CreateElementNode(document, "rect");
  duplicate.setAttribute("id", "keep");
  XMLNode circle = XMLNode::CreateElementNode(document, "circle");
  circle.setAttribute("id", "keep");
  XMLNode emptyId = XMLNode::CreateElementNode(document, "rect");
  emptyId.setAttribute("id", "");
  XMLNode text = XMLNode::CreateDataNode(document, "text");

  EXPECT_THAT(internal::ElementId(rect), Optional(Eq("keep")));
  EXPECT_THAT(internal::ElementId(text), Eq(std::nullopt));
  EXPECT_THAT(internal::FindReusableChild(rect, {circle, duplicate}, {false, false}),
              Optional(Eq(1u)));
  EXPECT_THAT(internal::FindReusableChild(rect, {circle, duplicate}, {false, true}),
              Eq(std::nullopt));
  EXPECT_THAT(internal::FindReusableChild(emptyId, {rect}, {false}), Eq(std::nullopt));

  EXPECT_TRUE(internal::IsRawTextLikeNode(XMLNode::Type::CData));
  EXPECT_TRUE(internal::IsRawTextLikeNode(XMLNode::Type::Comment));
  EXPECT_TRUE(internal::IsRawTextLikeNode(XMLNode::Type::ProcessingInstruction));
  EXPECT_FALSE(internal::IsRawTextLikeNode(XMLNode::Type::Data));

  EXPECT_TRUE(internal::HasCompatibleNodeIdentity(rect, duplicate));
  EXPECT_FALSE(internal::HasCompatibleNodeIdentity(rect, circle));
  EXPECT_FALSE(internal::HasCompatibleNodeIdentity(rect, text));
  EXPECT_TRUE(internal::HasCompatibleNodeIdentity(text, XMLNode::CreateDataNode(document, "new")));
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

TEST_F(XMLDocumentTests, NodeAtSourceOffsetReturnsParentAtChildEndBoundary) {
  XMLDocument doc = ParseDocument(R"(<svg><rect/></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode rect = svg.firstChild().value();
  std::optional<SourceRange> rectLocation = rect.getNodeLocation();
  ASSERT_TRUE(rectLocation.has_value());
  ASSERT_TRUE(rectLocation->end.offset.has_value());

  std::optional<XMLNode> node = doc.nodeAtSourceOffset(*rectLocation->end.offset);

  ASSERT_TRUE(node.has_value());
  EXPECT_EQ(*node, svg);
}

TEST_F(XMLDocumentTests, NodeAtSourceOffsetSkipsNodesWithClearedSourceLocations) {
  XMLDocument doc = ParseDocument(R"(<svg><g><rect/></g></svg>)");
  XMLNode group = doc.root().firstChild()->firstChild().value();
  XMLNode rect = group.firstChild().value();
  const std::size_t rectOffset = doc.source().find("rect");
  ASSERT_NE(rectOffset, std::string_view::npos);
  rect.clearSourceLocation();

  std::optional<XMLNode> node = doc.nodeAtSourceOffset(rectOffset);

  ASSERT_TRUE(node.has_value());
  EXPECT_EQ(*node, group);
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

TEST_F(XMLDocumentTests, AttributeAtSourceOffsetFallsBackToOpeningTagScan) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill = 'red' /></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  rect.clearAttributeSourceLocation("fill");
  const std::size_t valueOffset = doc.source().find("red");
  ASSERT_NE(valueOffset, std::string_view::npos);

  std::optional<XMLAttributeAtSourceOffset> attribute = doc.attributeAtSourceOffset(valueOffset);

  ASSERT_TRUE(attribute.has_value());
  EXPECT_EQ(attribute->node, rect);
  EXPECT_EQ(attribute->name, XMLQualifiedName("fill"));
  EXPECT_EQ(attribute->quote, '\'');
  ASSERT_TRUE(attribute->valueLocation.start.offset.has_value());
  ASSERT_TRUE(attribute->valueLocation.end.offset.has_value());
  EXPECT_EQ(*attribute->valueLocation.start.offset, valueOffset);
  EXPECT_EQ(*attribute->valueLocation.end.offset, valueOffset + 3);
}

TEST_F(XMLDocumentTests, AttributeAtSourceOffsetInWhitespaceReturnsNullopt) {
  XMLDocument doc = ParseDocument(R"(<svg> <rect/> </svg>)");
  // Offset inside the whitespace text node between <svg> and <rect> is not on an attribute.
  std::optional<XMLAttributeAtSourceOffset> attribute = doc.attributeAtSourceOffset(5);
  EXPECT_THAT(attribute, Eq(std::nullopt));
}

TEST_F(XMLDocumentTests, AttributeAtSourceOffsetWithSourceOnlyDocumentReturnsNullopt) {
  XMLDocument doc;
  doc.setSource(R"(<svg fill="red"/>)");

  EXPECT_THAT(doc.attributeAtSourceOffset(5), Eq(std::nullopt));
}

TEST_F(XMLDocumentTests, AttributeAtSourceOffsetIgnoresMalformedFallbackAttributeSpan) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red" stroke="blue"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  rect.clearAttributeSourceLocation("fill");
  const std::size_t fillOffset = doc.source().find("fill");
  ASSERT_NE(fillOffset, std::string_view::npos);
  ASSERT_TRUE(doc.sourceStore()
                  ->replace(fillOffset, std::string_view(R"(fill="red")").size(), "fill=")
                  .has_value());

  EXPECT_THAT(doc.attributeAtSourceOffset(fillOffset), Eq(std::nullopt));
}

TEST_F(XMLDocumentTests, AttributeAtSourceOffsetIgnoresFallbackAttributeWithUnquotedValue) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red" stroke="blue"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  rect.clearAttributeSourceLocation("fill");
  const std::size_t fillOffset = doc.source().find("fill");
  ASSERT_NE(fillOffset, std::string_view::npos);
  ASSERT_TRUE(doc.sourceStore()
                  ->replace(fillOffset, std::string_view(R"(fill="red")").size(), "fill=red")
                  .has_value());

  EXPECT_THAT(doc.attributeAtSourceOffset(fillOffset), Eq(std::nullopt));
}

TEST_F(XMLDocumentTests, AttributeAtSourceOffsetIgnoresFallbackAttributeWithMismatchedQuote) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red" stroke="blue"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  rect.clearAttributeSourceLocation("fill");
  const std::size_t fillOffset = doc.source().find("fill");
  ASSERT_NE(fillOffset, std::string_view::npos);
  ASSERT_TRUE(doc.sourceStore()
                  ->replace(fillOffset, std::string_view(R"(fill="red")").size(), "fill='red\"")
                  .has_value());

  EXPECT_THAT(doc.attributeAtSourceOffset(fillOffset), Eq(std::nullopt));
}

TEST_F(XMLDocumentTests, AttributeAtSourceOffsetIgnoresFallbackAttributeWithoutEquals) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red" stroke="blue"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  rect.clearAttributeSourceLocation("fill");
  const std::size_t fillOffset = doc.source().find("fill");
  ASSERT_NE(fillOffset, std::string_view::npos);
  ASSERT_TRUE(doc.sourceStore()
                  ->replace(fillOffset, std::string_view(R"(fill="red")").size(), "fill")
                  .has_value());

  EXPECT_THAT(doc.attributeAtSourceOffset(fillOffset), Eq(std::nullopt));
}

//
// applySourceEdit - error paths.
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

TEST_F(XMLDocumentTests, ApplySourceEditRangePastSourceEndFails) {
  XMLDocument doc = ParseDocument(R"(<svg/>)");

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(0), FileOffset::Offset(doc.source().size() + 1)},
      .replacement = "x",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("Invalid source edit range"));
}

TEST_F(XMLDocumentTests, ApplySourceEditUnresolvedRangeFails) {
  XMLDocument doc = ParseDocument(R"(<svg/>)");

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::EndOfString(), FileOffset::EndOfString()},
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

TEST_F(XMLDocumentTests, ApplySourceEditAttributeValueUsesFallbackSourceRange) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  rect.clearAttributeSourceLocation("fill");
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
  EXPECT_THAT(rect.getAttribute("fill"), Optional(Eq("blue")));
  EXPECT_EQ(doc.source(), R"(<svg><rect fill="blue"/></svg>)");
}

TEST_F(XMLDocumentTests, ApplySourceEditMalformedAttributeValueKeepsDomAndReportsDiagnostic) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  const std::size_t valueOffset = doc.source().find("red");
  ASSERT_NE(valueOffset, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 3)},
      .replacement = "bad\"",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::AttributeValue);
  EXPECT_THAT(result, DiagnosticReasonContains("Node not closed"));
  EXPECT_THAT(rect.getAttribute("fill"), Optional(Eq("red")));
  EXPECT_THAT(MutationKinds(result),
              testing::ElementsAre(XMLMutation::Kind::SourceDiagnosticChanged));
}

TEST_F(XMLDocumentTests, ApplySourceEditAttributeValueReportsInvalidatedOpeningTagRange) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  const std::size_t attributeOffset = doc.source().find("fill");
  ASSERT_NE(attributeOffset, std::string_view::npos);
  const std::size_t attributeEnd = attributeOffset + std::string_view(R"(fill="red")").size();
  rect.setAttributeSourceLocation(
      "fill", SourceRange{FileOffset::Offset(attributeOffset), FileOffset::Offset(attributeEnd)},
      SourceRange{FileOffset::Offset(attributeOffset), FileOffset::Offset(attributeEnd)}, '"');

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(attributeOffset), FileOffset::Offset(attributeEnd)},
      .replacement = R"(fool="red")",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::AttributeValue);
  EXPECT_THAT(result, DiagnosticReasonContains("opening tag malformed"));
  EXPECT_THAT(rect.getAttribute("fill"), Optional(Eq("red")));
  EXPECT_EQ(doc.source(), R"(<svg><rect fool="red"/></svg>)");
}

TEST_F(XMLDocumentTests, ApplySourceEditAttributeValueReportsRemovedTargetAttribute) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red" /></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  const std::size_t attributeOffset = doc.source().find("fill");
  ASSERT_NE(attributeOffset, std::string_view::npos);
  const std::size_t attributeEnd = attributeOffset + std::string_view(R"(fill="red")").size();
  rect.setAttributeSourceLocation(
      "fill",
      SourceRange{FileOffset::Offset(attributeOffset), FileOffset::Offset(attributeEnd + 1)},
      SourceRange{FileOffset::Offset(attributeOffset), FileOffset::Offset(attributeEnd)}, '"');

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(attributeOffset), FileOffset::Offset(attributeEnd)},
      .replacement = R"(stroke="blue")",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::AttributeValue);
  EXPECT_THAT(result, DiagnosticReasonContains("removed the target attribute"));
  EXPECT_THAT(rect.getAttribute("fill"), Optional(Eq("red")));
  EXPECT_EQ(doc.source(), R"(<svg><rect stroke="blue" /></svg>)");
}

TEST_F(XMLDocumentTests, ApplySourceEditRepeatedDiagnosticDoesNotEmitDuplicateMutation) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red"/></svg>)");
  const std::size_t valueOffset = doc.source().find("red");
  ASSERT_NE(valueOffset, std::string_view::npos);

  ApplySourceEditResult first = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 3)},
      .replacement = "bad\"",
      .sourceVersion = doc.sourceVersion(),
  });
  ASSERT_TRUE(first.applied);
  ASSERT_TRUE(first.diagnostic.has_value());

  const std::size_t updatedValueOffset = doc.source().find("bad");
  ASSERT_NE(updatedValueOffset, std::string_view::npos);
  ApplySourceEditResult repeated = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(updatedValueOffset),
                           FileOffset::Offset(updatedValueOffset + 4)},
      .replacement = "bad\"",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(repeated.applied);
  EXPECT_EQ(repeated.scope, ReparseScope::AttributeValue);
  EXPECT_THAT(repeated.diagnostic, Eq(first.diagnostic));
  EXPECT_THAT(MutationKinds(repeated), IsEmpty());
}

TEST_F(XMLDocumentTests, ApplySourceEditChangedDiagnosticRangeEmitsMutation) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red"/></svg>)");
  const std::size_t valueOffset = doc.source().find("red");
  ASSERT_NE(valueOffset, std::string_view::npos);

  ApplySourceEditResult first = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 3)},
      .replacement = "bad\"",
      .sourceVersion = doc.sourceVersion(),
  });
  ASSERT_TRUE(first.applied);
  ASSERT_TRUE(first.diagnostic.has_value());

  auto& context = doc.registry().ctx().get<donner::xml::components::XMLDocumentContext>();
  context.sourceDiagnostic = ParseDiagnostic::Error(
      first.diagnostic->reason, SourceRange{FileOffset::Offset(0), FileOffset::Offset(0)});

  const std::size_t updatedValueOffset = doc.source().find("bad");
  ASSERT_NE(updatedValueOffset, std::string_view::npos);
  ApplySourceEditResult repeated = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(updatedValueOffset),
                           FileOffset::Offset(updatedValueOffset + 4)},
      .replacement = "bad\"",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(repeated.applied);
  EXPECT_EQ(repeated.scope, ReparseScope::AttributeValue);
  EXPECT_THAT(repeated.diagnostic, Eq(first.diagnostic));
  EXPECT_THAT(MutationKinds(repeated),
              testing::ElementsAre(XMLMutation::Kind::SourceDiagnosticChanged));
}

TEST_F(XMLDocumentTests, ApplySourceEditOpeningTagUpdatesAttributeSet) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red" stroke="blue"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  const std::size_t editStart = doc.source().find(R"( fill="red")");
  const std::size_t editEnd = doc.source().find("/>", editStart);
  ASSERT_NE(editStart, std::string_view::npos);
  ASSERT_NE(editEnd, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart), FileOffset::Offset(editEnd)},
      .replacement = R"( opacity="0.5")",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_FALSE(rect.hasAttribute("fill"));
  EXPECT_FALSE(rect.hasAttribute("stroke"));
  EXPECT_THAT(rect.getAttribute("opacity"), Optional(Eq("0.5")));
  EXPECT_THAT(MutationKinds(result), testing::ElementsAre(XMLMutation::Kind::AttributeRemoved,
                                                          XMLMutation::Kind::AttributeRemoved,
                                                          XMLMutation::Kind::AttributeSet));
}

TEST_F(XMLDocumentTests, ApplySourceEditOpeningTagRenameReportsDiagnostic) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  const std::size_t tagOffset = doc.source().find("rect");
  ASSERT_NE(tagOffset, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(tagOffset), FileOffset::Offset(tagOffset + 4)},
      .replacement = "circle",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_THAT(result, DiagnosticReasonContains("rename is not implemented"));
  EXPECT_EQ(rect.tagName(), XMLQualifiedNameRef("rect"));
}

TEST_F(XMLDocumentTests, ApplySourceEditOpeningTagMalformedAttributeReportsDiagnostic) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red" stroke="blue"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  const std::size_t strokeOffset = doc.source().find(R"(stroke="blue")");
  ASSERT_NE(strokeOffset, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(strokeOffset), FileOffset::Offset(strokeOffset + 13)},
      .replacement = "stroke=",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_THAT(result, DiagnosticReasonContains("not enclosed in quotes"));
  EXPECT_THAT(rect.getAttribute("stroke"), Optional(Eq("blue")));
}

TEST_F(XMLDocumentTests, ApplySourceEditOpeningTagMissingTerminatorReportsDiagnostic) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  const std::size_t terminatorOffset = doc.source().find("/>");
  ASSERT_NE(terminatorOffset, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(terminatorOffset),
                           FileOffset::Offset(terminatorOffset + 2)},
      .replacement = "",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_THAT(result, DiagnosticReasonContains("opening tag malformed"));
  EXPECT_THAT(rect.getAttribute("fill"), Optional(Eq("red")));
}

TEST_F(XMLDocumentTests, ApplySourceEditOpeningTagIgnoresGreaterThanInSingleQuotedValue) {
  XMLDocument doc = ParseDocument(R"(<svg><rect data='a>b' fill="red"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  const std::size_t editStart = doc.source().find(R"( fill="red")");
  const std::size_t editEnd = doc.source().find("/>", editStart);
  ASSERT_NE(editStart, std::string_view::npos);
  ASSERT_NE(editEnd, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart), FileOffset::Offset(editEnd)},
      .replacement = R"( stroke="blue")",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(rect.getAttribute("data"), Optional(Eq("a>b")));
  EXPECT_FALSE(rect.hasAttribute("fill"));
  EXPECT_THAT(rect.getAttribute("stroke"), Optional(Eq("blue")));
}

TEST_F(XMLDocumentTests, ApplySourceEditOpeningTagIgnoresGreaterThanInDoubleQuotedValue) {
  XMLDocument doc = ParseDocument(R"(<svg><rect data="a>b" fill="red"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  const std::size_t editStart = doc.source().find(R"( fill="red")");
  const std::size_t editEnd = doc.source().find("/>", editStart);
  ASSERT_NE(editStart, std::string_view::npos);
  ASSERT_NE(editEnd, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart), FileOffset::Offset(editEnd)},
      .replacement = R"( stroke="blue")",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(rect.getAttribute("data"), Optional(Eq("a>b")));
  EXPECT_FALSE(rect.hasAttribute("fill"));
  EXPECT_THAT(rect.getAttribute("stroke"), Optional(Eq("blue")));
}

TEST_F(XMLDocumentTests, ApplySourceEditDirtyOpeningTagUsesNodeRangeForDiagnostic) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  const std::size_t fillOffset = doc.source().find("fill");
  ASSERT_NE(fillOffset, std::string_view::npos);
  ASSERT_TRUE(doc.sourceStore()->replace(fillOffset, 1, "<").has_value());
  rect.clearAttributeSourceLocation("fill");
  const std::size_t valueOffset = doc.source().find("red");
  ASSERT_NE(valueOffset, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 3)},
      .replacement = "blue",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_THAT(result, DiagnosticReasonContains("opening tag malformed"));
  EXPECT_THAT(rect.getAttribute("fill"), Optional(Eq("red")));
}

TEST_F(XMLDocumentTests, ApplySourceEditWithStaleElementStartFallsBackToParentSubtreeScope) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  const std::size_t rectNameOffset = doc.source().find("rect");
  ASSERT_NE(rectNameOffset, std::string_view::npos);
  rect.setSourceStartOffset(FileOffset::Offset(rectNameOffset));

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range =
          SourceRange{FileOffset::Offset(rectNameOffset), FileOffset::Offset(rectNameOffset + 4)},
      .replacement = "rect",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_EQ(rect.tagName(), XMLQualifiedNameRef("rect"));
}

TEST_F(XMLDocumentTests, ApplySourceEditAtOpeningTagStartUsesElementSubtreeScope) {
  XMLDocument doc = ParseDocument(R"(<svg><rect/></svg>)");
  const std::size_t rectStart = doc.source().find("<rect");
  ASSERT_NE(rectStart, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(rectStart), FileOffset::Offset(rectStart)},
      .replacement = "<!-- marker -->",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(std::string(doc.source()), testing::HasSubstr("marker"));
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

TEST_F(XMLDocumentTests, ApplySourceEditDataNodeWithoutValueLocationUsesNodeRange) {
  XMLDocument doc = ParseDocument(R"(<svg><text>hello</text></svg>)");
  XMLNode text = doc.root().firstChild()->firstChild().value();
  XMLNode data = text.firstChild().value();
  data.clearValueLocation();
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
  EXPECT_THAT(data.value(), Optional(Eq("world")));
  EXPECT_THAT(text.value(), Optional(Eq("world")));
}

TEST_F(XMLDocumentTests, ApplySourceEditElementTextContentFallbackUpdatesValue) {
  XMLDocument doc = ParseDocument(R"(<svg><text>hello</text></svg>)");
  XMLNode text = doc.root().firstChild()->firstChild().value();
  XMLNode textChild = text.firstChild().value();
  textChild.clearSourceLocation();
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
  EXPECT_THAT(text.value(), Optional(Eq("world")));
  EXPECT_EQ(doc.source(), R"(<svg><text>world</text></svg>)");
  EXPECT_THAT(MutationKinds(result), testing::Contains(XMLMutation::Kind::NodeValueChanged));
}

TEST_F(XMLDocumentTests, ApplySourceEditChildTextValueLocationUpdatesChildWithoutNodeRange) {
  XMLDocument doc = ParseDocument(R"(<svg><text>hello</text></svg>)");
  XMLNode text = doc.root().firstChild()->firstChild().value();
  XMLNode textChild = text.firstChild().value();
  const std::size_t textOffset = doc.source().find("hello");
  ASSERT_NE(textOffset, std::string_view::npos);

  textChild.clearSourceLocation();
  textChild.setValueLocation(
      SourceRange{FileOffset::Offset(textOffset), FileOffset::Offset(textOffset + 5)});

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(textOffset), FileOffset::Offset(textOffset + 5)},
      .replacement = "world",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(textChild.value(), Optional(Eq("world")));
  EXPECT_THAT(text.value(), Optional(Eq("world")));
  EXPECT_EQ(doc.source(), R"(<svg><text>world</text></svg>)");
}

TEST_F(XMLDocumentTests, ApplySourceEditElementTextFallbackUsesTagBoundaries) {
  XMLDocument doc = ParseDocument(R"(<svg><text>hello</text></svg>)");
  XMLNode text = doc.root().firstChild()->firstChild().value();
  XMLNode textChild = text.firstChild().value();
  textChild.clearSourceLocation();
  text.clearValueLocation();
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
  EXPECT_THAT(text.value(), Optional(Eq("world")));
}

TEST_F(XMLDocumentTests,
       ApplySourceEditElementTextWithStaleClosingTagFallsBackToElementSubtreeScope) {
  XMLDocument doc = ParseDocument(R"(<svg><text>hello</text></svg>)");
  XMLNode text = doc.root().firstChild()->firstChild().value();
  XMLNode textChild = text.firstChild().value();
  textChild.clearSourceLocation();
  text.clearValueLocation();
  const std::size_t closingOffset = doc.source().find("</text>");
  ASSERT_NE(closingOffset, std::string_view::npos);
  ASSERT_TRUE(doc.sourceStore()
                  ->replace(closingOffset, std::string_view("</text>").size(), "")
                  .has_value());

  const std::size_t textOffset = doc.source().find("hello");
  ASSERT_NE(textOffset, std::string_view::npos);
  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(textOffset), FileOffset::Offset(textOffset + 5)},
      .replacement = "world",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_THAT(result, DiagnosticReasonContains("Mismatched closing tag"));
  EXPECT_THAT(text.value(), Optional(Eq("hello")));
}

TEST_F(XMLDocumentTests, ApplySourceEditElementTextWithElementChildUsesSubtreeScope) {
  XMLDocument doc = ParseDocument(R"(<svg><text>hello<tspan/>world</text></svg>)");
  XMLNode text = doc.root().firstChild()->firstChild().value();
  XMLNode textChild = text.firstChild().value();
  textChild.clearSourceLocation();
  const std::size_t textOffset = doc.source().find("hello");
  ASSERT_NE(textOffset, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(textOffset), FileOffset::Offset(textOffset + 5)},
      .replacement = "hi",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(text.firstChild()->value(), Optional(Eq("hi")));
}

TEST_F(XMLDocumentTests, ApplySourceEditCDataTextNodeScopeUpdatesRawText) {
  XMLDocument doc = ParseDocument(R"(<svg><![CDATA[hello < world]]></svg>)");
  const std::size_t textOffset = doc.source().find("hello < world");
  ASSERT_NE(textOffset, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(textOffset), FileOffset::Offset(textOffset + 13)},
      .replacement = "goodbye & ok",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  XMLNode cdata = doc.root().firstChild()->firstChild().value();
  EXPECT_EQ(cdata.type(), XMLNode::Type::CData);
  EXPECT_THAT(cdata.value(), Optional(Eq("goodbye & ok")));
  EXPECT_EQ(doc.source(), R"(<svg><![CDATA[goodbye & ok]]></svg>)");
}

TEST_F(XMLDocumentTests, ApplySourceEditParsedCommentTextNodeScopeUpdatesRawText) {
  ParseResult<XMLDocument> maybeDocument =
      XMLParser::Parse(R"(<svg><!--hello--></svg>)", XMLParser::Options::ParseAll());
  ASSERT_THAT(maybeDocument, NoParseError());
  XMLDocument doc = std::move(maybeDocument.result());
  XMLNode comment = doc.root().firstChild()->firstChild().value();
  ASSERT_EQ(comment.type(), XMLNode::Type::Comment);
  const std::size_t textOffset = doc.source().find("hello");
  ASSERT_NE(textOffset, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(textOffset), FileOffset::Offset(textOffset + 5)},
      .replacement = "goodbye",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(comment.value(), Optional(Eq("goodbye")));
  EXPECT_EQ(doc.source(), R"(<svg><!--goodbye--></svg>)");
}

TEST_F(XMLDocumentTests, ApplySourceEditCommentTextNodeScopeUpdatesRawText) {
  XMLDocument doc = ParseDocument(R"(<svg><!--hello--></svg>)");
  const std::size_t textOffset = doc.source().find("hello");
  ASSERT_NE(textOffset, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(textOffset), FileOffset::Offset(textOffset + 5)},
      .replacement = "goodbye",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_EQ(doc.source(), R"(<svg><!--goodbye--></svg>)");
}

TEST_F(XMLDocumentTests, ApplySourceEditParsedCommentRejectsSplitCommentStructure) {
  ParseResult<XMLDocument> maybeDocument =
      XMLParser::Parse(R"(<svg><!--hello--></svg>)", XMLParser::Options::ParseAll());
  ASSERT_THAT(maybeDocument, NoParseError());
  XMLDocument doc = std::move(maybeDocument.result());
  XMLNode comment = doc.root().firstChild()->firstChild().value();
  ASSERT_EQ(comment.type(), XMLNode::Type::Comment);
  const std::size_t textOffset = doc.source().find("hello");
  ASSERT_NE(textOffset, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(textOffset), FileOffset::Offset(textOffset + 5)},
      .replacement = "goodbye--><!--again",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  EXPECT_THAT(result, DiagnosticReasonContains("changed the local XML structure"));
  EXPECT_THAT(comment.value(), Optional(Eq("hello")));
  EXPECT_EQ(doc.source(), R"(<svg><!--goodbye--><!--again--></svg>)");
}

TEST_F(XMLDocumentTests, ApplySourceEditTextNodeRejectsInsertedMarkup) {
  XMLDocument doc = ParseDocument(R"(<svg><text>hello</text></svg>)");
  XMLNode text = doc.root().firstChild()->firstChild().value();
  XMLNode textNode = text.firstChild().value();
  const std::size_t textOffset = doc.source().find("hello");
  ASSERT_NE(textOffset, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(textOffset), FileOffset::Offset(textOffset + 5)},
      .replacement = "<tspan/>",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  EXPECT_THAT(result, DiagnosticReasonContains("changed the local XML structure"));
  EXPECT_THAT(textNode.value(), Optional(Eq("hello")));
}

TEST_F(XMLDocumentTests, ApplySourceEditTextNodeRejectsEmptyReplacement) {
  XMLDocument doc = ParseDocument(R"(<svg><text>hello</text></svg>)");
  XMLNode text = doc.root().firstChild()->firstChild().value();
  XMLNode textNode = text.firstChild().value();
  const std::size_t textOffset = doc.source().find("hello");
  ASSERT_NE(textOffset, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(textOffset), FileOffset::Offset(textOffset + 5)},
      .replacement = "",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  EXPECT_THAT(result, DiagnosticReasonContains("changed the local XML structure"));
  EXPECT_THAT(textNode.value(), Optional(Eq("hello")));
}

TEST_F(XMLDocumentTests, ApplySourceEditTextNodePreservesLiteralAmpersand) {
  XMLDocument doc = ParseDocument(R"(<svg><text>hello</text></svg>)");
  XMLNode textNode = doc.root().firstChild()->firstChild()->firstChild().value();
  const std::size_t textOffset = doc.source().find("hello");
  ASSERT_NE(textOffset, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(textOffset), FileOffset::Offset(textOffset + 5)},
      .replacement = "&",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(textNode.value(), Optional(Eq("&")));
}

TEST_F(XMLDocumentTests, ApplySourceEditRejectsInvalidReplacementText) {
  XMLDocument doc = ParseDocument(R"(<svg><text>hello</text></svg>)");
  const std::size_t textOffset = doc.source().find("hello");
  ASSERT_NE(textOffset, std::string_view::npos);
  const std::string invalidText("bad\001", 4);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(textOffset), FileOffset::Offset(textOffset + 5)},
      .replacement = invalidText,
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_FALSE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  EXPECT_THAT(result, DiagnosticReasonContains("Invalid source replacement"));
  EXPECT_EQ(doc.source(), R"(<svg><text>hello</text></svg>)");
}

TEST_F(XMLDocumentTests, ApplySourceEditElementSubtreeReusesAndReplacesChildren) {
  XMLDocument doc =
      ParseDocument(R"(<svg><g><rect id="keep" fill="red"/><circle id="drop"/></g></svg>)");
  XMLNode group = doc.root().firstChild()->firstChild().value();
  XMLNode rect = group.firstChild().value();
  const std::size_t editStart = doc.source().find("<rect");
  const std::size_t editEnd = doc.source().find("</g>");
  ASSERT_NE(editStart, std::string_view::npos);
  ASSERT_NE(editEnd, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart), FileOffset::Offset(editEnd)},
      .replacement = R"(<rect id="keep" fill="blue"/><path id="new"/>)",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  ASSERT_TRUE(group.firstChild().has_value());
  EXPECT_EQ(*group.firstChild(), rect);
  EXPECT_THAT(rect.getAttribute("fill"), Optional(Eq("blue")));
  ASSERT_TRUE(rect.nextSibling().has_value());
  EXPECT_EQ(rect.nextSibling()->tagName(), XMLQualifiedNameRef("path"));
  EXPECT_THAT(MutationKinds(result), testing::Contains(XMLMutation::Kind::SubtreeReplaced));
  EXPECT_THAT(MutationKinds(result), testing::Contains(XMLMutation::Kind::NodeInserted));
  EXPECT_THAT(MutationKinds(result), testing::Contains(XMLMutation::Kind::NodeRemoved));
}

TEST_F(XMLDocumentTests, ApplySourceEditElementSubtreeReusedChildRemovesAttribute) {
  XMLDocument doc = ParseDocument(R"(<svg><g><rect id="keep" fill="red"/></g></svg>)");
  XMLNode group = doc.root().firstChild()->firstChild().value();
  XMLNode rect = group.firstChild().value();
  const std::size_t editStart = doc.source().find("<rect");
  const std::size_t editEnd = doc.source().find("</g>");
  ASSERT_NE(editStart, std::string_view::npos);
  ASSERT_NE(editEnd, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart), FileOffset::Offset(editEnd)},
      .replacement = R"(<rect id="keep"/>)",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  ASSERT_TRUE(group.firstChild().has_value());
  EXPECT_EQ(*group.firstChild(), rect);
  EXPECT_FALSE(rect.hasAttribute("fill"));
  EXPECT_THAT(MutationKinds(result), testing::Contains(XMLMutation::Kind::AttributeRemoved));
}

TEST_F(XMLDocumentTests, ApplySourceEditElementSubtreeClonesCDataAndElementChildren) {
  XMLDocument doc = ParseDocument(R"(<svg><g><old/></g></svg>)");
  XMLNode group = doc.root().firstChild()->firstChild().value();
  const std::size_t editStart = doc.source().find("<old");
  const std::size_t editEnd = doc.source().find("</g>");
  ASSERT_NE(editStart, std::string_view::npos);
  ASSERT_NE(editEnd, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart), FileOffset::Offset(editEnd)},
      .replacement = "<![CDATA[raw]]><child/>",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));

  XMLNode child = group.firstChild().value();
  EXPECT_EQ(child.type(), XMLNode::Type::CData);
  EXPECT_THAT(child.value(), Optional(Eq("raw")));
  child = child.nextSibling().value();
  EXPECT_EQ(child.tagName(), XMLQualifiedNameRef("child"));
  EXPECT_FALSE(child.nextSibling().has_value());
  EXPECT_THAT(MutationKinds(result), testing::Contains(XMLMutation::Kind::NodeInserted));
}

TEST_F(XMLDocumentTests, ApplySourceEditElementSubtreeReusesOnlyOneDuplicateId) {
  XMLDocument doc = ParseDocument(R"(<svg><g><rect id="keep"/><circle id="drop"/></g></svg>)");
  XMLNode group = doc.root().firstChild()->firstChild().value();
  XMLNode originalRect = group.firstChild().value();
  const std::size_t editStart = doc.source().find("<rect");
  const std::size_t editEnd = doc.source().find("</g>");
  ASSERT_NE(editStart, std::string_view::npos);
  ASSERT_NE(editEnd, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart), FileOffset::Offset(editEnd)},
      .replacement = R"(<rect id="keep" data-order="first"/><rect id="keep" data-order="second"/>)",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  ASSERT_TRUE(group.firstChild().has_value());
  EXPECT_EQ(*group.firstChild(), originalRect);
  EXPECT_THAT(originalRect.getAttribute("data-order"), Optional(Eq("first")));
  ASSERT_TRUE(originalRect.nextSibling().has_value());
  XMLNode clonedRect = originalRect.nextSibling().value();
  EXPECT_NE(clonedRect, originalRect);
  EXPECT_THAT(clonedRect.getAttribute("data-order"), Optional(Eq("second")));
}

TEST_F(XMLDocumentTests, ApplySourceEditWholeSelfClosingElementUsesParentSubtreeScope) {
  XMLDocument doc = ParseDocument(R"(<svg><rect/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  std::optional<SourceRange> rectLocation = rect.getNodeLocation();
  ASSERT_TRUE(rectLocation.has_value());

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = *rectLocation,
      .replacement = "<circle/>",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_EQ(doc.source(), R"(<svg><circle/></svg>)");
  EXPECT_EQ(doc.root().firstChild()->firstChild()->tagName(), XMLQualifiedNameRef("circle"));
}

TEST_F(XMLDocumentTests, ApplySourceEditAtDocumentEndUsesDocumentFallback) {
  XMLDocument doc = ParseDocument(R"(<svg/>)");

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(doc.source().size()),
                           FileOffset::Offset(doc.source().size())},
      .replacement = "\n<!-- trailing -->",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::Document);
  EXPECT_THAT(result, DiagnosticReasonContains("Only attribute-value"));
  EXPECT_THAT(std::string(doc.source()), testing::HasSubstr("trailing"));
}

TEST_F(XMLDocumentTests, ApplySourceEditProcessingInstructionUpdatesRawText) {
  ParseResult<XMLDocument> maybeDocument =
      XMLParser::Parse(R"(<svg><?target old?></svg>)", XMLParser::Options::ParseAll());
  ASSERT_THAT(maybeDocument, NoParseError());
  XMLDocument doc = std::move(maybeDocument.result());
  XMLNode instruction = doc.root().firstChild()->firstChild().value();
  ASSERT_EQ(instruction.type(), XMLNode::Type::ProcessingInstruction);
  const std::size_t valueOffset = doc.source().find("old");
  ASSERT_NE(valueOffset, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 3)},
      .replacement = "new-value",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(instruction.value(), Optional(Eq("new-value")));
  EXPECT_THAT(MutationKinds(result), testing::Contains(XMLMutation::Kind::NodeValueChanged));
}

TEST_F(XMLDocumentTests, ApplySourceEditProcessingInstructionTargetRenameReportsDiagnostic) {
  ParseResult<XMLDocument> maybeDocument =
      XMLParser::Parse(R"(<svg><?target old?></svg>)", XMLParser::Options::ParseAll());
  ASSERT_THAT(maybeDocument, NoParseError());
  XMLDocument doc = std::move(maybeDocument.result());
  XMLNode instruction = doc.root().firstChild()->firstChild().value();
  ASSERT_EQ(instruction.type(), XMLNode::Type::ProcessingInstruction);
  const std::size_t targetOffset = doc.source().find("target");
  const std::size_t valueOffset = doc.source().find("old");
  ASSERT_NE(targetOffset, std::string_view::npos);
  ASSERT_NE(valueOffset, std::string_view::npos);
  instruction.setValueLocation(
      SourceRange{FileOffset::Offset(targetOffset), FileOffset::Offset(valueOffset + 3)});

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(targetOffset), FileOffset::Offset(valueOffset + 3)},
      .replacement = "renamed old",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  EXPECT_THAT(result, DiagnosticReasonContains("target rename"));
  EXPECT_EQ(instruction.tagName(), XMLQualifiedNameRef("target"));
}

TEST_F(XMLDocumentTests, ApplySourceEditMalformedCDataReportsScopedDiagnostic) {
  XMLDocument doc = ParseDocument(R"(<svg><![CDATA[hello]]></svg>)");
  XMLNode cdata = doc.root().firstChild()->firstChild().value();
  ASSERT_EQ(cdata.type(), XMLNode::Type::CData);
  const std::size_t valueOffset = doc.source().find("hello");
  ASSERT_NE(valueOffset, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 5)},
      .replacement = "bad]]>tail",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  ASSERT_TRUE(result.diagnostic.has_value());
  EXPECT_THAT(cdata.value(), Optional(Eq("hello")));
}

TEST_F(XMLDocumentTests, ApplySourceEditRawTextWithInvalidatedNodeRangeReportsDiagnostic) {
  XMLDocument doc = ParseDocument(R"(<svg><![CDATA[hello]]></svg>)");
  XMLNode cdata = doc.root().firstChild()->firstChild().value();
  ASSERT_EQ(cdata.type(), XMLNode::Type::CData);
  std::optional<SourceRange> nodeLocation = cdata.getNodeLocation();
  ASSERT_TRUE(nodeLocation.has_value());
  cdata.setValueLocation(*nodeLocation);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = *nodeLocation,
      .replacement = "<![CDATA[world]]>",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  EXPECT_THAT(result, DiagnosticReasonContains("node source range unavailable"));
  EXPECT_THAT(cdata.value(), Optional(Eq("hello")));
}

TEST_F(XMLDocumentTests, ApplySourceEditElementSubtreeMalformedReportsDiagnostic) {
  XMLDocument doc = ParseDocument(R"(<svg><g><rect/></g></svg>)");
  XMLNode group = doc.root().firstChild()->firstChild().value();
  const std::size_t editStart = doc.source().find("<rect");
  const std::size_t editEnd = doc.source().find("</g>");
  ASSERT_NE(editStart, std::string_view::npos);
  ASSERT_NE(editEnd, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart), FileOffset::Offset(editEnd)},
      .replacement = "<broken>",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  ASSERT_TRUE(result.diagnostic.has_value());
  ASSERT_TRUE(group.firstChild().has_value());
  EXPECT_EQ(group.firstChild()->tagName(), XMLQualifiedNameRef("rect"));
  EXPECT_THAT(MutationKinds(result),
              testing::ElementsAre(XMLMutation::Kind::SourceDiagnosticChanged));
}

TEST_F(XMLDocumentTests, ApplySourceEditElementSubtreeRenamedSourceReportsDiagnostic) {
  XMLDocument doc = ParseDocument(R"(<svg><g><rect/></g></svg>)");
  XMLNode group = doc.root().firstChild()->firstChild().value();
  const std::size_t openingTagOffset = doc.source().find("<g");
  const std::size_t closingTagOffset = doc.source().find("</g>");
  ASSERT_NE(openingTagOffset, std::string_view::npos);
  ASSERT_NE(closingTagOffset, std::string_view::npos);
  ASSERT_TRUE(doc.sourceStore()->replace(openingTagOffset + 1, 1, "h").has_value());
  ASSERT_TRUE(doc.sourceStore()->replace(closingTagOffset + 2, 1, "h").has_value());
  const std::size_t editStart = doc.source().find("<rect");
  ASSERT_NE(editStart, std::string_view::npos);
  const std::size_t editEnd = editStart + std::string_view("<rect/>").size();

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart), FileOffset::Offset(editEnd)},
      .replacement = "<circle/>",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_THAT(result, DiagnosticReasonContains("renamed the target element"));
  EXPECT_EQ(group.tagName(), XMLQualifiedNameRef("g"));
  ASSERT_TRUE(group.firstChild().has_value());
  EXPECT_EQ(group.firstChild()->tagName(), XMLQualifiedNameRef("rect"));
}

TEST_F(XMLDocumentTests, ApplySourceEditSuccessClearsPreviousScopedDiagnostic) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red"/></svg>)");
  const std::size_t valueOffset = doc.source().find("red");
  ASSERT_NE(valueOffset, std::string_view::npos);

  ApplySourceEditResult malformed = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 3)},
      .replacement = "bad\"",
      .sourceVersion = doc.sourceVersion(),
  });
  ASSERT_TRUE(malformed.applied);
  ASSERT_TRUE(malformed.diagnostic.has_value());

  const std::size_t updatedValueOffset = doc.source().find("bad");
  ASSERT_NE(updatedValueOffset, std::string_view::npos);
  ApplySourceEditResult fixed = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(updatedValueOffset),
                           FileOffset::Offset(updatedValueOffset + 4)},
      .replacement = "blue",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(fixed.applied);
  EXPECT_EQ(fixed.scope, ReparseScope::AttributeValue);
  EXPECT_THAT(fixed.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(MutationKinds(fixed),
              testing::ElementsAre(XMLMutation::Kind::AttributeSet,
                                   XMLMutation::Kind::SourceDiagnosticChanged));
}

//
// setAttribute - DOM-side structured edit.
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

TEST_F(XMLDocumentTests, SetAttributeFallsBackToOpeningTagScanWhenCachedRangeIsMissing) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill = 'red' /></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  rect.clearAttributeSourceLocation("fill");

  ApplySourceEditResult result = doc.setAttribute(rect, "fill", "blue");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::AttributeValue);
  EXPECT_THAT(rect.getAttribute("fill"), Optional(Eq("blue")));
  EXPECT_EQ(doc.source(), R"(<svg><rect fill = 'blue' /></svg>)");
}

TEST_F(XMLDocumentTests, SetAttributeFallsBackWhenCachedValueRangeIsMissing) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  std::optional<XMLAttributeSourceLocation> sourceLocation =
      rect.getAttributeSourceLocation("fill");
  ASSERT_TRUE(sourceLocation.has_value());
  rect.setAttributeSourceLocation("fill", sourceLocation->fullRange,
                                  SourceRange{FileOffset::EndOfString(), FileOffset::EndOfString()},
                                  sourceLocation->quote);

  ApplySourceEditResult result = doc.setAttribute(rect, "fill", "blue");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::AttributeValue);
  EXPECT_THAT(rect.getAttribute("fill"), Optional(Eq("blue")));
  EXPECT_EQ(doc.source(), R"(<svg><rect fill="blue"/></svg>)");
}

TEST_F(XMLDocumentTests, SetAttributePreservesSingleQuoteStyleAndEscapesValue) {
  XMLDocument doc = ParseDocument(R"(<svg><rect label='old'/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();

  ApplySourceEditResult result = doc.setAttribute(rect, "label", "Tom's <rect>");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::AttributeValue);
  EXPECT_THAT(rect.getAttribute("label"), Optional(Eq("Tom's <rect>")));
  EXPECT_THAT(std::string(doc.source()), testing::HasSubstr("Tom&apos;s &lt;rect&gt;"));
}

TEST_F(XMLDocumentTests, SetAttributeRejectsInvalidXmlAttributeValue) {
  XMLDocument doc = ParseDocument(R"(<svg><rect label="old"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  const std::string invalidValue("bad\001value", 9);

  ApplySourceEditResult result = doc.setAttribute(rect, "label", invalidValue);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("cannot be represented"));
  EXPECT_THAT(rect.getAttribute("label"), Optional(Eq("old")));
}

TEST_F(XMLDocumentTests, SetAttributeRejectsInvalidXmlAttributeValueForNewAttribute) {
  XMLDocument doc = ParseDocument(R"(<svg><rect/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  const std::string invalidValue("bad\001value", 9);

  ApplySourceEditResult result = doc.setAttribute(rect, "label", invalidValue);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("cannot be represented"));
  EXPECT_FALSE(rect.hasAttribute("label"));
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

TEST_F(XMLDocumentTests, SetAttributeInsertsQualifiedNameWithPrefix) {
  XMLDocument doc = ParseDocument(R"(<svg><use/></svg>)");
  XMLNode use = doc.root().firstChild()->firstChild().value();
  const XMLQualifiedName hrefName(RcString("xlink"), RcString("href"));

  ApplySourceEditResult result = doc.setAttribute(use, hrefName, "#symbol");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_THAT(use.getAttribute(hrefName), Optional(Eq("#symbol")));
  EXPECT_EQ(doc.source(), R"(<svg><use xlink:href="#symbol"/></svg>)");
}

TEST_F(XMLDocumentTests, SetAttributeInsertsOnNonSelfClosingOpeningTag) {
  XMLDocument doc = ParseDocument(R"(<svg><rect></rect></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();

  ApplySourceEditResult result = doc.setAttribute(rect, "fill", "green");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_EQ(doc.source(), R"(<svg><rect fill="green"></rect></svg>)");
}

TEST_F(XMLDocumentTests, SetAttributeInsertsWithoutDoubleSpaceBeforeSelfCloseSlash) {
  XMLDocument doc = ParseDocument(R"(<svg><rect /></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();

  ApplySourceEditResult result = doc.setAttribute(rect, "fill", "green");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_EQ(doc.source(), R"(<svg><rect fill="green"/></svg>)");
}

TEST_F(XMLDocumentTests, SetAttributeReusesExistingWhitespaceBeforeTagClose) {
  XMLDocument doc = ParseDocument(R"(<svg><rect ></rect></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();

  ApplySourceEditResult result = doc.setAttribute(rect, "fill", "green");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_EQ(doc.source(), R"(<svg><rect fill="green"></rect></svg>)");
}

TEST_F(XMLDocumentTests, SetAttributeWithStaleOpeningTagStartFails) {
  XMLDocument doc = ParseDocument(R"(<svg><rect/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  const std::size_t tagNameOffset = doc.source().find("rect");
  ASSERT_NE(tagNameOffset, std::string_view::npos);
  rect.setSourceStartOffset(FileOffset::Offset(tagNameOffset));

  ApplySourceEditResult result = doc.setAttribute(rect, "fill", "blue");

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("opening tag source range"));
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

TEST_F(XMLDocumentTests, SetAttributeUpdatesXmlDeclarationAttribute) {
  XMLDocument doc = ParseDocument(R"(<?xml version="1.0"?><svg/>)");
  XMLNode declaration = doc.root().firstChild().value();
  ASSERT_EQ(declaration.type(), XMLNode::Type::XMLDeclaration);

  ApplySourceEditResult result = doc.setAttribute(declaration, "version", "1.1");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::AttributeValue);
  EXPECT_THAT(declaration.getAttribute("version"), Optional(Eq("1.1")));
  EXPECT_THAT(std::string(doc.source()), testing::HasSubstr(R"(version="1.1")"));
}

TEST_F(XMLDocumentTests, SetAttributeInsertsXmlDeclarationAttribute) {
  XMLDocument doc = ParseDocument(R"(<?xml version="1.0"?><svg/>)");
  XMLNode declaration = doc.root().firstChild().value();
  ASSERT_EQ(declaration.type(), XMLNode::Type::XMLDeclaration);

  ApplySourceEditResult result = doc.setAttribute(declaration, "encoding", "UTF-8");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_THAT(declaration.getAttribute("encoding"), Optional(Eq("UTF-8")));
  EXPECT_EQ(doc.source(), R"(<?xml version="1.0" encoding="UTF-8"?><svg/>)");
}

TEST_F(XMLDocumentTests, SetAttributeExistingDomOnlyAttributeFailsWithoutSourceRange) {
  XMLDocument doc = ParseDocument(R"(<svg><rect/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  rect.setAttribute("fill", "red");

  ApplySourceEditResult result = doc.setAttribute(rect, "fill", "blue");

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("without a source range"));
  EXPECT_THAT(rect.getAttribute("fill"), Optional(Eq("red")));
}

TEST_F(XMLDocumentTests, SetAttributeExistingAttributeWithMalformedFallbackRangeFails) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  rect.clearAttributeSourceLocation("fill");
  const std::size_t fillOffset = doc.source().find("fill");
  ASSERT_NE(fillOffset, std::string_view::npos);
  ASSERT_TRUE(doc.sourceStore()
                  ->replace(fillOffset, std::string_view(R"(fill="red")").size(), "fill=")
                  .has_value());

  ApplySourceEditResult result = doc.setAttribute(rect, "fill", "blue");

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("without a source range"));
  EXPECT_THAT(rect.getAttribute("fill"), Optional(Eq("red")));
}

TEST_F(XMLDocumentTests, SetAttributeRejectsExistingValueWhenOffsetIsNotUtf8Boundary) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  std::string corruptSource(doc.source());
  const std::size_t valueOffset = corruptSource.find("red");
  ASSERT_NE(valueOffset, std::string::npos);
  corruptSource.insert(valueOffset, std::string("\x80", 1));
  doc.setSource(corruptSource);

  ApplySourceEditResult result = doc.setAttribute(rect, "fill", "blue");

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("Invalid source replacement"));
  EXPECT_THAT(rect.getAttribute("fill"), Optional(Eq("red")));
  EXPECT_EQ(doc.source(), corruptSource);
}

TEST_F(XMLDocumentTests, SetAttributeOnSourcelessNodeInSourceDocumentFails) {
  XMLDocument doc = ParseDocument(R"(<svg></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");
  svg.appendChild(rect);

  ApplySourceEditResult result = doc.setAttribute(rect, "fill", "blue");

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("opening tag source range"));
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

TEST_F(XMLDocumentTests, RemoveAttributeFallsBackToOpeningTagScanWhenCachedRangeIsMissing) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill = 'red' stroke="blue"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  rect.clearAttributeSourceLocation("fill");

  ApplySourceEditResult result = doc.removeAttribute(rect, "fill");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_FALSE(rect.hasAttribute("fill"));
  EXPECT_EQ(doc.source(), R"(<svg><rect stroke="blue"/></svg>)");
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

TEST_F(XMLDocumentTests, RemoveDomOnlyAttributeFailsWithoutSourceRange) {
  XMLDocument doc = ParseDocument(R"(<svg><rect/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  rect.setAttribute("fill", "red");

  ApplySourceEditResult result = doc.removeAttribute(rect, "fill");

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("without a source range"));
  EXPECT_TRUE(rect.hasAttribute("fill"));
}

TEST_F(XMLDocumentTests, RemoveAttributeFromXmlDeclaration) {
  XMLDocument doc = ParseDocument(R"(<?xml version="1.0" encoding="UTF-8"?><svg/>)");
  XMLNode declaration = doc.root().firstChild().value();
  ASSERT_EQ(declaration.type(), XMLNode::Type::XMLDeclaration);

  ApplySourceEditResult result = doc.removeAttribute(declaration, "encoding");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_FALSE(declaration.hasAttribute("encoding"));
  EXPECT_EQ(doc.source(), R"(<?xml version="1.0"?><svg/>)");
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

TEST_F(XMLDocumentTests, InsertNodeUsesClosingTagScanWhenCachedClosingTagRangeIsMissing) {
  XMLDocument doc = ParseDocument(R"(<svg><g></g></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode group = svg.firstChild().value();
  svg.clearClosingTagLocation();
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");

  ApplySourceEditResult result = doc.insertNode(svg, rect);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  ASSERT_TRUE(group.nextSibling().has_value());
  EXPECT_EQ(*group.nextSibling(), rect);
  EXPECT_EQ(doc.source(), R"(<svg><g></g><rect/></svg>)");
}

TEST_F(XMLDocumentTests, InsertNodeUsesClosingTagScanWhenCachedClosingTagRangeIsStale) {
  XMLDocument doc = ParseDocument(R"(<svg><g></g></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode group = svg.firstChild().value();
  svg.setClosingTagLocation(SourceRange{FileOffset::Offset(0), FileOffset::Offset(0)});
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");

  ApplySourceEditResult result = doc.insertNode(svg, rect);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  ASSERT_TRUE(group.nextSibling().has_value());
  EXPECT_EQ(*group.nextSibling(), rect);
  EXPECT_EQ(doc.source(), R"(<svg><g></g><rect/></svg>)");
}

TEST_F(XMLDocumentTests, InsertNodeExpandsSelfClosingParent) {
  XMLDocument doc = ParseDocument(R"(<svg><g /></svg>)");
  XMLNode group = doc.root().firstChild()->firstChild().value();
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");

  ApplySourceEditResult result = doc.insertNode(group, rect);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_EQ(group.firstChild(), rect);
  EXPECT_THAT(std::string(doc.source()), testing::HasSubstr("<g><rect"));
  EXPECT_THAT(std::string(doc.source()), testing::HasSubstr("</g>"));
}

TEST_F(XMLDocumentTests, InsertNodeExpandsTightSelfClosingParent) {
  XMLDocument doc = ParseDocument(R"(<svg><g/></svg>)");
  XMLNode group = doc.root().firstChild()->firstChild().value();
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");

  ApplySourceEditResult result = doc.insertNode(group, rect);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_EQ(doc.source(), R"(<svg><g><rect/></g></svg>)");
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

TEST_F(XMLDocumentTests, InsertNodeWithReferenceFromOtherDocumentFails) {
  XMLDocument doc = ParseDocument(R"(<svg></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");
  XMLDocument other = ParseDocument(R"(<svg><ref/></svg>)");
  XMLNode foreignReference = other.root().firstChild()->firstChild().value();

  ApplySourceEditResult result = doc.insertNode(svg, rect, foreignReference);

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

TEST_F(XMLDocumentTests, InsertNodeBeforeReferenceWithoutSourceRangeFails) {
  XMLDocument doc = ParseDocument(R"(<svg><circle/></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode circle = svg.firstChild().value();
  circle.clearSourceLocation();
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");

  ApplySourceEditResult result = doc.insertNode(svg, rect, circle);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("source insertion point"));
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

TEST_F(XMLDocumentTests, InsertNodeMovesSourceBackedElementForward) {
  XMLDocument doc = ParseDocument(R"(<svg><a><moved/></a><b></b></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode a = svg.firstChild().value();
  XMLNode b = a.nextSibling().value();
  XMLNode moved = a.firstChild().value();

  ApplySourceEditResult result = doc.insertNode(b, moved);

  EXPECT_TRUE(result.applied);
  EXPECT_FALSE(a.firstChild().has_value());
  ASSERT_TRUE(b.firstChild().has_value());
  EXPECT_EQ(*b.firstChild(), moved);
  EXPECT_THAT(std::string(doc.source()), testing::HasSubstr("<a></a><b><moved/>"));
  EXPECT_THAT(MutationKinds(result), testing::ElementsAre(XMLMutation::Kind::NodeRemoved,
                                                          XMLMutation::Kind::NodeInserted));
}

TEST_F(XMLDocumentTests, InsertNodeMovesSourceBackedElementForwardIntoSelfClosingParent) {
  XMLDocument doc = ParseDocument(R"(<svg><a><moved/></a><b /></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode a = svg.firstChild().value();
  XMLNode b = a.nextSibling().value();
  XMLNode moved = a.firstChild().value();

  ApplySourceEditResult result = doc.insertNode(b, moved);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_FALSE(a.firstChild().has_value());
  ASSERT_TRUE(b.firstChild().has_value());
  EXPECT_EQ(*b.firstChild(), moved);
  EXPECT_EQ(doc.source(), R"(<svg><a></a><b><moved/></b></svg>)");
  EXPECT_THAT(MutationKinds(result), testing::ElementsAre(XMLMutation::Kind::NodeRemoved,
                                                          XMLMutation::Kind::NodeInserted));
}

TEST_F(XMLDocumentTests, InsertNodeMovesSourceBackedElementForwardBeforeReference) {
  XMLDocument doc = ParseDocument(R"(<svg><a><moved/></a><b><ref/></b></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode a = svg.firstChild().value();
  XMLNode b = a.nextSibling().value();
  XMLNode moved = a.firstChild().value();
  XMLNode ref = b.firstChild().value();

  ApplySourceEditResult result = doc.insertNode(b, moved, ref);

  EXPECT_TRUE(result.applied);
  EXPECT_FALSE(a.firstChild().has_value());
  ASSERT_TRUE(b.firstChild().has_value());
  EXPECT_EQ(*b.firstChild(), moved);
  ASSERT_TRUE(moved.nextSibling().has_value());
  EXPECT_EQ(*moved.nextSibling(), ref);
  EXPECT_EQ(doc.source(), R"(<svg><a></a><b><moved/><ref/></b></svg>)");
  EXPECT_THAT(MutationKinds(result), testing::ElementsAre(XMLMutation::Kind::NodeRemoved,
                                                          XMLMutation::Kind::NodeInserted));
}

TEST_F(XMLDocumentTests, InsertNodeMovesSourceBackedElementBackwardBeforeReference) {
  XMLDocument doc = ParseDocument(R"(<svg><a><ref/></a><b><moved/></b></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode a = svg.firstChild().value();
  XMLNode ref = a.firstChild().value();
  XMLNode b = a.nextSibling().value();
  XMLNode moved = b.firstChild().value();

  ApplySourceEditResult result = doc.insertNode(a, moved, ref);

  EXPECT_TRUE(result.applied);
  ASSERT_TRUE(a.firstChild().has_value());
  EXPECT_EQ(*a.firstChild(), moved);
  ASSERT_TRUE(moved.nextSibling().has_value());
  EXPECT_EQ(*moved.nextSibling(), ref);
  EXPECT_FALSE(b.firstChild().has_value());
  EXPECT_EQ(doc.source(), R"(<svg><a><moved/><ref/></a><b></b></svg>)");
  EXPECT_THAT(MutationKinds(result), testing::ElementsAre(XMLMutation::Kind::NodeRemoved,
                                                          XMLMutation::Kind::NodeInserted));
}

TEST_F(XMLDocumentTests, InsertNodeRejectsReferenceFromDifferentParent) {
  XMLDocument doc = ParseDocument(R"(<svg><a></a><b><ref/></b></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode a = svg.firstChild().value();
  XMLNode b = a.nextSibling().value();
  XMLNode ref = b.firstChild().value();
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");

  ApplySourceEditResult result = doc.insertNode(a, rect, ref);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("source insertion point"));
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

TEST_F(XMLDocumentTests, InsertNodeNoOpsWhenReferenceIsSameNode) {
  XMLDocument doc = ParseDocument(R"(<svg><a><first/><second/></a></svg>)");
  XMLNode a = doc.root().firstChild()->firstChild().value();
  XMLNode first = a.firstChild().value();

  ApplySourceEditResult result = doc.insertNode(a, first, first);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  ASSERT_TRUE(a.firstChild().has_value());
  EXPECT_EQ(*a.firstChild(), first);
}

TEST_F(XMLDocumentTests, InsertNodeNoOpsWhenAlreadyBeforeReference) {
  XMLDocument doc = ParseDocument(R"(<svg><a><first/><second/></a></svg>)");
  XMLNode a = doc.root().firstChild()->firstChild().value();
  XMLNode first = a.firstChild().value();
  XMLNode second = first.nextSibling().value();

  ApplySourceEditResult result = doc.insertNode(a, first, second);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  ASSERT_TRUE(a.firstChild().has_value());
  EXPECT_EQ(*a.firstChild(), first);
  ASSERT_TRUE(first.nextSibling().has_value());
  EXPECT_EQ(*first.nextSibling(), second);
}

TEST_F(XMLDocumentTests, InsertNodeRejectsPartiallySourceBackedMove) {
  XMLDocument doc = ParseDocument(R"(<svg><a></a><b></b></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode a = svg.firstChild().value();
  XMLNode b = a.nextSibling().value();
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");
  a.appendChild(rect);

  ApplySourceEditResult result = doc.insertNode(b, rect);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("partially source-backed"));
}

TEST_F(XMLDocumentTests, InsertNodeRejectsDetachedNodeWithSourceRangeOnly) {
  XMLDocument doc = ParseDocument(R"(<svg><a></a></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode detached = XMLNode::CreateElementNode(doc, "detached");
  detached.setSourceStartOffset(FileOffset::Offset(0));
  detached.setSourceEndOffset(FileOffset::Offset(5));

  ApplySourceEditResult result = doc.insertNode(svg, detached);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("partially source-backed"));
}

TEST_F(XMLDocumentTests, InsertNodeRejectsMovingSourceBackedTextNode) {
  XMLDocument doc = ParseDocument(R"(<svg><a>text</a><b></b></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode a = svg.firstChild().value();
  XMLNode b = a.nextSibling().value();
  XMLNode text = a.firstChild().value();
  ASSERT_EQ(text.type(), XMLNode::Type::Data);

  ApplySourceEditResult result = doc.insertNode(b, text);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("Moving non-element nodes"));
}

TEST_F(XMLDocumentTests, InsertNodeRejectsMoveWithInvalidSourceRange) {
  XMLDocument doc = ParseDocument(R"(<svg><a><moved/></a><b></b></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode a = svg.firstChild().value();
  XMLNode b = a.nextSibling().value();
  XMLNode moved = a.firstChild().value();
  moved.setSourceEndOffset(FileOffset::Offset(doc.source().size() + 1));

  ApplySourceEditResult result = doc.insertNode(b, moved);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("source range"));
}

TEST_F(XMLDocumentTests, InsertNodeMoveReconcilesLiveChildrenNotPresentInSource) {
  XMLDocument doc = ParseDocument(R"(<svg><a><moved/></a><b></b></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode a = svg.firstChild().value();
  XMLNode b = a.nextSibling().value();
  XMLNode moved = a.firstChild().value();
  XMLNode extra = XMLNode::CreateElementNode(doc, "extra");
  moved.appendChild(extra);

  ApplySourceEditResult result = doc.insertNode(b, moved);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_FALSE(a.firstChild().has_value());
  ASSERT_TRUE(b.firstChild().has_value());
  EXPECT_EQ(*b.firstChild(), moved);
  EXPECT_FALSE(moved.firstChild().has_value());
  EXPECT_THAT(MutationKinds(result), testing::ElementsAre(XMLMutation::Kind::NodeRemoved,
                                                          XMLMutation::Kind::NodeInserted));
}

TEST_F(XMLDocumentTests, InsertNodeRejectsSourcelessNonElementSerialization) {
  XMLDocument doc = ParseDocument(R"(<svg></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode comment = XMLNode::CreateCommentNode(doc, "note");

  ApplySourceEditResult result = doc.insertNode(svg, comment);

  EXPECT_FALSE(result.applied);
  ASSERT_TRUE(result.diagnostic.has_value());
}

TEST_F(XMLDocumentTests, InsertNodeRejectsParentWithStaleSourceRange) {
  XMLDocument doc = ParseDocument(R"(<svg><g></g></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  svg.clearClosingTagLocation();
  svg.setSourceEndOffset(FileOffset::Offset(doc.source().size() + 10));
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");

  ApplySourceEditResult result = doc.insertNode(svg, rect);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("source insertion point"));
}

TEST_F(XMLDocumentTests, InsertNodeRejectsParentWithTooShortSourceRange) {
  XMLDocument doc = ParseDocument(R"(<svg><g></g></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  svg.clearClosingTagLocation();
  svg.setSourceEndOffset(FileOffset::Offset(3));
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");

  ApplySourceEditResult result = doc.insertNode(svg, rect);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("source insertion point"));
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

TEST_F(XMLDocumentTests, RemoveNodeClearsDescendantSourceLocations) {
  XMLDocument doc = ParseDocument(R"(<svg><g><rect fill="red"/></g></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode group = svg.firstChild().value();
  XMLNode rect = group.firstChild().value();
  ASSERT_TRUE(rect.getNodeLocation().has_value());
  ASSERT_TRUE(rect.getAttributeSourceLocation("fill").has_value());

  ApplySourceEditResult result = doc.removeNode(group);

  EXPECT_TRUE(result.applied);
  EXPECT_FALSE(svg.firstChild().has_value());
  EXPECT_FALSE(group.getNodeLocation().has_value());
  EXPECT_FALSE(rect.getNodeLocation().has_value());
  EXPECT_FALSE(rect.getAttributeSourceLocation("fill").has_value());
}

TEST_F(XMLDocumentTests, RemoveAttributeRemovesLeadingWhitespaceBeforeAttribute) {
  XMLDocument doc = ParseDocument("<svg><rect\n  fill=\"red\" stroke=\"blue\"/></svg>");
  XMLNode rect = doc.root().firstChild()->firstChild().value();

  ApplySourceEditResult result = doc.removeAttribute(rect, "fill");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_EQ(doc.source(), R"(<svg><rect stroke="blue"/></svg>)");
}

TEST_F(XMLDocumentTests, RemoveAttributeRemovesCarriageReturnAndTabBeforeAttribute) {
  XMLDocument doc = ParseDocument("<svg><rect\r\tfill=\"red\" stroke=\"blue\"/></svg>");
  XMLNode rect = doc.root().firstChild()->firstChild().value();

  ApplySourceEditResult result = doc.removeAttribute(rect, "fill");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_EQ(doc.source(), R"(<svg><rect stroke="blue"/></svg>)");
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

TEST_F(XMLDocumentTests, RemoveNodeWithStaleSourceRangeFails) {
  XMLDocument doc = ParseDocument(R"(<svg><rect/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  rect.setSourceEndOffset(FileOffset::Offset(doc.source().size() + 1));

  ApplySourceEditResult result = doc.removeNode(rect);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("without a source range"));
}

//
// Structural source-formatting fidelity.
//
// These assert the exact source bytes produced by each structural mutation kind when the
// document is already laid out across indented lines. The fidelity standard mirrors
// attribute writeback: a mutation must leave the surrounding whitespace as a human would.
//

TEST_F(XMLDocumentTests, InsertNodeIntoEmptyParentIndentsChildOnItsOwnLine) {
  XMLDocument doc = ParseDocument("<svg>\n  <g/>\n</svg>");
  XMLNode g = ElementChild(doc.root().firstChild().value(), 0);
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");

  ApplySourceEditResult result = doc.insertNode(g, rect);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(doc.source(), "<svg>\n  <g>\n    <rect/>\n  </g>\n</svg>");
}

TEST_F(XMLDocumentTests, InsertNodeBetweenSiblingsMatchesSiblingIndent) {
  XMLDocument doc = ParseDocument("<svg>\n  <a/>\n  <b/>\n</svg>");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode b = ElementChild(svg, 1);
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");

  ApplySourceEditResult result = doc.insertNode(svg, rect, b);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(doc.source(), "<svg>\n  <a/>\n  <rect/>\n  <b/>\n</svg>");
}

TEST_F(XMLDocumentTests, InsertNodeAppendsAtSiblingIndentBeforeClosingTag) {
  XMLDocument doc = ParseDocument("<svg>\n  <a/>\n  <b/>\n</svg>");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");

  ApplySourceEditResult result = doc.insertNode(svg, rect);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(doc.source(), "<svg>\n  <a/>\n  <b/>\n  <rect/>\n</svg>");
}

TEST_F(XMLDocumentTests, InsertNodeDetectsTabIndentUnit) {
  XMLDocument doc = ParseDocument("<svg>\n\t<g/>\n</svg>");
  XMLNode g = ElementChild(doc.root().firstChild().value(), 0);
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");

  ApplySourceEditResult result = doc.insertNode(g, rect);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(doc.source(), "<svg>\n\t<g>\n\t\t<rect/>\n\t</g>\n</svg>");
}

TEST_F(XMLDocumentTests, InsertNodeStaysCompactForSingleLineSource) {
  XMLDocument doc = ParseDocument(R"(<svg><g/></svg>)");
  XMLNode g = ElementChild(doc.root().firstChild().value(), 0);
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");

  ApplySourceEditResult result = doc.insertNode(g, rect);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(doc.source(), R"(<svg><g><rect/></g></svg>)");
}

TEST_F(XMLDocumentTests, MoveNodeToEndCarriesWhitespaceWithoutOrphaningOldLine) {
  XMLDocument doc = ParseDocument("<svg>\n  <a/>\n  <b/>\n  <c/>\n</svg>");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode a = ElementChild(svg, 0);

  // Move <a> to the end of its parent. The old line must not be left orphaned.
  ApplySourceEditResult result = doc.insertNode(svg, a);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(doc.source(), "<svg>\n  <b/>\n  <c/>\n  <a/>\n</svg>");
}

TEST_F(XMLDocumentTests, MoveNodeBeforeSiblingCarriesWhitespace) {
  XMLDocument doc = ParseDocument("<svg>\n  <a/>\n  <b/>\n  <c/>\n</svg>");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode a = ElementChild(svg, 0);
  XMLNode c = ElementChild(svg, 2);
  ASSERT_EQ(c.tagName(), XMLQualifiedNameRef("c"));

  // Move <c> to before <a>. The old <c> line must collapse cleanly.
  ApplySourceEditResult result = doc.insertNode(svg, c, a);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(doc.source(), "<svg>\n  <c/>\n  <a/>\n  <b/>\n</svg>");
}

//
// setElementText.
//

TEST_F(XMLDocumentTests, SetElementTextReplacesExistingTextAndEscapesSource) {
  XMLDocument doc = ParseDocument(R"(<svg><text>hello</text></svg>)");
  XMLNode text = doc.root().firstChild()->firstChild().value();

  ApplySourceEditResult result = doc.setElementText(text, "Tom & <Jerry>");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(text.value(), Optional(Eq("Tom & <Jerry>")));
  EXPECT_EQ(doc.source(), R"(<svg><text>Tom &amp; &lt;Jerry&gt;</text></svg>)");
  EXPECT_THAT(MutationKinds(result), testing::ElementsAre(XMLMutation::Kind::NodeValueChanged));
}

TEST_F(XMLDocumentTests, SetElementTextClearsExistingText) {
  XMLDocument doc = ParseDocument(R"(<svg><text>hello</text></svg>)");
  XMLNode text = doc.root().firstChild()->firstChild().value();

  ApplySourceEditResult result = doc.setElementText(text, "");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  EXPECT_THAT(text.value(), Optional(Eq("")));
  EXPECT_FALSE(text.getValueLocation().has_value());
  EXPECT_EQ(doc.source(), R"(<svg><text></text></svg>)");
}

TEST_F(XMLDocumentTests, SetElementTextInsertsIntoEmptyElement) {
  XMLDocument doc = ParseDocument(R"(<svg><text></text></svg>)");
  XMLNode text = doc.root().firstChild()->firstChild().value();

  ApplySourceEditResult result = doc.setElementText(text, "hello");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  EXPECT_THAT(text.value(), Optional(Eq("hello")));
  EXPECT_EQ(doc.source(), R"(<svg><text>hello</text></svg>)");
}

TEST_F(XMLDocumentTests, SetElementTextStaysInlineEvenInBlockLaidOutSource) {
  // Text content is authored inline regardless of the surrounding block layout: expanding a
  // self-closing <text/> yields <text>hello</text>, not a text node on its own line.
  XMLDocument doc = ParseDocument("<svg>\n  <text/>\n</svg>");
  XMLNode text = ElementChild(doc.root().firstChild().value(), 0);

  ApplySourceEditResult result = doc.setElementText(text, "hello");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(doc.source(), "<svg>\n  <text>hello</text>\n</svg>");
}

TEST_F(XMLDocumentTests, SetElementTextExpandsSelfClosingElement) {
  XMLDocument doc = ParseDocument(R"(<svg><text/></svg>)");
  XMLNode text = doc.root().firstChild()->firstChild().value();

  ApplySourceEditResult result = doc.setElementText(text, "hello");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  EXPECT_THAT(text.value(), Optional(Eq("hello")));
  EXPECT_EQ(doc.source(), R"(<svg><text>hello</text></svg>)");
}

TEST_F(XMLDocumentTests, SetElementTextExpandsSelfClosingElementWithWhitespaceBeforeSlash) {
  for (std::string_view whitespace : {"\t", "\n", "\r"}) {
    SCOPED_TRACE(testing::Message() << "whitespace byte " << static_cast<int>(whitespace[0]));
    XMLDocument doc =
        ParseDocument(std::string("<svg><text") + std::string(whitespace) + "/></svg>");
    XMLNode text = doc.root().firstChild()->firstChild().value();

    ApplySourceEditResult result = doc.setElementText(text, "hello");

    EXPECT_TRUE(result.applied);
    EXPECT_EQ(result.scope, ReparseScope::TextNode);
    EXPECT_THAT(text.value(), Optional(Eq("hello")));
    EXPECT_EQ(doc.source(), R"(<svg><text>hello</text></svg>)");
  }
}

TEST_F(XMLDocumentTests, SetElementTextEmptyTextWithoutExistingValueIsNoOp) {
  XMLDocument doc = ParseDocument(R"(<svg><text></text></svg>)");
  XMLNode text = doc.root().firstChild()->firstChild().value();

  ApplySourceEditResult result = doc.setElementText(text, "");

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_EQ(doc.source(), R"(<svg><text></text></svg>)");
}

TEST_F(XMLDocumentTests, SetElementTextOnNodeFromOtherDocumentFails) {
  XMLDocument doc = ParseDocument(R"(<svg><text/></svg>)");
  XMLDocument other;
  XMLNode foreign = XMLNode::CreateElementNode(other, "text");

  ApplySourceEditResult result = doc.setElementText(foreign, "hello");

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("another document"));
}

TEST_F(XMLDocumentTests, SetElementTextWithoutSourceStoreFails) {
  XMLDocument doc;
  XMLNode text = XMLNode::CreateElementNode(doc, "text");
  doc.root().appendChild(text);

  ApplySourceEditResult result = doc.setElementText(text, "hello");

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("without source text"));
}

TEST_F(XMLDocumentTests, SetElementTextOnNonElementNodeFails) {
  XMLDocument doc = ParseDocument(R"(<svg>hello</svg>)");
  XMLNode data = doc.root().firstChild()->firstChild().value();
  ASSERT_EQ(data.type(), XMLNode::Type::Data);

  ApplySourceEditResult result = doc.setElementText(data, "world");

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("non-element"));
}

TEST_F(XMLDocumentTests, SetElementTextRejectsInvalidXmlText) {
  XMLDocument doc = ParseDocument(R"(<svg><text>hello</text></svg>)");
  XMLNode text = doc.root().firstChild()->firstChild().value();
  const std::string invalidText("bad\001text", 8);

  ApplySourceEditResult result = doc.setElementText(text, invalidText);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("cannot be represented"));
  EXPECT_EQ(doc.source(), R"(<svg><text>hello</text></svg>)");
}

TEST_F(XMLDocumentTests, SetElementTextRejectsInsertionWhenEndOffsetIsNotUtf8Boundary) {
  XMLDocument doc = ParseDocument(R"(<svg><text/></svg>)");
  XMLNode text = doc.root().firstChild()->firstChild().value();
  std::string corruptSource(doc.source());
  const std::size_t selfClosingEnd = corruptSource.find("/>") + 2;
  corruptSource.insert(selfClosingEnd, std::string("\x80", 1));
  doc.setSource(corruptSource);

  ApplySourceEditResult result = doc.setElementText(text, "hello");

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("Invalid source replacement"));
  EXPECT_FALSE(text.value().has_value());
  EXPECT_EQ(doc.source(), corruptSource);
}

TEST_F(XMLDocumentTests, SetElementTextRejectsExistingTextWhenOffsetIsNotUtf8Boundary) {
  XMLDocument doc = ParseDocument(R"(<svg><text>hello</text></svg>)");
  XMLNode text = doc.root().firstChild()->firstChild().value();
  std::string corruptSource(doc.source());
  const std::size_t valueOffset = corruptSource.find("hello");
  ASSERT_NE(valueOffset, std::string::npos);
  corruptSource.insert(valueOffset, std::string("\x80", 1));
  doc.setSource(corruptSource);

  ApplySourceEditResult result = doc.setElementText(text, "world");

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("source insertion point"));
  EXPECT_THAT(text.value(), Optional(Eq("hello")));
  EXPECT_EQ(doc.source(), corruptSource);
}

TEST_F(XMLDocumentTests, SetElementTextOnSourcelessElementInSourceDocumentFails) {
  XMLDocument doc = ParseDocument(R"(<svg></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode text = XMLNode::CreateElementNode(doc, "text");
  svg.appendChild(text);

  ApplySourceEditResult result = doc.setElementText(text, "hello");

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("source insertion point"));
  EXPECT_FALSE(text.value().has_value());
}

//
// applySourceEdit - scope-detection edge cases and reparse error paths.
//

TEST_F(XMLDocumentTests, ApplySourceEditOnEmptySourceFallsBackToDocumentScope) {
  XMLDocument doc;
  doc.setSource("");

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(0), FileOffset::Offset(0)},
      .replacement = "<svg/>",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::Document);
  EXPECT_THAT(result, DiagnosticReasonContains("Only attribute-value"));
  EXPECT_EQ(doc.source(), "<svg/>");
}

TEST_F(XMLDocumentTests, ApplySourceEditAfterRootElementFallsBackToDocumentScope) {
  XMLDocument doc = ParseDocument(R"(<svg/><!--tail-->)");
  const std::size_t tailOffset = doc.source().find("tail");
  ASSERT_NE(tailOffset, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(tailOffset), FileOffset::Offset(tailOffset + 4)},
      .replacement = "TAIL",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::Document);
  EXPECT_THAT(result, DiagnosticReasonContains("Only attribute-value"));
  EXPECT_EQ(doc.source(), R"(<svg/><!--TAIL-->)");
}

TEST_F(XMLDocumentTests, ApplySourceEditSpanningPastTextNodeEndFallsBackToDocumentScope) {
  XMLDocument doc = ParseDocument(R"(<text>hi</text>)");
  XMLNode text = doc.root().firstChild().value();
  const std::size_t textOffset = doc.source().find("hi");
  ASSERT_NE(textOffset, std::string_view::npos);

  // The edit starts inside the data node but extends past its end into the closing tag,
  // so no narrower reparse scope applies.
  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(textOffset), FileOffset::Offset(textOffset + 4)},
      .replacement = "yo</",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::Document);
  EXPECT_THAT(result, DiagnosticReasonContains("Only attribute-value"));
  EXPECT_EQ(doc.source(), R"(<text>yo</text>)");
  EXPECT_THAT(text.value(), Optional(Eq("hi")));
}

TEST_F(XMLDocumentTests, ApplySourceEditOutsideElementValueRangeFallsBackToSubtreeScope) {
  XMLDocument doc = ParseDocument(R"(<text>hello</text>)");
  XMLNode text = doc.root().firstChild().value();
  XMLNode textChild = text.firstChild().value();
  textChild.clearSourceLocation();
  const std::size_t textOffset = doc.source().find("hello");
  ASSERT_NE(textOffset, std::string_view::npos);
  // Shrink the element's tracked value range so the edit lands outside it.
  text.setValueLocation(
      SourceRange{FileOffset::Offset(textOffset), FileOffset::Offset(textOffset + 2)});

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(textOffset + 3), FileOffset::Offset(textOffset + 5)},
      .replacement = "LO",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_EQ(doc.source(), R"(<text>helLO</text>)");
  EXPECT_THAT(text.value(), Optional(Eq("helLO")));
}

TEST_F(XMLDocumentTests, ApplySourceEditSpanningIntoSecondAttributeUsesOpeningTagScope) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red" stroke="blue"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  const std::size_t editStart = doc.source().find("red") + 1;
  const std::size_t editEnd = doc.source().find("blue") + 4;
  ASSERT_NE(doc.source().find("red"), std::string_view::npos);
  ASSERT_NE(doc.source().find("blue"), std::string_view::npos);

  // Starts inside the fill value but extends past the end of the fill attribute, so it cannot
  // be an attribute-value edit and falls back to an opening-tag reparse.
  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart), FileOffset::Offset(editEnd)},
      .replacement = R"(ed" opacity="0.5)",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::OpeningTag);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(rect.getAttribute("fill"), Optional(Eq("red")));
  EXPECT_THAT(rect.getAttribute("opacity"), Optional(Eq("0.5")));
  EXPECT_FALSE(rect.hasAttribute("stroke"));
  EXPECT_EQ(doc.source(), R"(<svg><rect fill="red" opacity="0.5"/></svg>)");
}

TEST_F(XMLDocumentTests, ApplySourceEditSubtreeSplitIntoTwoElementsReportsDiagnostic) {
  XMLDocument doc = ParseDocument(R"(<g><rect/></g>)");
  XMLNode g = doc.root().firstChild().value();
  const std::size_t editStart = doc.source().find("<rect/>");
  ASSERT_NE(editStart, std::string_view::npos);

  // Splitting the parent into two sibling elements is not a valid subtree replacement.
  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart), FileOffset::Offset(editStart + 7)},
      .replacement = "</g><g>",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_THAT(result, DiagnosticReasonContains("did not produce one element"));
  ASSERT_TRUE(g.firstChild().has_value());
  EXPECT_EQ(g.firstChild()->tagName(), XMLQualifiedNameRef("rect"));
}

TEST_F(XMLDocumentTests, ApplySourceEditSubtreeReusedChildDropsRemovedTextValue) {
  XMLDocument doc = ParseDocument(R"(<g><t id="k">old</t></g>)");
  XMLNode g = doc.root().firstChild().value();
  XMLNode t = g.firstChild().value();
  ASSERT_THAT(t.value(), Optional(Eq("old")));
  const std::size_t editStart = doc.source().find("<t");
  const std::size_t editEnd = doc.source().find("</g>");
  ASSERT_NE(editStart, std::string_view::npos);
  ASSERT_NE(editEnd, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart), FileOffset::Offset(editEnd)},
      .replacement = R"(<t id="k"/>)",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::ElementSubtree);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  ASSERT_TRUE(g.firstChild().has_value());
  EXPECT_EQ(*g.firstChild(), t);
  EXPECT_THAT(t.value(), Eq(std::nullopt));
  EXPECT_FALSE(t.firstChild().has_value());
  EXPECT_THAT(MutationKinds(result), testing::Contains(XMLMutation::Kind::NodeValueChanged));
}

TEST_F(XMLDocumentTests, ApplySourceEditTextNodeUnclosedMarkupReportsParseError) {
  XMLDocument doc = ParseDocument(R"(<text>hello</text>)");
  XMLNode textNode = doc.root().firstChild()->firstChild().value();
  const std::size_t textOffset = doc.source().find("hello");
  ASSERT_NE(textOffset, std::string_view::npos);

  ApplySourceEditResult result = doc.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(textOffset), FileOffset::Offset(textOffset + 5)},
      .replacement = "<a>",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, ReparseScope::TextNode);
  ASSERT_TRUE(result.diagnostic.has_value());
  EXPECT_THAT(textNode.value(), Optional(Eq("hello")));
  EXPECT_EQ(doc.source(), R"(<text><a></text>)");
}

TEST_F(XMLDocumentTests, InternalNodeIdentityMatchesDocumentAndProcessingInstructionNodes) {
  XMLDocument docA;
  XMLDocument docB;

  EXPECT_TRUE(internal::HasCompatibleNodeIdentity(docA.root(), docB.root()));

  XMLNode piTarget = XMLNode::CreateProcessingInstructionNode(docA, "target", "data");
  XMLNode piSameTarget = XMLNode::CreateProcessingInstructionNode(docA, "target", "other");
  XMLNode piOtherTarget = XMLNode::CreateProcessingInstructionNode(docA, "renamed", "data");
  EXPECT_TRUE(internal::HasCompatibleNodeIdentity(piTarget, piSameTarget));
  EXPECT_FALSE(internal::HasCompatibleNodeIdentity(piTarget, piOtherTarget));

  XMLNode declarationA = XMLNode::CreateXMLDeclarationNode(docA);
  XMLNode declarationB = XMLNode::CreateXMLDeclarationNode(docA);
  EXPECT_TRUE(internal::HasCompatibleNodeIdentity(declarationA, declarationB));
}

//
// insertNode - move error paths.
//

TEST_F(XMLDocumentTests, InsertNodeMoveBeforeReferenceWithoutSourceRangeFails) {
  XMLDocument doc = ParseDocument(R"(<svg><a><m/></a><b><r/></b></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode a = svg.firstChild().value();
  XMLNode b = a.nextSibling().value();
  XMLNode m = a.firstChild().value();
  XMLNode r = b.firstChild().value();
  r.clearSourceLocation();

  ApplySourceEditResult result = doc.insertNode(b, m, r);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result,
              DiagnosticReasonContains("Cannot move node without a source insertion point"));
  ASSERT_TRUE(a.firstChild().has_value());
  EXPECT_EQ(*a.firstChild(), m);
  EXPECT_EQ(doc.source(), R"(<svg><a><m/></a><b><r/></b></svg>)");
}

TEST_F(XMLDocumentTests, InsertNodeMoveOverlappingOwnSourceRangeFails) {
  XMLDocument doc = ParseDocument(R"(<svg><m/><b></b></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode m = svg.firstChild().value();
  XMLNode b = m.nextSibling().value();
  // Extend the moved node's stale source range so the insertion point inside <b> lands within
  // it.
  m.setSourceEndOffset(FileOffset::Offset(14));

  ApplySourceEditResult result = doc.insertNode(b, m);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("inside its own source range"));
  EXPECT_EQ(doc.source(), R"(<svg><m/><b></b></svg>)");
}

TEST_F(XMLDocumentTests, InsertNodeMoveWithMalformedSourceRangeReportsParseError) {
  XMLDocument doc = ParseDocument(R"(<svg><m/><b></b></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode m = svg.firstChild().value();
  XMLNode b = m.nextSibling().value();
  // The stale range covers "<m/><b", which does not reparse as a single element.
  m.setSourceEndOffset(FileOffset::Offset(11));

  ApplySourceEditResult result = doc.insertNode(b, m);

  EXPECT_FALSE(result.applied);
  ASSERT_TRUE(result.diagnostic.has_value());
  EXPECT_EQ(doc.source(), R"(<svg><m/><b></b></svg>)");
  ASSERT_TRUE(svg.firstChild().has_value());
  EXPECT_EQ(*svg.firstChild(), m);
}

TEST_F(XMLDocumentTests, InsertNodeMoveOfCommentOnlySourceRangeFails) {
  XMLDocument doc = ParseDocument(R"(<svg><!--c--><m/><b></b></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode m = svg.firstChild().value();
  ASSERT_EQ(m.tagName(), XMLQualifiedNameRef("m"));
  XMLNode b = m.nextSibling().value();
  // Point the moved node's stale source range at the comment, which reparses successfully but
  // yields no element.
  m.setSourceStartOffset(FileOffset::Offset(5));
  m.setSourceEndOffset(FileOffset::Offset(13));

  ApplySourceEditResult result = doc.insertNode(b, m);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("Moved source did not produce an element"));
  EXPECT_EQ(doc.source(), R"(<svg><!--c--><m/><b></b></svg>)");
}

TEST_F(XMLDocumentTests, InsertNodeMoveBackwardRemovalAtNonUtf8BoundaryFails) {
  XMLDocument doc = ParseDocument(R"(<svg><b></b><m/></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode b = svg.firstChild().value();
  XMLNode m = b.nextSibling().value();
  // Corrupt the source so the byte just past the moved node is a bare UTF-8 continuation
  // byte; the removal half of the move then fails at that boundary.
  std::string corruptSource(doc.source());
  corruptSource.insert(corruptSource.find("<m/>") + 4, std::string("\x80", 1));
  doc.setSource(corruptSource);

  ApplySourceEditResult result = doc.insertNode(b, m);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("Invalid source removal for node move"));
  ASSERT_TRUE(b.nextSibling().has_value());
  EXPECT_EQ(*b.nextSibling(), m);
}

TEST_F(XMLDocumentTests, InsertNodeMoveForwardRemovalAtNonUtf8BoundaryFails) {
  XMLDocument doc = ParseDocument(R"(<svg><m/><b></b></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode m = svg.firstChild().value();
  XMLNode b = m.nextSibling().value();
  // Corrupt the source so the byte just past the moved node is a bare UTF-8 continuation
  // byte, then restore <b>'s locations (setSource invalidated its anchors).
  std::string corruptSource(doc.source());
  corruptSource.insert(9, std::string("\x80", 1));
  doc.setSource(corruptSource);
  b.setSourceStartOffset(FileOffset::Offset(10));
  b.setSourceEndOffset(FileOffset::Offset(17));
  b.setClosingTagLocation(SourceRange{FileOffset::Offset(13), FileOffset::Offset(17)});

  ApplySourceEditResult result = doc.insertNode(b, m);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("Invalid source removal for node move"));
  ASSERT_TRUE(svg.firstChild().has_value());
  EXPECT_EQ(*svg.firstChild(), m);
  EXPECT_EQ(doc.source(), corruptSource);
}

TEST_F(XMLDocumentTests, InsertNodeMoveBackwardInsertionAtNonUtf8BoundaryFails) {
  // The two-byte UTF-8 sequence \xC3\xA9 sits inside <b>; a stale reference offset pointing
  // at its continuation byte makes the insertion half of the backward move fail.
  XMLDocument doc = ParseDocument("<svg><b>\xC3\xA9<r/></b><m/></svg>");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode b = svg.firstChild().value();
  XMLNode r = ElementChild(b, 0);
  XMLNode m = b.nextSibling().value();
  ASSERT_EQ(m.tagName(), XMLQualifiedNameRef("m"));
  r.setSourceStartOffset(FileOffset::Offset(9));

  ApplySourceEditResult result = doc.insertNode(b, m, r);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("Invalid source replacement for node move"));
  EXPECT_EQ(doc.source(), "<svg><b>\xC3\xA9<r/></b><m/></svg>");
}

TEST_F(XMLDocumentTests, InsertNodeMoveForwardInsertionAtNonUtf8BoundaryFails) {
  XMLDocument doc = ParseDocument("<svg><m/><b>\xC3\xA9<r/></b></svg>");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode m = svg.firstChild().value();
  XMLNode b = m.nextSibling().value();
  XMLNode r = ElementChild(b, 0);
  // Stale reference offset pointing at the continuation byte of \xC3\xA9. The removal half of
  // the forward move succeeds, then the shifted insertion fails at the non-boundary offset.
  r.setSourceStartOffset(FileOffset::Offset(13));

  ApplySourceEditResult result = doc.insertNode(b, m, r);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("Invalid source replacement for node move"));
  ASSERT_TRUE(svg.firstChild().has_value());
  EXPECT_EQ(*svg.firstChild(), m);
}

TEST_F(XMLDocumentTests, InsertNodeBeforeReferenceWithOutOfRangeSourceOffsetFails) {
  XMLDocument doc = ParseDocument(R"(<svg><r/></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode r = svg.firstChild().value();
  r.setSourceStartOffset(FileOffset::Offset(20));
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");

  ApplySourceEditResult result = doc.insertNode(svg, rect, r);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("Invalid source replacement for node insertion"));
  EXPECT_EQ(doc.source(), R"(<svg><r/></svg>)");
}

TEST_F(XMLDocumentTests, InsertNodeMovesElementWithChildrenAndSyncsChildLocations) {
  XMLDocument doc = ParseDocument(R"(<svg><m><c/></m><b></b></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode m = svg.firstChild().value();
  XMLNode c = m.firstChild().value();
  XMLNode b = m.nextSibling().value();

  ApplySourceEditResult result = doc.insertNode(b, m);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(doc.source(), R"(<svg><b><m><c/></m></b></svg>)");
  ASSERT_TRUE(b.firstChild().has_value());
  EXPECT_EQ(*b.firstChild(), m);
  ASSERT_TRUE(m.firstChild().has_value());
  EXPECT_EQ(*m.firstChild(), c);
  std::optional<SourceRange> cLocation = c.getNodeLocation();
  ASSERT_TRUE(cLocation.has_value());
  ExpectSourceRangeOffsets(*cLocation, doc.source().find("<c/>"), doc.source().find("<c/>") + 4);
}

TEST_F(XMLDocumentTests, InsertNodeMoveReplacesDomOnlyChildrenFromSource) {
  XMLDocument doc = ParseDocument(R"(<svg><m><c/></m><b></b></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode m = svg.firstChild().value();
  XMLNode c = m.firstChild().value();
  XMLNode b = m.nextSibling().value();
  // Replace the source-backed child with a DOM-only data node; the move re-syncs the moved
  // subtree from its serialized source, which still contains <c/>.
  m.removeChild(c);
  m.appendChild(XMLNode::CreateDataNode(doc, "x"));

  ApplySourceEditResult result = doc.insertNode(b, m);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(doc.source(), R"(<svg><b><m><c/></m></b></svg>)");
  ASSERT_TRUE(m.firstChild().has_value());
  EXPECT_EQ(m.firstChild()->type(), XMLNode::Type::Element);
  EXPECT_EQ(m.firstChild()->tagName(), XMLQualifiedNameRef("c"));
  EXPECT_FALSE(m.firstChild()->nextSibling().has_value());
}

TEST_F(XMLDocumentTests, InsertNodeReconcilesCommentChildDroppedByReparse) {
  XMLDocument doc = ParseDocument(R"(<svg></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");
  rect.appendChild(XMLNode::CreateCommentNode(doc, "note"));

  ApplySourceEditResult result = doc.insertNode(svg, rect);

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(std::string(doc.source()), testing::HasSubstr("<!--note-->"));
  ASSERT_TRUE(svg.firstChild().has_value());
  EXPECT_EQ(*svg.firstChild(), rect);
  // The reparse of the serialized node drops the comment, so the reconciliation removes the
  // DOM-only comment child.
  EXPECT_FALSE(rect.firstChild().has_value());
}

TEST_F(XMLDocumentTests, InsertNodeOfDataNodeReportsDiagnostic) {
  XMLDocument doc = ParseDocument(R"(<svg></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode data = XMLNode::CreateDataNode(doc, "hi");

  ApplySourceEditResult result = doc.insertNode(svg, data);

  EXPECT_FALSE(result.applied);
  ASSERT_TRUE(result.diagnostic.has_value());
  EXPECT_EQ(doc.source(), R"(<svg></svg>)");
  EXPECT_FALSE(svg.firstChild().has_value());
}

//
// Structural mutation failures at corrupted UTF-8 boundaries.
//

TEST_F(XMLDocumentTests, SetAttributeInsertionWithInvalidUtf8NameFails) {
  XMLDocument doc = ParseDocument(R"(<svg><rect/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  // Attribute names are serialized verbatim, so a name that is not valid UTF-8 is rejected
  // when the insertion is written back to the source store.
  const std::string invalidName("\x80name", 5);

  ApplySourceEditResult result = doc.setAttribute(rect, XMLQualifiedNameRef(invalidName), "blue");

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result,
              DiagnosticReasonContains("Invalid source replacement for attribute insertion"));
  EXPECT_FALSE(rect.hasAttribute(XMLQualifiedNameRef(invalidName)));
  EXPECT_EQ(doc.source(), R"(<svg><rect/></svg>)");
}

TEST_F(XMLDocumentTests, RemoveAttributeAtNonUtf8BoundaryFails) {
  XMLDocument doc = ParseDocument(R"(<svg><rect fill="red"/></svg>)");
  XMLNode rect = doc.root().firstChild()->firstChild().value();
  // Insert a bare continuation byte right after the closing quote; the removal range then
  // ends at a non-boundary offset.
  std::string corruptSource(doc.source());
  const std::size_t afterQuote = corruptSource.find(R"("red")") + 5;
  corruptSource.insert(afterQuote, std::string("\x80", 1));
  doc.setSource(corruptSource);

  ApplySourceEditResult result = doc.removeAttribute(rect, "fill");

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("Invalid source replacement for attribute removal"));
  EXPECT_TRUE(rect.hasAttribute("fill"));
  EXPECT_EQ(doc.source(), corruptSource);
}

TEST_F(XMLDocumentTests, RemoveNodeAtNonUtf8BoundaryFails) {
  XMLDocument doc = ParseDocument(R"(<svg><rect/></svg>)");
  XMLNode svg = doc.root().firstChild().value();
  XMLNode rect = svg.firstChild().value();
  // Insert a bare continuation byte right after the node; its removal range then ends at a
  // non-boundary offset.
  std::string corruptSource(doc.source());
  corruptSource.insert(corruptSource.find("<rect/>") + 7, std::string("\x80", 1));
  doc.setSource(corruptSource);

  ApplySourceEditResult result = doc.removeNode(rect);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result, DiagnosticReasonContains("Invalid source replacement for node removal"));
  ASSERT_TRUE(svg.firstChild().has_value());
  EXPECT_EQ(*svg.firstChild(), rect);
  EXPECT_EQ(doc.source(), corruptSource);
}

TEST_F(XMLDocumentTests, SetElementTextWithReversedValueRangeClearsRangeAndAppends) {
  XMLDocument doc = ParseDocument(R"(<text>hello</text>)");
  XMLNode text = doc.root().firstChild().value();
  const std::size_t textOffset = doc.source().find("hello");
  ASSERT_NE(textOffset, std::string_view::npos);

  // A reversed range is rejected by setValueLocation, which clears the tracked value range.
  text.setValueLocation(
      SourceRange{FileOffset::Offset(textOffset + 5), FileOffset::Offset(textOffset)});
  EXPECT_THAT(text.getValueLocation(), Eq(std::nullopt));

  // With no tracked value range, setElementText falls back to inserting before the closing
  // tag instead of replacing the existing text.
  ApplySourceEditResult result = doc.setElementText(text, "world");

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(text.value(), Optional(Eq("world")));
  EXPECT_EQ(doc.source(), R"(<text>helloworld</text>)");
}

//
// Structural source-formatting: indentation detection fallbacks.
//

TEST_F(XMLDocumentTests, InsertNodeIntoUnindentedParentUsesFallbackIndentUnit) {
  XMLDocument doc = ParseDocument("<svg>\n<g/>\n</svg>");
  XMLNode g = ElementChild(doc.root().firstChild().value(), 0);
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");

  // No line in the document is indented, so the detector falls back to two spaces.
  ApplySourceEditResult result = doc.insertNode(g, rect);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(doc.source(), "<svg>\n<g>\n  <rect/>\n</g>\n</svg>");
}

TEST_F(XMLDocumentTests, InsertNodeIntoEmptyMultilineParentIndentsPastClosingTag) {
  XMLDocument doc = ParseDocument("<svg>\n  <g>\n  </g>\n</svg>");
  XMLNode g = ElementChild(doc.root().firstChild().value(), 0);
  XMLNode rect = XMLNode::CreateElementNode(doc, "rect");

  // The empty parent has no deeper sibling to copy indentation from, so the child indents one
  // detected unit past the closing tag.
  ApplySourceEditResult result = doc.insertNode(g, rect);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(doc.source(), "<svg>\n  <g>\n    <rect/>\n  </g>\n</svg>");
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
