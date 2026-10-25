#include "donner/svg/SVGDocument.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Transform.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/xml/XMLDocument.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/svg/AllSVGElements.h"
#include "donner/svg/SVGAnimationQuery.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGStyleElement.h"
#include "donner/svg/SVGStyleQuery.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/components/ParsedPayloadResourceBudget.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/animation/AnimationReference.h"
#include "donner/svg/components/animation/AnimationStateComponent.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/paint/ClipPathComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/components/text/TextRootComponent.h"
#include "donner/svg/internal/RenderingDiagnostics.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/properties/PropertyRegistry.h"
#include "donner/svg/renderer/Renderer.h"

using testing::Eq;
using testing::Optional;

namespace donner::svg {

namespace {

/// Helper to parse an SVG string and return the resulting document.
SVGDocument ParseSVG(std::string_view input, parser::SVGParser::Options options = {}) {
  options.disableUserAttributes = false;

  ParseWarningSink disabled = ParseWarningSink::Disabled();
  auto maybeResult = parser::SVGParser::ParseSVG(input, disabled, options);
  EXPECT_THAT(maybeResult, NoParseError());
  return std::move(maybeResult).result();
}

/// The id of a query result, so a presence assertion prints which element matched.
std::optional<RcString> QueriedId(const std::optional<SVGElement>& element) {
  return element ? std::optional<RcString>(element->id()) : std::nullopt;
}

/// Matcher to check for an element with the given id.
MATCHER_P(ElementIdEq, id, "") {
  return arg.id() == id;
}

components::RenderTreeState& EnsureRenderTreeState(Registry& registry) {
  if (!registry.ctx().contains<components::RenderTreeState>()) {
    registry.ctx().emplace<components::RenderTreeState>();
  }
  return registry.ctx().get<components::RenderTreeState>();
}

constexpr std::string_view kSvgNamespaceUri = "http://www.w3.org/2000/svg";
constexpr std::string_view kForeignNamespaceUri = "http://example.test/foreign";

/// Apply a source edit that replaces the first \p needle in \p document's source.
xml::ApplySourceEditResult ReplaceInSource(SVGDocument& document, std::string_view needle,
                                           std::string_view replacement) {
  const std::size_t start = document.source().find(needle);
  if (start == std::string_view::npos) {
    ADD_FAILURE() << "missing source text: " << needle;
    return {};
  }
  return document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(start), FileOffset::Offset(start + needle.size())},
      .replacement = replacement,
      .sourceVersion = document.sourceVersion(),
  });
}

/// A descriptive element type and its tag.
struct DescriptiveCase {
  std::string_view tag;
  ElementType type;
};

constexpr std::array<DescriptiveCase, 3> kDescriptiveCases = {{
    {"title", ElementType::Title},
    {"desc", ElementType::Desc},
    {"metadata", ElementType::Metadata},
}};

/// The text of a `<title>`, `<desc>` or `<metadata>` element.
RcString DescriptiveText(SVGElement element) {
  switch (element.type()) {
    case ElementType::Title: return element.cast<SVGTitleElement>().textContent();
    case ElementType::Desc: return element.cast<SVGDescElement>().textContent();
    case ElementType::Metadata: return element.cast<SVGMetadataElement>().textContent();
    default: ADD_FAILURE() << "not a descriptive element: " << element.type(); return RcString();
  }
}

}  // namespace

TEST(SVGDocument, Create) {
  SVGDocument document;
  EXPECT_TRUE(document.rootEntityHandle());
  EXPECT_EQ(document.svgElement().ownerDocument(), document);
}

TEST(SVGDocument, UnsafeRegistryNamesRawEscapeHatch) {
  SVGDocument document;

  EXPECT_EQ(&document.unsafeRegistry(), &document.registry());
}

TEST(SVGDocument, RenderingDiagnosticsDoesNotBuildOrConsumeRenderingState) {
  auto document = ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="shape" width="10" height="10"/></svg>)");
  auto element = document.querySelector("#shape");
  ASSERT_TRUE(element.has_value());
  const auto revision = document.handle()->revision();
  const auto before = internal::CaptureRenderingDiagnostics(document, 0);
  EXPECT_TRUE(before.instances.empty());
  EXPECT_EQ(element->computedStyleIfPresent(), nullptr);
  EXPECT_EQ(document.handle()->revision(), revision);

  Renderer renderer;
  renderer.draw(document);
  const auto captured = internal::CaptureRenderingDiagnostics(document, 1);
  ASSERT_TRUE(captured.state.has_value());
  EXPECT_TRUE(captured.state->hasBeenBuilt);
  EXPECT_GT(captured.instanceCount, 0u);
  EXPECT_EQ(captured.instances.size(), 1u);

  element->setId("changed");
  const auto changedRevision = document.handle()->revision();
  const bool pending = document.hasPendingRenderInvalidation();
  const auto pendingState = internal::CaptureRenderingDiagnostics(document, 0);
  EXPECT_TRUE(pendingState.instances.empty());
  EXPECT_TRUE(pendingState.dirtyEntities.empty());
  EXPECT_EQ(document.hasPendingRenderInvalidation(), pending);
  EXPECT_EQ(document.handle()->revision(), changedRevision);
  EXPECT_TRUE(captured.state->hasBeenBuilt);
}

TEST(SVGDocument, CanvasSize) {
  SVGDocument document;
  EXPECT_EQ(document.canvasSize(), Vector2i(512, 512));
  EXPECT_EQ(document.canvasSizeOverride(), std::nullopt);

  document.setCanvasSize(100, 200);
  EXPECT_EQ(document.canvasSize(), Vector2i(100, 200));
  EXPECT_EQ(document.canvasSizeOverride(), std::optional(Vector2i(100, 200)));

  document.useAutomaticCanvasSize();
  EXPECT_EQ(document.canvasSize(), Vector2i(512, 512));
  EXPECT_EQ(document.canvasSizeOverride(), std::nullopt);
}

TEST(SVGDocument, SetCanvasSizeNoOpDoesNotInvalidateBuiltRenderTree) {
  SVGDocument document;
  document.setCanvasSize(100, 200);

  auto& renderState = EnsureRenderTreeState(document.registry());
  renderState.hasBeenBuilt = true;
  renderState.needsFullRebuild = false;
  renderState.needsFullStyleRecompute = false;
  document.registry().clear<components::DirtyFlagsComponent>();

  document.setCanvasSize(100, 200);

  EXPECT_FALSE(document.hasPendingRenderInvalidation());
  EXPECT_EQ(document.canvasSize(), Vector2i(100, 200));

  document.setCanvasSize(100, 201);

  EXPECT_TRUE(document.hasPendingRenderInvalidation());
  EXPECT_EQ(document.canvasSize(), Vector2i(100, 201));
}

TEST(SVGDocument, SetTimeNoOpDoesNotInvalidateBuiltRenderTree) {
  SVGDocument document;
  document.setTime(5.0);
  EXPECT_EQ(document.currentTime(), 5.0);

  auto& renderState = EnsureRenderTreeState(document.registry());
  renderState.hasBeenBuilt = true;
  renderState.needsFullRebuild = false;
  renderState.needsFullStyleRecompute = false;
  document.registry().clear<components::DirtyFlagsComponent>();

  document.setTime(5.0);

  EXPECT_FALSE(document.hasPendingRenderInvalidation());
  EXPECT_EQ(document.currentTime(), 5.0);

  document.setTime(6.0);

  EXPECT_TRUE(document.hasPendingRenderInvalidation());
  EXPECT_EQ(document.currentTime(), 6.0);
}

TEST(SVGDocument, UserLanguagesRoundTripAndNoOpSkipsInvalidation) {
  SVGDocument document;
  EXPECT_THAT(document.userLanguages(), testing::ElementsAre(RcString("en")));

  auto& renderState = EnsureRenderTreeState(document.registry());
  renderState.hasBeenBuilt = true;
  renderState.needsFullRebuild = false;
  renderState.needsFullStyleRecompute = false;
  document.registry().clear<components::DirtyFlagsComponent>();

  document.setUserLanguages({RcString("en")});

  EXPECT_FALSE(document.hasPendingRenderInvalidation());
  EXPECT_THAT(document.userLanguages(), testing::ElementsAre(RcString("en")));

  document.setUserLanguages({RcString("de"), RcString("en")});

  EXPECT_TRUE(document.hasPendingRenderInvalidation());
  EXPECT_THAT(document.userLanguages(), testing::ElementsAre(RcString("de"), RcString("en")));
}

TEST(SVGDocument, CanvasSizeFromFile) {
  {
    auto document = ParseSVG(R"(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      </svg>
    )");
    EXPECT_EQ(document.canvasSize(), Vector2i(200, 200));
  }

  {
    auto document = ParseSVG(R"(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200" width="100" height="200">
      </svg>
    )");
    EXPECT_EQ(document.canvasSize(), Vector2i(100, 200));
  }
}

TEST(SVGDocument, SourceBackedAccessorsReflectParsedXmlSource) {
  SVGDocument emptyDocument;
  EXPECT_FALSE(emptyDocument.hasSourceStore());
  EXPECT_THAT(emptyDocument.source(), Eq(std::string_view()));
  EXPECT_EQ(emptyDocument.sourceVersion(), 0u);

  auto document = ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r"/></svg>)");

  EXPECT_TRUE(document.hasSourceStore());
  EXPECT_THAT(document.source(), testing::HasSubstr(R"(<rect id="r"/>)"));
  EXPECT_EQ(document.sourceVersion(), 0u);
}

/// Installing new source text through the document's XML facade replaces its source store. The
/// document's own source accessors, which the editor reads without document access, must then
/// report the new store rather than the one the document was parsed with.
TEST(SVGDocument, SourceAccessorsReportSourceInstalledThroughTheXmlDocument) {
  SVGDocument document =
      ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r"/></svg>)");
  // One edit first, so the parsed store's version differs from that of a new store.
  std::optional<SVGElement> rect = document.querySelector("#r");
  ASSERT_TRUE(rect.has_value());
  ASSERT_TRUE(document.setElementAttribute(*rect, "width", "4").applied);
  ASSERT_EQ(document.sourceVersion(), 1u);
  document.setThreadingMode(ThreadingMode::ConcurrentDom);

  constexpr std::string_view kReplacement =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><circle r="2"/></svg>)";
  document.withWriteAccess([&document, kReplacement](DocumentWriteAccess& /*access*/) {
    document.xmlDocument().setSource(std::string(kReplacement));
  });

  EXPECT_TRUE(document.hasSourceStore());
  EXPECT_THAT(document.source(), Eq(kReplacement));
  EXPECT_EQ(document.sourceVersion(), document.xmlDocument().sourceVersion());
  EXPECT_GT(document.sourceVersion(), 1u);
}

/// A document built from an XML tree without source text has no source store until source text is
/// installed through its XML facade, and its source accessors must report the store from then on.
TEST(SVGDocument, SourceAccessorsReportSourceInstalledOnADocumentBuiltWithoutSource) {
  xml::XMLDocument xmlTree;
  xml::XMLNode svg = xml::XMLNode::CreateElementNode(xmlTree, "svg");
  svg.setAttribute("xmlns", "http://www.w3.org/2000/svg");
  xmlTree.root().appendChild(svg);
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseXMLDocument(std::move(xmlTree), disabled);
  ASSERT_THAT(parsed, NoParseError());
  SVGDocument document = std::move(parsed).result();
  ASSERT_FALSE(document.hasSourceStore());
  document.setThreadingMode(ThreadingMode::ConcurrentDom);

  constexpr std::string_view kSource = R"(<svg xmlns="http://www.w3.org/2000/svg"/>)";
  document.withWriteAccess([&document, kSource](DocumentWriteAccess& /*access*/) {
    document.xmlDocument().setSource(std::string(kSource));
  });

  EXPECT_TRUE(document.hasSourceStore());
  EXPECT_THAT(document.source(), Eq(kSource));
  EXPECT_EQ(document.sourceVersion(), document.xmlDocument().sourceVersion());
}

TEST(SVGDocument, PendingRenderInvalidationRequiresBuiltRenderTree) {
  SVGDocument document;

  EXPECT_FALSE(document.hasPendingRenderInvalidation());

  auto& renderState = document.registry().ctx().emplace<components::RenderTreeState>();
  renderState.hasBeenBuilt = false;
  renderState.needsFullRebuild = true;
  renderState.needsFullStyleRecompute = true;
  document.svgElement().entityHandle().get_or_emplace<components::DirtyFlagsComponent>().mark(
      components::DirtyFlagsComponent::RenderInstance);

  EXPECT_FALSE(document.hasPendingRenderInvalidation());
}

TEST(SVGDocument, PendingRenderInvalidationReflectsDirtyFlagsAndFullInvalidation) {
  SVGDocument document;
  Registry& registry = document.registry();
  auto& renderState = EnsureRenderTreeState(registry);
  renderState.hasBeenBuilt = true;
  renderState.needsFullRebuild = false;
  renderState.needsFullStyleRecompute = false;
  registry.clear<components::DirtyFlagsComponent>();

  EXPECT_FALSE(document.hasPendingRenderInvalidation());

  document.svgElement().entityHandle().get_or_emplace<components::DirtyFlagsComponent>().mark(
      components::DirtyFlagsComponent::RenderInstance);

  EXPECT_TRUE(document.hasPendingRenderInvalidation());

  registry.clear<components::DirtyFlagsComponent>();
  renderState.needsFullRebuild = true;
  renderState.needsFullStyleRecompute = false;

  EXPECT_TRUE(document.hasPendingRenderInvalidation());

  renderState.needsFullRebuild = false;
  renderState.needsFullStyleRecompute = true;

  EXPECT_TRUE(document.hasPendingRenderInvalidation());
}

TEST(SVGDocument, QuerySelector) {
  {
    auto document = ParseSVG(R"(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
        <rect id="rect1" x="10" y="10" width="100" height="100" />
        <rect id="rect2" x="10" y="10" width="100" height="100" />
      </svg>
    )");

    EXPECT_THAT(document.querySelector("rect"), Optional(ElementIdEq("rect1")));
    EXPECT_THAT(document.querySelector("#rect2"), Optional(ElementIdEq("rect2")));
    EXPECT_THAT(document.querySelector("svg > :nth-child(2)"), Optional(ElementIdEq("rect2")));
    EXPECT_THAT(document.querySelector("does-not-exist"), Eq(std::nullopt));
    EXPECT_THAT(document.querySelector("["), Eq(std::nullopt));
  }
}

TEST(SVGDocument, SourceLessApplySourceEditReportsDiagnostic) {
  SVGDocument document;

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(0), FileOffset::Offset(0)},
      .replacement = "x",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_FALSE(result.applied);
  ASSERT_TRUE(result.diagnostic.has_value());
  EXPECT_THAT(result.diagnostic->reason, testing::HasSubstr("without XML source text"));
}

TEST(SVGDocument, ApplySourceEditProjectsAttributeValueMutation) {
  auto document =
      ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" fill="red"/></svg>)");
  SVGElement rect = document.querySelector("#r").value();
  const std::size_t valueOffset = document.source().find("red");
  ASSERT_NE(valueOffset, std::string_view::npos);

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 3)},
      .replacement = "blue",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, xml::ReparseScope::AttributeValue);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(rect.getAttribute("fill"), Optional(Eq("blue")));
  EXPECT_THAT(document.source(), testing::HasSubstr(R"(fill="blue")"));
}

TEST(SVGDocument, EmptySelectorPreservesAndRefreshesProjectedGenericText) {
  auto document = ParseSVG(R"svg(<svg xmlns="http://www.w3.org/2000/svg">
    <g id="generic">word<!--c--> </g><g id="data"><![CDATA[word]]></g>
  </svg>)svg");
  EXPECT_THAT(QueriedId(document.querySelector("#generic:empty")), Eq(std::nullopt));
  auto attribute = ReplaceInSource(document, "id=\"generic\"", "id=\"generic\" class=\"new\"");
  ASSERT_TRUE(attribute.applied);
  ASSERT_THAT(attribute.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(QueriedId(document.querySelector("#generic:empty")), Eq(std::nullopt));
  auto replacement = ReplaceInSource(document, "<g id=\"generic\" class=\"new\">word<!--c--> </g>",
                                     "<g id=\"generic\"/>");
  ASSERT_TRUE(replacement.applied);
  ASSERT_THAT(replacement.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(QueriedId(document.querySelector("#generic:empty")), Optional(testing::_));
  EXPECT_THAT(QueriedId(document.querySelector("#data:empty")), Eq(std::nullopt));
}

TEST(SVGDocument, EmptySelectorTextAuthoritySurvivesSourceAndTypedEdits) {
  auto document = ParseSVG(R"svg(<svg xmlns="http://www.w3.org/2000/svg">
    <text id="text">initial</text><title id="title">title</title>
  </svg>)svg");
  auto replacement = ReplaceInSource(document, "<text id=\"text\">initial</text>",
                                     "<text id=\"text\">new<!--c--><![CDATA[word]]></text>");
  ASSERT_TRUE(replacement.applied);
  ASSERT_THAT(replacement.diagnostic, Eq(std::nullopt));
  auto text = document.querySelector("#text")->cast<SVGTextElement>();
  auto node = xml::XMLNode::TryCast(text.unsafeEntityHandle()).value();
  for (auto child = node.firstChild(); child; child = child->nextSibling()) {
    if (child->type() == xml::XMLNode::Type::Data || child->type() == xml::XMLNode::Type::CData) {
      child->setValue(" ");
    }
  }
  EXPECT_TRUE(text.isEmptyForSelector(nullptr));
  text.appendText(" ");
  EXPECT_TRUE(text.isEmptyForSelector(nullptr));
  text.appendText("append");
  EXPECT_FALSE(text.isEmptyForSelector(nullptr));
  text.appendText("");
  EXPECT_FALSE(text.isEmptyForSelector(nullptr));
  text.setTextContent("");
  EXPECT_TRUE(text.isEmptyForSelector(nullptr));
  replacement = ReplaceInSource(document, "title</title>", "new<![CDATA[word]]></title>");
  ASSERT_TRUE(replacement.applied);
  ASSERT_THAT(replacement.diagnostic, Eq(std::nullopt));
  auto title = document.querySelector("#title")->cast<SVGTitleElement>();
  EXPECT_FALSE(title.isEmptyForSelector(nullptr));
  title.setTextContent("");
  EXPECT_TRUE(title.isEmptyForSelector(nullptr));
}

TEST(SVGDocument, EmptySelectorKeepsRetainedAuthorityForRawTextDeltas) {
  auto document = ParseSVG(R"svg(<svg xmlns="http://www.w3.org/2000/svg">
    <style>
      text { opacity: .2 } text:empty { opacity: .8 }
      #parent { opacity: .4 } #parent:has(> text:empty) { opacity: .6 }
    </style>
    <g id="parent"><text id="text">word</text></g>
  </svg>)svg");
  auto text = document.querySelector("#text").value();
  auto parent = document.querySelector("#parent").value();
  auto xmlDocument = document.xmlDocument();
  auto delta = xml::XMLNode::CreateDataNode(xmlDocument, " ");
  xml::XMLNode::TryCast(text.unsafeEntityHandle())->appendChild(delta);
  EXPECT_THAT(text.getComputedStyle().opacity.get(), Optional(.2));
  EXPECT_THAT(parent.getComputedStyle().opacity.get(), Optional(.4));
  auto changed = ReplaceInSource(document, "word", "new");
  ASSERT_THAT(changed.diagnostic, Eq(std::nullopt));
  EXPECT_FALSE(text.isEmptyForSelector(nullptr));
  changed = ReplaceInSource(document, "new", " ");
  ASSERT_THAT(changed.diagnostic, Eq(std::nullopt));
  EXPECT_TRUE(text.isEmptyForSelector(nullptr));
  EXPECT_THAT(text.getComputedStyle().opacity.get(), Optional(.8));
  EXPECT_THAT(parent.getComputedStyle().opacity.get(), Optional(.6));
}

TEST(SVGDocument, EmptySelectorIgnoresRejectedRawTextProjection) {
  parser::SVGParser::Options options;
  options.maximumContentProjectionChunks = 1;
  auto document =
      ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><text id="t"> </text></svg>)", options);
  auto text = document.querySelector("#t").value();
  EXPECT_TRUE(text.isEmptyForSelector(nullptr));
  auto changed = ReplaceInSource(document, "<text id=\"t\"> </text>",
                                 "<text id=\"t\">word<![CDATA[other]]></text>");
  ASSERT_TRUE(changed.applied);
  ASSERT_THAT(changed.diagnostic, Optional(testing::_));
  EXPECT_THAT(changed.diagnostic->reason, testing::HasSubstr("too many chunks"));
  EXPECT_EQ(text.entityHandle().get<components::TextComponent>().text, " ");
  EXPECT_TRUE(text.isEmptyForSelector(nullptr));
}

TEST(SVGDocument, EmptySelectorSourceSetterReplacesMaterializedRawText) {
  for (const std::string_view content :
       {"word<![CDATA[other]]>", "<![CDATA[word]]>", " <![CDATA[ ]]>"}) {
    SCOPED_TRACE(content);
    auto document =
        ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><text id="t">seed</text></svg>)");
    const std::string replacement = "<text id=\"t\">" + std::string(content) + "</text>";
    auto changed = ReplaceInSource(document, "<text id=\"t\">seed</text>", replacement);
    ASSERT_THAT(changed.diagnostic, Eq(std::nullopt));
    auto text = document.querySelector("#t").value();
    for (const std::string_view value : {" ", "new & <text>", ""}) {
      auto replaced = document.setElementTextContent(text, value);
      ASSERT_TRUE(replaced.applied);
      ASSERT_THAT(replaced.diagnostic, Eq(std::nullopt));
      auto fresh = ParseSVG(document.source());
      EXPECT_EQ(text.isEmptyForSelector(nullptr),
                fresh.querySelector("#t")->isEmptyForSelector(nullptr));
      EXPECT_EQ(text.cast<SVGTextElement>().textContent(), value);
      EXPECT_EQ(fresh.querySelector("#t")->cast<SVGTextElement>().textContent(), value);
    }
  }
  for (const bool initiallyEmpty : {false, true}) {
    SCOPED_TRACE(initiallyEmpty);
    auto document =
        ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><text id="t">seed</text></svg>)");
    auto changed =
        ReplaceInSource(document, "<text id=\"t\">seed</text>",
                        initiallyEmpty ? "<text id=\"t\"> </text>" : "<text id=\"t\">word</text>");
    ASSERT_THAT(changed.diagnostic, Eq(std::nullopt));
    auto text = document.querySelector("#t").value();
    EXPECT_EQ(text.isEmptyForSelector(nullptr), initiallyEmpty);
    auto replaced = document.setElementTextContent(text, initiallyEmpty ? "new" : " ");
    ASSERT_TRUE(replaced.applied);
    ASSERT_THAT(replaced.diagnostic, Eq(std::nullopt));
    EXPECT_EQ(text.isEmptyForSelector(nullptr), !initiallyEmpty);
  }
}

/// Whole-document invalidation observed after a typed text edit on a built render tree.
struct TextEditInvalidation {
  bool fullRebuild = false;
  bool fullStyleRecompute = false;
  bool siblingStyleRetained = false;
  std::optional<PaintServer> siblingFill;  ///< `#r` fill computed after the edit.

  bool operator==(const TextEditInvalidation& other) const = default;

  friend std::ostream& operator<<(std::ostream& os, const TextEditInvalidation& value) {
    return os << "{fullRebuild=" << value.fullRebuild
              << ", fullStyleRecompute=" << value.fullStyleRecompute
              << ", siblingStyleRetained=" << value.siblingStyleRetained << ", siblingFill=";
    if (value.siblingFill.has_value()) {
      os << *value.siblingFill;
    } else {
      os << "nullopt";
    }
    return os << "}";
  }
};

/// Apply the editor's text-typing sequence to `#t` after marking the render tree as built.
/// \p prepare runs on the parsed document first, before any style is computed.
TextEditInvalidation TypeTextIntoBuiltDocument(
    std::string_view svg, std::string_view typed,
    const std::function<void(SVGDocument&)>& prepare = {}) {
  SVGDocument document = ParseSVG(svg);
  if (prepare) {
    prepare(document);
  }
  SVGElement text = document.querySelector("#t").value();
  SVGElement sibling = document.querySelector("#r").value();
  std::ignore = sibling.getComputedStyle();
  auto& renderState = EnsureRenderTreeState(document.registry());
  renderState.hasBeenBuilt = true;
  renderState.needsFullRebuild = false;
  renderState.needsFullStyleRecompute = false;
  document.registry().clear<components::DirtyFlagsComponent>();

  const xml::ApplySourceEditResult result = document.setElementTextContent(text, typed);
  EXPECT_TRUE(result.applied) << svg;
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  text.cast<SVGTextContentElement>().setTextContent(typed);

  TextEditInvalidation invalidation{
      .fullRebuild = renderState.needsFullRebuild,
      .fullStyleRecompute = renderState.needsFullStyleRecompute,
      .siblingStyleRetained = sibling.entityHandle().all_of<components::ComputedStyleComponent>(),
  };
  invalidation.siblingFill = sibling.getComputedStyle().fill.get();
  return invalidation;
}

TEST(SVGDocument, TypedTextWithoutEmptySelectorKeepsIncrementalInvalidation) {
  const PaintServer kRed = PaintServer::Solid(css::Color(css::RGBA(0xFF, 0, 0, 0xFF)));
  for (const std::string_view initial : {R"(<text id="t"/>)", R"(<text id="t">S</text>)"}) {
    SCOPED_TRACE(initial);
    const std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg">
      <style>rect { fill: red } text:first-child + rect { opacity: .5 }</style>
      <rect id="r" width="10" height="10"/>)" +
                            std::string(initial) + "</svg>";
    EXPECT_THAT(TypeTextIntoBuiltDocument(svg, "SVG"),
                Eq(TextEditInvalidation{.siblingStyleRetained = true, .siblingFill = kRed}));
  }
}

TEST(SVGDocument, TypedTextReplacingRawTextChildrenKeepsIncrementalInvalidation) {
  const PaintServer kRed = PaintServer::Solid(css::Color(css::RGBA(0xFF, 0, 0, 0xFF)));
  constexpr std::string_view kSvg = R"(<svg xmlns="http://www.w3.org/2000/svg">
    <style>rect { fill: red }</style><rect id="r" width="10" height="10"/><text id="t">seed</text>
  </svg>)";
  // An incremental source edit retains the text's Data and CDATA children, which the typed setter
  // then discards; removing them must not count as a structural edit.
  const auto materializeRawText = [](SVGDocument& document) {
    const auto changed = ReplaceInSource(document, R"(<text id="t">seed</text>)",
                                         R"(<text id="t">word<![CDATA[other]]></text>)");
    ASSERT_TRUE(changed.applied);
    ASSERT_THAT(changed.diagnostic, Eq(std::nullopt));
    const std::optional<xml::XMLNode> text =
        xml::XMLNode::TryCast(document.querySelector("#t")->unsafeEntityHandle());
    ASSERT_THAT(text, Optional(testing::_));
    const std::optional<xml::XMLNode> firstChild = text->firstChild();
    EXPECT_THAT(firstChild ? std::optional(firstChild->type()) : std::nullopt,
                Optional(xml::XMLNode::Type::Data))
        << "the edit must retain raw text children";
  };
  EXPECT_THAT(TypeTextIntoBuiltDocument(kSvg, "SVG", materializeRawText),
              Eq(TextEditInvalidation{.siblingStyleRetained = true, .siblingFill = kRed}));
}

TEST(SVGDocument, TypedTextEmptinessChangeRestylesEmptySelectorDocuments) {
  const PaintServer kBlack = PaintServer::Solid(css::Color(css::RGBA(0, 0, 0, 0xFF)));
  const PaintServer kRed = PaintServer::Solid(css::Color(css::RGBA(0xFF, 0, 0, 0xFF)));
  for (const auto& [rule, fill] :
       {std::pair<std::string_view, PaintServer>{"text:empty + rect { fill: red }", kBlack},
        std::pair<std::string_view, PaintServer>{"svg:has(text:not(:empty)) rect { fill: red }",
                                                 kRed}}) {
    SCOPED_TRACE(rule);
    const std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg"><style>)" +
                            std::string(rule) +
                            R"(</style><text id="t"/><rect id="r" width="10" height="10"/></svg>)";
    EXPECT_THAT(TypeTextIntoBuiltDocument(svg, "SVG"),
                Eq(TextEditInvalidation{
                    .fullRebuild = true, .fullStyleRecompute = true, .siblingFill = fill}));
  }
}

TEST(SVGDocument, QuerySelectorSkipsRetainedRawTextAndComments) {
  auto document =
      ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><text id="t">seed</text></svg>)");
  auto changed = ReplaceInSource(document, "<text id=\"t\">seed</text>",
                                 "<text id=\"t\">word<![CDATA[other]]><!--c--></text>");
  ASSERT_THAT(changed.diagnostic, Eq(std::nullopt));
  auto text = document.querySelector("#t").value();
  ASSERT_THAT(QueriedId(text.querySelector("*")), Eq(std::nullopt));
  EXPECT_THAT(QueriedId(text.querySelector(":defined")), Eq(std::nullopt));
  changed = ReplaceInSource(document, "word<![CDATA[other]]><!--c-->",
                            "word<![CDATA[other]]><!--c--><tspan id=\"real\"/>");
  ASSERT_THAT(changed.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(text.querySelector("*")->id(), Eq("real"));
  EXPECT_THAT(text.querySelector(":defined")->id(), Eq("real"));
}

TEST(SVGDocument, ApplySourceEditProjectsTextMutation) {
  auto document =
      ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><text id="label">hello</text></svg>)");
  SVGElement text = document.querySelector("#label").value();
  const std::size_t valueOffset = document.source().find("hello");
  ASSERT_NE(valueOffset, std::string_view::npos);

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 5)},
      .replacement = "world",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  const auto& textComponent = text.entityHandle().get<components::TextComponent>();
  EXPECT_EQ(textComponent.text, "world");
  ASSERT_EQ(textComponent.textChunks.size(), 1u);
  EXPECT_EQ(textComponent.textChunks[0], "world");
}

TEST(SVGDocument, ApplySourceEditProjectsCDataTextMutation) {
  auto document = ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><text id="label"><![CDATA[hello]]></text></svg>)");
  SVGElement text = document.querySelector("#label").value();
  const std::size_t valueOffset = document.source().find("hello");
  ASSERT_NE(valueOffset, std::string_view::npos);

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 5)},
      .replacement = "world",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  const auto& textComponent = text.entityHandle().get<components::TextComponent>();
  EXPECT_EQ(textComponent.text, "world");
  EXPECT_THAT(textComponent.textChunks, testing::ElementsAre(RcString("world")));
}

TEST(SVGDocument, ApplySourceEditRejectsTextMutationOnNonTextElement) {
  auto document = ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><custom id="custom">hello</custom></svg>)");
  const std::size_t valueOffset = document.source().find("hello");
  ASSERT_NE(valueOffset, std::string_view::npos);

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 5)},
      .replacement = "world",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  ASSERT_TRUE(result.diagnostic.has_value());
  EXPECT_THAT(result.diagnostic->reason, testing::HasSubstr("not SVG text or style content"));
  EXPECT_THAT(document.source(), testing::HasSubstr(">world</custom>"));
}

TEST(SVGDocument, TextProjectionPreservesChunkBoundariesAroundChildElements) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <text id="label">one<tspan>two</tspan>three<![CDATA[four]]></text>
      <text id="leading-child"><tspan/>tail</text>
      <text id="adjacent-children"><tspan/>middle<tspan/>tail</text>
    </svg>
  )");

  SVGElement label = document.querySelector("#label").value();
  const auto& labelText = label.entityHandle().get<components::TextComponent>();
  EXPECT_EQ(labelText.text, "onethreefour");
  EXPECT_THAT(labelText.textChunks,
              testing::ElementsAre(RcString("one"), RcString("three"), RcString("four")));

  SVGElement leadingChild = document.querySelector("#leading-child").value();
  const auto& leadingText = leadingChild.entityHandle().get<components::TextComponent>();
  EXPECT_EQ(leadingText.text, "tail");
  EXPECT_THAT(leadingText.textChunks, testing::ElementsAre(RcString(""), RcString("tail")));

  SVGElement adjacentChildren = document.querySelector("#adjacent-children").value();
  const auto& adjacentText = adjacentChildren.entityHandle().get<components::TextComponent>();
  EXPECT_EQ(adjacentText.text, "middletail");
  EXPECT_THAT(adjacentText.textChunks,
              testing::ElementsAre(RcString(""), RcString("middle"), RcString("tail")));
}

TEST(SVGDocument, ApplySourceEditProjectsStyleMutationWithSourceMap) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style id="style">rect { fill: red; }</style>
      <rect/>
    </svg>
  )");
  SVGElement style = document.querySelector("#style").value();
  const std::size_t valueOffset = document.source().find("red");
  ASSERT_NE(valueOffset, std::string_view::npos);

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 3)},
      .replacement = "blue",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  const auto& stylesheet = style.entityHandle().get<components::StylesheetComponent>();
  EXPECT_EQ(stylesheet.stylesheet.rules().size(), 1u);
  const std::optional<xml::XMLNode> styleNode = xml::XMLNode::TryCast(style.entityHandle());
  ASSERT_TRUE(styleNode.has_value());
  const std::optional<SourceRange> textLocation = styleNode->getValueLocation();
  ASSERT_TRUE(textLocation.has_value());
  ASSERT_TRUE(styleNode->value().has_value());
  EXPECT_EQ(textLocation->end.offset.value() - textLocation->start.offset.value(),
            styleNode->value()->size());
  EXPECT_FALSE(stylesheet.sourceMap.empty());
  const std::optional<SVGStyleRuleAtSourceOffset> editedRule =
      FindStyleRuleAtSourceOffset(document, valueOffset);
  ASSERT_TRUE(editedRule.has_value());
  EXPECT_EQ(editedRule->stylesheetEntity, style.unsafeEntityHandle().entity());
}

TEST(SVGDocument, StyleProjectionCombinesDataAndCDataTextChildren) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style id="style">rect { fill: red; }<![CDATA[circle { fill: blue; }]]></style>
      <rect/>
      <circle/>
    </svg>
  )");
  SVGElement style = document.querySelector("#style").value();

  const auto& stylesheet = style.entityHandle().get<components::StylesheetComponent>();
  EXPECT_THAT(stylesheet.text, testing::HasSubstr("rect { fill: red; }"));
  EXPECT_THAT(stylesheet.text, testing::HasSubstr("circle { fill: blue; }"));
  EXPECT_EQ(stylesheet.stylesheet.rules().size(), 2u);
}

TEST(SVGDocument, StyleProjectionSkipsNonCssType) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style id="style" type="text/plain">rect { fill: red; }</style>
      <rect/>
    </svg>
  )");
  SVGElement style = document.querySelector("#style").value();

  const auto& stylesheet = style.entityHandle().get<components::StylesheetComponent>();
  EXPECT_FALSE(stylesheet.isCssType());
  EXPECT_TRUE(stylesheet.text.empty());
  EXPECT_TRUE(stylesheet.stylesheet.rules().empty());
}

TEST(SVGDocument, ApplySourceEditNonCssStyleMutationLeavesStylesheetEmpty) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style id="style" type="text/plain">rect { fill: red; }</style>
      <rect/>
    </svg>
  )");
  SVGElement style = document.querySelector("#style").value();
  const std::size_t valueOffset = document.source().find("red");
  ASSERT_NE(valueOffset, std::string_view::npos);

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 3)},
      .replacement = "blue",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, xml::ReparseScope::TextNode);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(document.source(), testing::HasSubstr("blue"));
  const auto& stylesheet = style.entityHandle().get<components::StylesheetComponent>();
  EXPECT_FALSE(stylesheet.isCssType());
  EXPECT_TRUE(stylesheet.text.empty());
  EXPECT_TRUE(stylesheet.stylesheet.rules().empty());
}

TEST(SVGDocument, StyleProjectionHandlesEmptyCssStyleElement) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style id="empty-style"></style>
      <rect/>
    </svg>
  )");
  SVGElement style = document.querySelector("#empty-style").value();

  EXPECT_EQ(style.entityHandle().try_get<components::StylesheetComponent>(), nullptr);
  EXPECT_EQ(style.cast<SVGStyleElement>().textContent(), "");
}

TEST(SVGDocument, StyleProjectionRejectsElementChildren) {
  parser::SVGParser::Options options;
  options.disableUserAttributes = false;

  ParseWarningSink disabled = ParseWarningSink::Disabled();
  auto result = parser::SVGParser::ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><style><rect/></style></svg>)", disabled, options);

  EXPECT_THAT(result, ParseErrorIs(testing::HasSubstr("Unexpected <style> element contents")));
}

TEST(SVGDocument, ApplySourceEditProjectsSubtreeReplacement) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="group"><rect id="old"/></g>
    </svg>
  )");
  const std::size_t editStart = document.source().find(R"(<rect id="old"/>)");
  const std::size_t editEnd = document.source().find("</g>");
  ASSERT_NE(editStart, std::string_view::npos);
  ASSERT_NE(editEnd, std::string_view::npos);

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart), FileOffset::Offset(editEnd)},
      .replacement = R"(<circle id="new"/>)",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(document.querySelector("#old"), Eq(std::nullopt));
  ASSERT_THAT(document.querySelector("#new"), Optional(ElementIdEq("new")));
  EXPECT_EQ(document.querySelector("#new")->type(), ElementType::Circle);
}

TEST(SVGDocument, ApplySourceEditProjectsSubtreeElementFamilies) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="group"><old/></g>
    </svg>
  )");
  const std::size_t editStart = document.source().find("<old/>");
  const std::size_t editEnd = document.source().find("</g>");
  ASSERT_NE(editStart, std::string_view::npos);
  ASSERT_NE(editEnd, std::string_view::npos);

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart), FileOffset::Offset(editEnd)},
      .replacement =
          R"(<a id="anchor"><tspan id="anchor-tspan">link</tspan></a><clipPath id="clip"><rect id="clip-rect"/></clipPath><defs id="defs"/><ellipse id="ellipse"/><filter id="filter"><feGaussianBlur id="blur"/></filter><line id="line"/><polygon id="polygon"/><polyline id="polyline"/>)",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, xml::ReparseScope::ElementSubtree);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));

  ASSERT_THAT(document.querySelector("#anchor"), Optional(ElementIdEq("anchor")));
  EXPECT_EQ(document.querySelector("#anchor")->type(), ElementType::A);
  EXPECT_TRUE(
      document.querySelector("#anchor")->entityHandle().all_of<components::TextComponent>());
  ASSERT_THAT(document.querySelector("#anchor-tspan"), Optional(ElementIdEq("anchor-tspan")));
  EXPECT_EQ(document.querySelector("#anchor-tspan")->type(), ElementType::TSpan);

  ASSERT_THAT(document.querySelector("#clip"), Optional(ElementIdEq("clip")));
  EXPECT_EQ(document.querySelector("#clip")->type(), ElementType::ClipPath);
  EXPECT_TRUE(
      document.querySelector("#clip")->entityHandle().all_of<components::ClipPathComponent>());
  const auto& clipBehavior =
      document.querySelector("#clip")->entityHandle().get<components::RenderingBehaviorComponent>();
  EXPECT_EQ(clipBehavior.behavior, components::RenderingBehavior::Nonrenderable);
  EXPECT_FALSE(clipBehavior.inheritsParentTransform);

  ASSERT_THAT(document.querySelector("#defs"), Optional(ElementIdEq("defs")));
  EXPECT_EQ(document.querySelector("#defs")->type(), ElementType::Defs);
  EXPECT_EQ(document.querySelector("#defs")
                ->entityHandle()
                .get<components::RenderingBehaviorComponent>()
                .behavior,
            components::RenderingBehavior::Nonrenderable);

  ASSERT_THAT(document.querySelector("#filter"), Optional(ElementIdEq("filter")));
  EXPECT_EQ(document.querySelector("#filter")->type(), ElementType::Filter);
  EXPECT_TRUE(
      document.querySelector("#filter")->entityHandle().all_of<components::FilterComponent>());
  EXPECT_EQ(document.querySelector("#filter")
                ->entityHandle()
                .get<components::RenderingBehaviorComponent>()
                .behavior,
            components::RenderingBehavior::Nonrenderable);

  ASSERT_THAT(document.querySelector("#blur"), Optional(ElementIdEq("blur")));
  EXPECT_EQ(document.querySelector("#blur")->type(), ElementType::FeGaussianBlur);
  EXPECT_TRUE(document.querySelector("#blur")
                  ->entityHandle()
                  .all_of<components::FilterPrimitiveComponent>());
  EXPECT_TRUE(document.querySelector("#blur")
                  ->entityHandle()
                  .all_of<components::FEGaussianBlurComponent>());
  EXPECT_EQ(document.querySelector("#blur")
                ->entityHandle()
                .get<components::RenderingBehaviorComponent>()
                .behavior,
            components::RenderingBehavior::Nonrenderable);

  EXPECT_EQ(document.querySelector("#ellipse")->type(), ElementType::Ellipse);
  EXPECT_EQ(document.querySelector("#line")->type(), ElementType::Line);
  EXPECT_EQ(document.querySelector("#polygon")->type(), ElementType::Polygon);
  EXPECT_EQ(document.querySelector("#polyline")->type(), ElementType::Polyline);
}

// Renaming an element's opening tag in source builds the element type the new name names, as the
// parser would, without a document reparse.
TEST(SVGDocument, ApplySourceEditOpeningTagRenameBuildsTypedElement) {
  auto document = ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><custom id="shape" width="4" height="2"/>)"
      R"(<rect id="sibling"/></svg>)");
  ASSERT_EQ(document.querySelector("#shape")->type(), ElementType::Unknown);
  const Entity sibling = document.querySelector("#sibling")->entityHandle().entity();
  const std::size_t openNameOffset = document.source().find("custom");
  ASSERT_NE(openNameOffset, std::string_view::npos);

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range =
          SourceRange{FileOffset::Offset(openNameOffset), FileOffset::Offset(openNameOffset + 6)},
      .replacement = "rect",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_EQ(result.scope, xml::ReparseScope::OpeningTag);
  ASSERT_THAT(document.querySelector("#shape"), Optional(ElementIdEq("shape")));
  ASSERT_EQ(document.querySelector("#shape")->type(), ElementType::Rect);
  EXPECT_EQ(document.querySelector("#shape")->cast<SVGRectElement>().width(), Lengthd(4));
  EXPECT_EQ(document.querySelector("#sibling")->entityHandle().entity(), sibling);
}

// The reported sequence: typing into `<ellipse` makes it an unknown element, and undoing the
// keystroke must bring the ellipse back.
TEST(SVGDocument, ApplySourceEditOpeningTagRenameRoundTripRestoresElementType) {
  auto document = ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><ellipse id="e" cx="10" cy="5" rx="4" ry="2"/>)"
      R"(</svg>)");
  ASSERT_EQ(document.querySelector("#e")->type(), ElementType::Ellipse);
  const std::size_t insertOffset = document.source().find("lipse");
  ASSERT_NE(insertOffset, std::string_view::npos);

  xml::ApplySourceEditResult typed = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(insertOffset), FileOffset::Offset(insertOffset)},
      .replacement = "X",
      .sourceVersion = document.sourceVersion(),
  });
  ASSERT_THAT(typed.diagnostic, Eq(std::nullopt));
  ASSERT_EQ(document.querySelector("#e")->type(), ElementType::Unknown);

  xml::ApplySourceEditResult undone = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(insertOffset), FileOffset::Offset(insertOffset + 1)},
      .replacement = "",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(undone.applied);
  EXPECT_THAT(undone.diagnostic, Eq(std::nullopt));
  ASSERT_THAT(document.querySelector("#e"), Optional(ElementIdEq("e")));
  ASSERT_EQ(document.querySelector("#e")->type(), ElementType::Ellipse);
  EXPECT_THAT(document.querySelector("#e")->cast<SVGEllipseElement>().rx(), Optional(Lengthd(4)));
}

// A namespace edit that moves elements into the SVG namespace builds them in place, as the parser
// would, at the edit's own incremental scope.
TEST(SVGDocument, NamespaceRebindBuildsTypedElementsIncrementally) {
  constexpr std::string_view kSource =
      "<svg xmlns='http://www.w3.org/2000/svg' xmlns:x='http://example.test/foreign'>"
      "<defs><rect id='swatch' width='8' height='12'/></defs>"
      "<x:image id='picture' width='8' height='12' href='picture.png'/>"
      "<x:use id='copy' href='#swatch'/>"
      "<x:linearGradient id='paint' x2='0.5'><x:stop id='stop' offset='1'/></x:linearGradient>"
      "</svg>";
  for (const bool throughSourceEdit : {false, true}) {
    SCOPED_TRACE(throughSourceEdit ? "applySourceEdit" : "setElementAttribute");
    auto document = ParseSVG(kSource);
    ASSERT_THAT(document.querySelector("#picture"), Optional(ElementIdEq("picture")));
    ASSERT_EQ(document.querySelector("#picture")->type(), ElementType::Unknown);
    ASSERT_THAT(document.querySelector("#swatch"), Optional(ElementIdEq("swatch")));
    const Entity picture = document.querySelector("#picture")->entityHandle().entity();
    const Entity swatch = document.querySelector("#swatch")->entityHandle().entity();

    const xml::ApplySourceEditResult result =
        throughSourceEdit
            ? ReplaceInSource(document, kForeignNamespaceUri, kSvgNamespaceUri)
            : document.setElementAttribute(
                  document.svgElement(), xml::XMLQualifiedNameRef("xmlns", "x"), kSvgNamespaceUri);

    EXPECT_TRUE(result.applied);
    EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
    EXPECT_EQ(result.scope, xml::ReparseScope::AttributeValue);
    ASSERT_EQ(document.querySelector("#picture")->type(), ElementType::Image);
    EXPECT_EQ(document.querySelector("#picture")->cast<SVGImageElement>().href(), "picture.png");
    ASSERT_THAT(document.querySelector("#copy"), Optional(ElementIdEq("copy")));
    ASSERT_EQ(document.querySelector("#copy")->type(), ElementType::Use);
    EXPECT_EQ(document.querySelector("#copy")->cast<SVGUseElement>().href(), "#swatch");
    ASSERT_THAT(document.querySelector("#paint"), Optional(ElementIdEq("paint")));
    ASSERT_EQ(document.querySelector("#paint")->type(), ElementType::LinearGradient);
    EXPECT_THAT(document.querySelector("#paint")->cast<SVGLinearGradientElement>().x2(),
                Optional(Lengthd(0.5)));
    ASSERT_THAT(document.querySelector("#stop"), Optional(ElementIdEq("stop")));
    EXPECT_EQ(document.querySelector("#stop")->type(), ElementType::Stop);
    EXPECT_EQ(document.querySelector("#picture")->entityHandle().entity(), picture);
    EXPECT_EQ(document.querySelector("#swatch")->entityHandle().entity(), swatch);
  }
}

TEST(SVGDocument, NamespaceRebindKeepsProjectedElementsIncremental) {
  auto document = ParseSVG(
      "<svg xmlns='http://www.w3.org/2000/svg' xmlns:x='http://example.test/foreign'>"
      "<x:rect id='shape' width='8' height='12'/></svg>");
  ASSERT_THAT(document.querySelector("#shape"), Optional(ElementIdEq("shape")));
  const Entity shape = document.querySelector("#shape")->entityHandle().entity();

  const xml::ApplySourceEditResult result =
      ReplaceInSource(document, kForeignNamespaceUri, kSvgNamespaceUri);

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_EQ(result.scope, xml::ReparseScope::AttributeValue);
  ASSERT_THAT(document.querySelector("#shape"), Optional(ElementIdEq("shape")));
  EXPECT_EQ(document.querySelector("#shape")->type(), ElementType::Rect);
  EXPECT_EQ(document.querySelector("#shape")->entityHandle().entity(), shape);
}

TEST(SVGDocument, SourceInsertedElementsAreBuiltIncrementally) {
  struct InsertionCase {
    std::string_view replacement;
    ElementType type;
  };
  for (const InsertionCase& insertion : {
           InsertionCase{"<rect id='new' width='4' height='4'/>", ElementType::Rect},
           InsertionCase{"<image id='new' width='4' height='4' href='picture.png'/>",
                         ElementType::Image},
           InsertionCase{"<use id='new' href='#swatch'/>", ElementType::Use},
           InsertionCase{"<linearGradient id='new'><stop offset='1'/></linearGradient>",
                         ElementType::LinearGradient},
           InsertionCase{"<radialGradient id='new'/>", ElementType::RadialGradient},
           InsertionCase{"<mask id='new'><rect width='4' height='4'/></mask>", ElementType::Mask},
           InsertionCase{"<marker id='new' markerWidth='4'/>", ElementType::Marker},
           InsertionCase{"<svg id='new' width='4' height='4'/>", ElementType::SVG},
       }) {
    SCOPED_TRACE(insertion.replacement);
    auto document = ParseSVG(
        R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="swatch"/><g id="host"><rect id="old"/><rect id="kept"/></g></svg>)");
    const Entity swatch = document.querySelector("#swatch")->entityHandle().entity();
    const Entity kept = document.querySelector("#kept")->entityHandle().entity();

    const xml::ApplySourceEditResult result =
        ReplaceInSource(document, R"(<rect id="old"/>)", insertion.replacement);

    EXPECT_TRUE(result.applied);
    EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
    EXPECT_EQ(result.scope, xml::ReparseScope::ElementSubtree);
    ASSERT_THAT(document.querySelector("#new"), Optional(ElementIdEq("new")));
    EXPECT_EQ(document.querySelector("#new")->type(), insertion.type);
    EXPECT_EQ(document.querySelector("#swatch")->entityHandle().entity(), swatch);
    EXPECT_EQ(document.querySelector("#kept")->entityHandle().entity(), kept);
  }
}

// A built `<title>`, `<desc>` or `<metadata>` element gets its text, as the parser gives it. A
// rebound element reads the text the parser kept as its node value.
TEST(SVGDocument, BuiltDescriptiveElementsKeepTheirText) {
  for (const DescriptiveCase& descriptive : kDescriptiveCases) {
    const std::string tag(descriptive.tag);
    SCOPED_TRACE(tag);

    auto inserted = ParseSVG(
        R"(<svg xmlns="http://www.w3.org/2000/svg"><g id="host"><rect id="old"/></g></svg>)");
    const xml::ApplySourceEditResult insertion = ReplaceInSource(
        inserted, R"(<rect id="old"/>)", "<" + tag + " id='new'>Hello</" + tag + ">");
    EXPECT_TRUE(insertion.applied);
    EXPECT_EQ(insertion.scope, xml::ReparseScope::ElementSubtree);
    EXPECT_THAT(insertion.diagnostic, Eq(std::nullopt));
    ASSERT_THAT(inserted.querySelector("#new"), Optional(ElementIdEq("new")));
    ASSERT_EQ(inserted.querySelector("#new")->type(), descriptive.type);
    EXPECT_EQ(DescriptiveText(*inserted.querySelector("#new")), "Hello");

    auto rebound = ParseSVG(
        "<svg xmlns='http://www.w3.org/2000/svg' xmlns:x='http://example.test/foreign'><x:" + tag +
        " id='new'>Hello</x:" + tag + "></svg>");
    const xml::ApplySourceEditResult rebind = rebound.setElementAttribute(
        rebound.svgElement(), xml::XMLQualifiedNameRef("xmlns", "x"), kSvgNamespaceUri);
    EXPECT_TRUE(rebind.applied);
    EXPECT_EQ(rebind.scope, xml::ReparseScope::AttributeValue);
    EXPECT_THAT(rebind.diagnostic, Eq(std::nullopt));
    ASSERT_THAT(rebound.querySelector("#new"), Optional(ElementIdEq("new")));
    ASSERT_EQ(rebound.querySelector("#new")->type(), descriptive.type);
    EXPECT_EQ(DescriptiveText(*rebound.querySelector("#new")), "Hello");
  }
}

// Editing the text of a `<title>`, `<desc>` or `<metadata>` element updates its text, whether the
// parser built the element, a source insertion did, or a namespace rebind promoted it.
TEST(SVGDocument, DescriptiveTextFollowsTextEdits) {
  for (const DescriptiveCase& descriptive : kDescriptiveCases) {
    const std::string tag(descriptive.tag);
    SCOPED_TRACE(tag);
    const auto expectEditedText = [&](SVGDocument& document) {
      const xml::ApplySourceEditResult edit = ReplaceInSource(document, "Hello", "World");
      EXPECT_TRUE(edit.applied);
      EXPECT_EQ(edit.scope, xml::ReparseScope::TextNode);
      EXPECT_THAT(edit.diagnostic, Eq(std::nullopt));
      ASSERT_THAT(document.querySelector("#t"), Optional(ElementIdEq("t")));
      ASSERT_EQ(document.querySelector("#t")->type(), descriptive.type);
      EXPECT_EQ(DescriptiveText(*document.querySelector("#t")), "World");
    };

    {
      SCOPED_TRACE("parsed");
      auto document = ParseSVG("<svg xmlns='http://www.w3.org/2000/svg'><" + tag +
                               " id='t'>Hello</" + tag + "></svg>");
      expectEditedText(document);
    }
    {
      SCOPED_TRACE("inserted");
      auto document =
          ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><g><rect id="old"/></g></svg>)");
      ASSERT_TRUE(ReplaceInSource(document, R"(<rect id="old"/>)",
                                  "<" + tag + " id='t'>Hello</" + tag + ">")
                      .applied);
      expectEditedText(document);
    }
    {
      SCOPED_TRACE("rebound");
      auto document = ParseSVG(
          "<svg xmlns='http://www.w3.org/2000/svg' xmlns:x='http://example.test/foreign'><x:" +
          tag + " id='t'>Hello</x:" + tag + "></svg>");
      ASSERT_TRUE(document
                      .setElementAttribute(document.svgElement(),
                                           xml::XMLQualifiedNameRef("xmlns", "x"), kSvgNamespaceUri)
                      .applied);
      expectEditedText(document);
    }
  }
}

// A subtree reparse that keeps a `<title>`, `<desc>` or `<metadata>` element, as the edit's target
// or as a descendant kept by its id, updates the element's text in place, as a fresh parse would.
TEST(SVGDocument, SubtreeReparseUpdatesKeptDescriptiveText) {
  struct SubtreeEdit {
    const char* label;
    const char* content;      //!< Element content before the edit.
    const char* needle;       //!< Source text to replace, with `TAG` for the tag name.
    const char* replacement;  //!< Replacement, with `TAG` for the tag name.
    const char* expected;     //!< Text a fresh parse of the edited source gives.
  };
  const SubtreeEdit kEdits[] = {
      {"kept by id", "Hello", "<TAG id='t'>Hello</TAG>", "<TAG id='t'>World</TAG><rect/>", "World"},
      {"kept by id, last chunk unchanged", "first<!--c-->last",
       "<TAG id='t'>first<!--c-->last</TAG>", "<TAG id='t'>new<!--c-->last</TAG><rect/>",
       "newlast"},
      {"CDATA text", "<![CDATA[Hello]]>", "Hello", "World", "World"},
      {"text before a comment", "first<!--c-->last", "first", "new", "newlast"},
      {"emptied CDATA", "<![CDATA[Hello]]>", "<![CDATA[Hello]]>", "", ""},
      {"kept by id, CDATA emptied", "<![CDATA[Hello]]>", "<TAG id='t'><![CDATA[Hello]]></TAG>",
       "<TAG id='t'></TAG><rect/>", ""},
  };
  const auto withTag = [](std::string text, const std::string& tag) {
    for (std::size_t at = text.find("TAG"); at != std::string::npos; at = text.find("TAG", at)) {
      text.replace(at, 3, tag);
    }
    return text;
  };

  for (const DescriptiveCase& descriptive : kDescriptiveCases) {
    const std::string tag(descriptive.tag);
    for (const SubtreeEdit& subtreeEdit : kEdits) {
      SCOPED_TRACE(tag + ": " + subtreeEdit.label);
      auto document = ParseSVG("<svg xmlns='http://www.w3.org/2000/svg'><g id='host'><" + tag +
                               " id='t'>" + subtreeEdit.content + "</" + tag + "></g></svg>");
      ASSERT_THAT(document.querySelector("#t"), Optional(ElementIdEq("t")));
      const Entity kept = document.querySelector("#t")->entityHandle().entity();

      const xml::ApplySourceEditResult edit = ReplaceInSource(
          document, withTag(subtreeEdit.needle, tag), withTag(subtreeEdit.replacement, tag));
      EXPECT_TRUE(edit.applied);
      EXPECT_EQ(edit.scope, xml::ReparseScope::ElementSubtree);
      EXPECT_THAT(edit.diagnostic, Eq(std::nullopt));
      ASSERT_THAT(document.querySelector("#t"), Optional(ElementIdEq("t")));
      EXPECT_EQ(document.querySelector("#t")->entityHandle().entity(), kept);
      ASSERT_EQ(document.querySelector("#t")->type(), descriptive.type);
      EXPECT_EQ(DescriptiveText(*document.querySelector("#t")), subtreeEdit.expected);

      auto fresh = ParseSVG(document.source());
      ASSERT_THAT(fresh.querySelector("#t"), Optional(ElementIdEq("t")));
      EXPECT_EQ(DescriptiveText(*fresh.querySelector("#t")), subtreeEdit.expected);
    }
  }
}

// A `<title>`, `<desc>` or `<metadata>` text edit that the content-projection budget rejects keeps
// the element's old text and reservation, as projected `<text>` does, and reports an error.
TEST(SVGDocument, RejectedDescriptiveTextEditKeepsTheOldText) {
  const auto retainedBytes = [](const SVGDocument& document) {
    return document.registry()
        .ctx()
        .get<components::ParsedPayloadResourceBudget>()
        .securityStats()
        .retainedBytes;
  };
  const auto expectKeptText = [&](SVGDocument& document, const xml::ApplySourceEditResult& edit,
                                  ElementType type, std::size_t bytesBefore) {
    EXPECT_TRUE(edit.applied);
    ASSERT_TRUE(edit.diagnostic.has_value());
    EXPECT_THAT(edit.diagnostic->reason, testing::HasSubstr("content-projection budget"));
    ASSERT_THAT(document.querySelector("#t"), Optional(ElementIdEq("t")));
    ASSERT_EQ(document.querySelector("#t")->type(), type);
    EXPECT_EQ(DescriptiveText(*document.querySelector("#t")), "Hello");
    EXPECT_EQ(retainedBytes(document), bytesBefore);
  };

  for (const DescriptiveCase& descriptive : kDescriptiveCases) {
    const std::string tag(descriptive.tag);
    {
      SCOPED_TRACE(tag + ": payload budget");
      const std::string source =
          "<svg xmlns='http://www.w3.org/2000/svg'><" + tag + " id='t'>Hello</" + tag + "></svg>";
      parser::SVGParser::Options options;
      options.maximumParsedPayloadSize = retainedBytes(ParseSVG(source));
      auto document = ParseSVG(source, options);
      const std::size_t bytesBefore = retainedBytes(document);
      ASSERT_EQ(bytesBefore, options.maximumParsedPayloadSize);

      expectKeptText(document, ReplaceInSource(document, "Hello", "Hello, world"), descriptive.type,
                     bytesBefore);
    }
    {
      SCOPED_TRACE(tag + ": chunk limit");
      parser::SVGParser::Options options;
      options.maximumContentProjectionChunks = 1;
      auto document = ParseSVG("<svg xmlns='http://www.w3.org/2000/svg'><g id='host'><" + tag +
                                   " id='t'>Hello</" + tag + "></g></svg>",
                               options);
      const std::size_t bytesBefore = retainedBytes(document);

      expectKeptText(document,
                     ReplaceInSource(document, "<" + tag + " id='t'>Hello</" + tag + ">",
                                     "<" + tag + " id='t'>Hello<![CDATA[, world]]></" + tag + ">"),
                     descriptive.type, bytesBefore);
    }
  }
}

// A `<title>`, `<desc>` or `<metadata>` element that a source edit builds reports an error when the
// content-projection budget refuses its text, as an inserted `<text>` does, and stays empty, as a
// fresh parse leaves it.
TEST(SVGDocument, RejectedBuiltDescriptiveTextReportsAnError) {
  for (const DescriptiveCase& descriptive : kDescriptiveCases) {
    const std::string tag(descriptive.tag);
    SCOPED_TRACE(tag);
    parser::SVGParser::Options options;
    options.maximumContentProjectionChunks = 1;
    auto document = ParseSVG(
        R"(<svg xmlns="http://www.w3.org/2000/svg"><g><rect id="old"/></g></svg>)", options);

    const xml::ApplySourceEditResult insertion =
        ReplaceInSource(document, R"(<rect id="old"/>)",
                        "<" + tag + " id='t'>Hello<![CDATA[, world]]></" + tag + ">");
    EXPECT_TRUE(insertion.applied);
    ASSERT_TRUE(insertion.diagnostic.has_value());
    EXPECT_THAT(insertion.diagnostic->reason, testing::HasSubstr("content-projection budget"));
    ASSERT_THAT(document.querySelector("#t"), Optional(ElementIdEq("t")));
    ASSERT_EQ(document.querySelector("#t")->type(), descriptive.type);
    EXPECT_EQ(DescriptiveText(*document.querySelector("#t")), "");
  }
}

TEST(SVGDocument, SourceInsertedAnimationIsEnabledWithEitherParseOption) {
  for (const bool experimental : {false, true}) {
    SCOPED_TRACE(experimental ? "experimental enabled" : "experimental disabled");
    parser::SVGParser::Options options;
    options.enableExperimental = experimental;
    auto document = ParseSVG(
        R"(<svg xmlns="http://www.w3.org/2000/svg"><g id="host"><rect id="old"/></g></svg>)",
        options);

    const xml::ApplySourceEditResult result = ReplaceInSource(
        document, R"(<rect id="old"/>)", "<set id='new' attributeName='opacity' to='0.5'/>");

    EXPECT_TRUE(result.applied);
    EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
    EXPECT_EQ(result.scope, xml::ReparseScope::ElementSubtree);
    ASSERT_THAT(document.querySelector("#new"), Optional(ElementIdEq("new")));
    EXPECT_EQ(document.querySelector("#new")->type(), ElementType::Set);
  }
}

// Each entry point that can move an element into the SVG namespace builds it in place.
TEST(SVGDocument, NamespaceMembershipEntryPointsBuildTypedElements) {
  constexpr std::string_view kDefaultNamespaceSource =
      "<svg xmlns='http://www.w3.org/2000/svg'><g id='host' xmlns='http://example.test/foreign'>"
      "<image id='target' width='4' height='4' href='picture.png'/></g></svg>";
  struct EntryPointCase {
    std::string_view name;
    std::string_view source;
    std::function<xml::ApplySourceEditResult(SVGDocument&)> edit;
    xml::ReparseScope scope;
  };
  const std::vector<EntryPointCase> cases = {
      {"removeElementAttribute of a default xmlns", kDefaultNamespaceSource,
       [](SVGDocument& document) {
         return document.removeElementAttribute(*document.querySelector("#host"),
                                                xml::XMLQualifiedNameRef("xmlns"));
       },
       xml::ReparseScope::OpeningTag},
      {"applySourceEdit of a default xmlns", kDefaultNamespaceSource,
       [](SVGDocument& document) {
         return ReplaceInSource(document, kForeignNamespaceUri, kSvgNamespaceUri);
       },
       xml::ReparseScope::AttributeValue},
      {"insertElement out of a foreign default namespace", kDefaultNamespaceSource,
       [](SVGDocument& document) {
         return document.insertElement(document.svgElement(), *document.querySelector("#target"));
       },
       xml::ReparseScope::ElementSubtree},
      {"setElementAttribute rebinding inside a foreign subtree",
       "<svg xmlns='http://www.w3.org/2000/svg' xmlns:x='http://example.test/foreign' "
       "xmlns:y='http://example.test/other'><x:wrapper id='host'><y:image id='target' width='4' "
       "height='4' href='picture.png'/></x:wrapper></svg>",
       [](SVGDocument& document) {
         return document.setElementAttribute(
             document.svgElement(), xml::XMLQualifiedNameRef("xmlns", "y"), kSvgNamespaceUri);
       },
       xml::ReparseScope::AttributeValue},
  };
  for (const EntryPointCase& entryPoint : cases) {
    SCOPED_TRACE(entryPoint.name);
    auto document = ParseSVG(entryPoint.source);
    ASSERT_THAT(document.querySelector("#target"), Optional(ElementIdEq("target")));
    ASSERT_EQ(document.querySelector("#target")->type(), ElementType::Unknown);
    const Entity target = document.querySelector("#target")->entityHandle().entity();

    const xml::ApplySourceEditResult result = entryPoint.edit(document);

    EXPECT_TRUE(result.applied);
    EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
    EXPECT_EQ(result.scope, entryPoint.scope);
    ASSERT_THAT(document.querySelector("#target"), Optional(ElementIdEq("target")));
    ASSERT_EQ(document.querySelector("#target")->type(), ElementType::Image);
    EXPECT_EQ(document.querySelector("#target")->cast<SVGImageElement>().href(), "picture.png");
    EXPECT_EQ(document.querySelector("#target")->entityHandle().entity(), target);
  }
}

// The projection builds every element type the parser builds, for a source insertion and for a
// namespace rebind, so adding an element type cannot leave the projection falling back to an
// unknown element.
TEST(SVGDocument, ProjectionBuildsEveryParserElementType) {
  for (const bool experimental : {false, true}) {
    parser::SVGParser::Options options;
    options.enableExperimental = experimental;
    [&]<typename... Types>(entt::type_list<Types...>) {
      (
          [&]<typename ElementT>() {
            const std::string tag(ElementT::Tag);
            SCOPED_TRACE(tag + (experimental ? " (experimental enabled)" : ""));
            constexpr bool kExperimentalType = [] {
              if constexpr (requires { ElementT::IsExperimental; }) {
                return ElementT::IsExperimental;
              } else {
                return false;
              }
            }();
            const ElementType expected =
                experimental || !kExperimentalType ? ElementT::Type : ElementType::Unknown;

            auto inserted = ParseSVG(
                R"(<svg xmlns="http://www.w3.org/2000/svg"><g id="host"><rect id="old"/></g></svg>)",
                options);
            const xml::ApplySourceEditResult insertion =
                ReplaceInSource(inserted, R"(<rect id="old"/>)", "<" + tag + " id='new'/>");
            EXPECT_TRUE(insertion.applied);
            EXPECT_THAT(insertion.diagnostic, Eq(std::nullopt));
            EXPECT_EQ(insertion.scope, xml::ReparseScope::ElementSubtree);
            ASSERT_THAT(inserted.querySelector("#new"), Optional(ElementIdEq("new")));
            EXPECT_EQ(inserted.querySelector("#new")->type(), expected);
            // Found by position: the parser does not apply `id` to every type, such as feFuncA.
            auto reparsed = ParseSVG(inserted.source(), options);
            ASSERT_THAT(reparsed.querySelector("#host"), Optional(ElementIdEq("host")));
            const std::optional<SVGElement> freshElement =
                reparsed.querySelector("#host")->firstChild();
            ASSERT_THAT(freshElement, testing::Ne(std::nullopt));
            EXPECT_EQ(freshElement->type(), expected);

            auto rebound = ParseSVG(
                "<svg xmlns='http://www.w3.org/2000/svg' "
                "xmlns:x='http://example.test/foreign'><x:" +
                    tag + " id='new'/></svg>",
                options);
            const xml::ApplySourceEditResult rebind = rebound.setElementAttribute(
                rebound.svgElement(), xml::XMLQualifiedNameRef("xmlns", "x"), kSvgNamespaceUri);
            EXPECT_TRUE(rebind.applied);
            EXPECT_THAT(rebind.diagnostic, Eq(std::nullopt));
            EXPECT_EQ(rebind.scope, xml::ReparseScope::AttributeValue);
            ASSERT_THAT(rebound.querySelector("#new"), Optional(ElementIdEq("new")));
            EXPECT_EQ(rebound.querySelector("#new")->type(), expected);
          }.template operator()<Types>(),
          ...);
    }(AllSVGElements{});
  }
}

TEST(SVGDocument, SourceBackedElementAttributeHelpersUpdateSourceAndProjection) {
  auto document =
      ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" fill="red"/></svg>)");
  SVGElement rect = document.querySelector("#r").value();

  xml::ApplySourceEditResult setResult = document.setElementAttribute(rect, "fill", "green");
  EXPECT_TRUE(setResult.applied);
  EXPECT_THAT(rect.getAttribute("fill"), Optional(Eq("green")));
  EXPECT_THAT(document.source(), testing::HasSubstr(R"(fill="green")"));

  xml::ApplySourceEditResult removeResult = document.removeElementAttribute(rect, "fill");
  EXPECT_TRUE(removeResult.applied);
  EXPECT_FALSE(rect.hasAttribute("fill"));
  EXPECT_THAT(document.source(), testing::Not(testing::HasSubstr("fill=")));
}

TEST(SVGDocument, SourceBackedInlineSizeAttributeRoundTrips) {
  // SVG2 inline-size round-trips through source writeback: it is parsed as an attribute, readable
  // via getAttribute, updatable via setElementAttribute (patching the source), and removable.
  auto document = ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><text id="t" inline-size="150">Hello world</text></svg>)");
  SVGElement text = document.querySelector("#t").value();

  EXPECT_THAT(text.getAttribute("inline-size"), Optional(Eq("150")));

  xml::ApplySourceEditResult setResult = document.setElementAttribute(text, "inline-size", "200");
  EXPECT_TRUE(setResult.applied);
  EXPECT_THAT(text.getAttribute("inline-size"), Optional(Eq("200")));
  EXPECT_THAT(document.source(), testing::HasSubstr(R"(inline-size="200")"));

  xml::ApplySourceEditResult removeResult = document.removeElementAttribute(text, "inline-size");
  EXPECT_TRUE(removeResult.applied);
  EXPECT_FALSE(text.hasAttribute("inline-size"));
  EXPECT_THAT(document.source(), testing::Not(testing::HasSubstr("inline-size=")));
}

TEST(SVGDocument, SourceBackedInvalidAttributeSetReportsProjectionDiagnostic) {
  auto document =
      ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" width="1"/></svg>)");
  SVGElement rect = document.querySelector("#r").value();

  xml::ApplySourceEditResult result = document.setElementAttribute(rect, "width", "not-a-length");

  EXPECT_TRUE(result.applied);
  ASSERT_TRUE(result.diagnostic.has_value());
  EXPECT_THAT(result.diagnostic->reason, testing::HasSubstr("Invalid length or percentage"));
  EXPECT_THAT(document.source(), testing::HasSubstr(R"(width="not-a-length")"));
}

TEST(SVGDocument, ProgrammaticElementAttributeHelpersUseDomFallback) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);
  document.svgElement().appendChild(rect);

  xml::ApplySourceEditResult setResult = document.setElementAttribute(rect, "fill", "green");
  EXPECT_FALSE(setResult.applied);
  EXPECT_THAT(setResult.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(rect.getAttribute("fill"), Optional(Eq("green")));

  xml::ApplySourceEditResult removeResult = document.removeElementAttribute(rect, "fill");
  EXPECT_FALSE(removeResult.applied);
  EXPECT_THAT(removeResult.diagnostic, Eq(std::nullopt));
  EXPECT_FALSE(rect.hasAttribute("fill"));
}

TEST(SVGDocument, SourceBackedDetachedElementAttributeHelpersUseDomFallback) {
  auto document = ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"/>)");
  SVGRectElement detached = SVGRectElement::Create(document);

  xml::ApplySourceEditResult setResult = document.setElementAttribute(detached, "fill", "green");
  EXPECT_FALSE(setResult.applied);
  EXPECT_THAT(setResult.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(detached.getAttribute("fill"), Optional(Eq("green")));
  EXPECT_THAT(document.source(), testing::Not(testing::HasSubstr("green")));

  xml::ApplySourceEditResult removeResult = document.removeElementAttribute(detached, "fill");
  EXPECT_FALSE(removeResult.applied);
  EXPECT_THAT(removeResult.diagnostic, Eq(std::nullopt));
  EXPECT_FALSE(detached.hasAttribute("fill"));
}

TEST(SVGDocument, ProgrammaticInvalidAttributeSetReportsDiagnostic) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);
  document.svgElement().appendChild(rect);

  xml::ApplySourceEditResult result = document.setElementAttribute(rect, "width", "not-a-length");

  EXPECT_FALSE(result.applied);
  ASSERT_TRUE(result.diagnostic.has_value());
  EXPECT_THAT(result.diagnostic->reason, testing::HasSubstr("Invalid length or percentage"));
}

TEST(SVGDocument, SourceBackedSetElementTextContentUpdatesSourceAndProjection) {
  auto document = ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><text id="label"/></svg>)");
  SVGElement text = document.querySelector("#label").value();

  xml::ApplySourceEditResult result = document.setElementTextContent(text, "hello & goodbye");

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, xml::ReparseScope::TextNode);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(document.source(), testing::HasSubstr("hello &amp; goodbye"));
  const auto& textComponent = text.entityHandle().get<components::TextComponent>();
  EXPECT_EQ(textComponent.text, "hello & goodbye");
  EXPECT_THAT(textComponent.textChunks, testing::ElementsAre(RcString("hello & goodbye")));
}

TEST(SVGDocument, SourceBackedSetElementTextContentEmptyRemovesExistingTextNode) {
  auto document =
      ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><text id="label">hello</text></svg>)");
  SVGElement text = document.querySelector("#label").value();

  xml::ApplySourceEditResult result = document.setElementTextContent(text, "");

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(document.source(), testing::HasSubstr(R"(<text id="label"></text>)"));
  const auto& textComponent = text.entityHandle().get<components::TextComponent>();
  EXPECT_EQ(textComponent.text, "");
  EXPECT_THAT(textComponent.textChunks, testing::ElementsAre(RcString("")));
}

TEST(SVGDocument, SourceBackedSetElementTextContentWithElementChildrenStaysComponentOnly) {
  auto document = ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><text id="label"><tspan>child</tspan></text></svg>)");
  SVGElement text = document.querySelector("#label").value();

  xml::ApplySourceEditResult result = document.setElementTextContent(text, "replacement");

  EXPECT_FALSE(result.applied);
  EXPECT_EQ(result.scope, xml::ReparseScope::TextNode);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(document.source(), testing::HasSubstr("<tspan>child</tspan>"));
  EXPECT_EQ(text.entityHandle().get<components::TextComponent>().text, "");
}

TEST(SVGDocument, SourceBackedSetElementTextContentRejectsElementWithoutXmlIdentity) {
  auto document = ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"/>)");
  SVGRectElement detached = SVGRectElement::Create(document);

  xml::ApplySourceEditResult result = document.setElementTextContent(detached, "detached");

  EXPECT_FALSE(result.applied);
  EXPECT_EQ(result.scope, xml::ReparseScope::TextNode);
  ASSERT_TRUE(result.diagnostic.has_value());
  EXPECT_THAT(result.diagnostic->reason, testing::HasSubstr("without XML source identity"));
}

TEST(SVGDocument, SourceLessSetElementTextContentReturnsUnappliedResult) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);
  document.svgElement().appendChild(rect);

  xml::ApplySourceEditResult result = document.setElementTextContent(rect, "source-less");

  EXPECT_FALSE(result.applied);
  EXPECT_EQ(result.scope, xml::ReparseScope::TextNode);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
}

TEST(SVGDocument, SetElementTextContentOnNonTextElementReportsDiagnostic) {
  auto document =
      ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><custom id="c">old</custom></svg>)");
  SVGElement custom = document.querySelector("#c").value();

  xml::ApplySourceEditResult result = document.setElementTextContent(custom, "new");

  ASSERT_TRUE(result.diagnostic.has_value());
  EXPECT_THAT(result.diagnostic->reason, testing::HasSubstr("not SVG text or style content"));
  EXPECT_THAT(document.source(), testing::HasSubstr(">new</custom>"));
}

TEST(SVGDocument, MixedContentTextNodeEditReprojectsTextChunks) {
  // Editing a text node inside an element with mixed content (text plus child elements) reprojects
  // the parent element's text, preserving chunk boundaries around the child element.
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <text id="m">one<tspan id="s">two</tspan>three</text>
    </svg>
  )");
  SVGElement text = document.querySelector("#m").value();
  const std::size_t valueOffset = document.source().find("one");
  ASSERT_NE(valueOffset, std::string_view::npos);

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 3)},
      .replacement = "ONE",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(document.source(), testing::HasSubstr(">ONE<tspan"));
  const auto& textComponent = text.entityHandle().get<components::TextComponent>();
  EXPECT_EQ(textComponent.text, "ONEthree");
  EXPECT_THAT(textComponent.textChunks, testing::ElementsAre(RcString("ONE"), RcString("three")));
}

TEST(SVGDocument, TSpanTextEditInvalidatesAncestorTextRoot) {
  // Editing text inside a <tspan> marks the ancestor <text> root's text geometry dirty, walking
  // up the tree from the tspan (which is not itself a text root).
  auto document = ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><text id="m"><tspan id="s">two</tspan></text></svg>)");
  SVGElement tspan = document.querySelector("#s").value();
  SVGElement text = document.querySelector("#m").value();
  text.entityHandle().remove<components::DirtyFlagsComponent>();
  const std::size_t valueOffset = document.source().find("two");
  ASSERT_NE(valueOffset, std::string_view::npos);

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 3)},
      .replacement = "TWO",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_EQ(tspan.entityHandle().get<components::TextComponent>().text, "TWO");

  const auto* dirtyFlags = text.entityHandle().try_get<components::DirtyFlagsComponent>();
  ASSERT_NE(dirtyFlags, nullptr);
  EXPECT_TRUE(dirtyFlags->test(components::DirtyFlagsComponent::TextGeometry));
}

TEST(SVGDocument, GlyphOrientationMutationInvalidatesAncestorTextGeometry) {
  auto document = ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><text id="m"
    writing-mode="vertical-rl"><tspan id="s">A</tspan></text></svg>)");
  SVGElement tspan = document.querySelector("#s").value();
  SVGElement text = document.querySelector("#m").value();
  text.entityHandle().remove<components::DirtyFlagsComponent>();

  tspan.setAttribute("glyph-orientation-vertical", "0");

  const auto* dirtyFlags = text.entityHandle().try_get<components::DirtyFlagsComponent>();
  ASSERT_NE(dirtyFlags, nullptr);
  EXPECT_TRUE(dirtyFlags->test(components::DirtyFlagsComponent::TextGeometry));
}

TEST(SVGDocument, StandaloneAnchorTextEditProjectsWithoutTextRoot) {
  // An <a> outside of text content carries text components but has no <text> root ancestor; the
  // invalidation walk terminates at the document root without finding one.
  auto document = ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><a id="a1">hi</a></svg>)");
  SVGElement anchor = document.querySelector("#a1").value();
  const std::size_t valueOffset = document.source().find("hi");
  ASSERT_NE(valueOffset, std::string_view::npos);

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 2)},
      .replacement = "bye",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_EQ(anchor.entityHandle().get<components::TextComponent>().text, "bye");
}

TEST(SVGDocument, CDataMixedContentEditReprojectsTextChunks) {
  // Editing CDATA contents adjacent to a plain text sibling reprojects the parent text element,
  // concatenating the data and CDATA chunks.
  auto document = ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><text id="m">one<![CDATA[two]]></text></svg>)");
  SVGElement text = document.querySelector("#m").value();
  const std::size_t valueOffset = document.source().find("two");
  ASSERT_NE(valueOffset, std::string_view::npos);

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 3)},
      .replacement = "TWO",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(document.source(), testing::HasSubstr("<![CDATA[TWO]]>"));
  const auto& textComponent = text.entityHandle().get<components::TextComponent>();
  EXPECT_EQ(textComponent.text, "oneTWO");
  EXPECT_THAT(textComponent.textChunks, testing::ElementsAre(RcString("one"), RcString("TWO")));
}

TEST(SVGDocument, ProcessingInstructionValueEditAppliesWithoutProjectionDiagnostic) {
  // Editing processing instruction contents applies to the source without producing a projection
  // diagnostic, and does not disturb sibling elements.
  auto document =
      ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><?foo bar?><rect id="r"/></svg>)");
  const std::size_t valueOffset = document.source().find("bar");
  ASSERT_NE(valueOffset, std::string_view::npos);

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 3)},
      .replacement = "baz",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(document.source(), testing::HasSubstr("<?foo baz?>"));
  EXPECT_THAT(document.querySelector("#r"), Optional(ElementIdEq("r")));
}

TEST(SVGDocument, CommentValueEditAppliesWithoutProjectionDiagnostic) {
  // Editing comment contents applies to the source without producing a projection diagnostic,
  // and does not disturb sibling elements.
  auto document =
      ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><!-- note --><rect id="r"/></svg>)");
  const std::size_t valueOffset = document.source().find("note");
  ASSERT_NE(valueOffset, std::string_view::npos);

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 4)},
      .replacement = "updated",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(document.source(), testing::HasSubstr("<!-- updated -->"));
  EXPECT_THAT(document.querySelector("#r"), Optional(ElementIdEq("r")));
}

TEST(SVGDocument, StyleTextEditWithElementChildReportsDiagnostic) {
  // Insert an element child into a <style> element, then edit the style text: reprojecting the
  // stylesheet encounters the unexpected element child.
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style id="s">rect { fill: red; }</style>
      <rect id="r"/>
    </svg>
  )");
  SVGElement style = document.querySelector("#s").value();
  SVGRectElement inserted = SVGRectElement::Create(document);
  inserted.setId("inserted");
  ASSERT_TRUE(document.insertElement(style, inserted).applied);

  const std::size_t valueOffset = document.source().find("red");
  ASSERT_NE(valueOffset, std::string_view::npos);

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 3)},
      .replacement = "blue",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  ASSERT_TRUE(result.diagnostic.has_value());
  EXPECT_THAT(result.diagnostic->reason, testing::HasSubstr("Unexpected <style> element contents"));
}

TEST(SVGDocument, SubtreeEditWithInvalidAttributeReportsProjectionDiagnostic) {
  // An edit spanning multiple children reparses the parent subtree; projecting the replacement
  // reports the invalid attribute on the new child.
  auto document = ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><g id="host"><rect id="a" width="5"/><rect id="b"/></g></svg>)");
  const std::size_t editStart = document.source().find(R"(<rect id="a")");
  const std::string_view lastChild = R"(<rect id="b"/>)";
  const std::size_t lastChildOffset = document.source().find(lastChild);
  ASSERT_NE(editStart, std::string_view::npos);
  ASSERT_NE(lastChildOffset, std::string_view::npos);
  const std::size_t editEnd = lastChildOffset + lastChild.size();

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart), FileOffset::Offset(editEnd)},
      .replacement = R"(<rect id="c" width="not-a-length"/>)",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, xml::ReparseScope::ElementSubtree);
  ASSERT_TRUE(result.diagnostic.has_value());
  EXPECT_THAT(result.diagnostic->reason, testing::HasSubstr("Invalid length or percentage"));
  ASSERT_THAT(document.querySelector("#c"), Optional(ElementIdEq("c")));
  EXPECT_THAT(document.querySelector("#a"), Eq(std::nullopt));
  EXPECT_THAT(document.querySelector("#b"), Eq(std::nullopt));
}

TEST(SVGDocument, SubtreeEditProjectsMixedTextChunks) {
  // Text content inserted through a source edit is projected by SVGDocument's subtree projection,
  // preserving chunk boundaries around child elements and concatenating adjacent text and CDATA.
  auto document = ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><g id="host"><rect id="old"/></g></svg>)");
  const std::string_view oldChild = R"(<rect id="old"/>)";
  const std::size_t editStart = document.source().find(oldChild);
  ASSERT_NE(editStart, std::string_view::npos);

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart),
                           FileOffset::Offset(editStart + oldChild.size())},
      .replacement =
          R"(<text id="t2"><tspan>lead</tspan>mid<tspan>x</tspan>tail<![CDATA[cd]]></text><text id="t3">solo</text>)",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));

  SVGElement mixed = document.querySelector("#t2").value();
  const auto& mixedText = mixed.entityHandle().get<components::TextComponent>();
  EXPECT_EQ(mixedText.text, "midtailcd");
  EXPECT_THAT(mixedText.textChunks, testing::ElementsAre(RcString(""), RcString("mid"),
                                                         RcString("tail"), RcString("cd")));

  SVGElement solo = document.querySelector("#t3").value();
  const auto& soloText = solo.entityHandle().get<components::TextComponent>();
  EXPECT_EQ(soloText.text, "solo");
  EXPECT_THAT(soloText.textChunks, testing::ElementsAre(RcString("solo")));
}

TEST(SVGDocument, RejectedTextProjectionPreservesLastValidText) {
  parser::SVGParser::Options options;
  options.maximumContentProjectionChunks = 3;
  auto document = ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><text id="label">stable<tspan id="a"/><tspan id="b"/></text></svg>)",
      options);
  const std::size_t editStart = document.source().find("stable");
  const std::size_t lastChild = document.source().find(R"(<tspan id="b"/>)");
  ASSERT_NE(editStart, std::string_view::npos);
  ASSERT_NE(lastChild, std::string_view::npos);
  const std::size_t editEnd = lastChild + std::string_view(R"(<tspan id="b"/>)").size();

  std::string oversizedReplacement = "new";
  for (std::size_t i = 0; i <= options.maximumContentProjectionChunks; ++i) {
    oversizedReplacement += "<tspan/>";
  }

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart), FileOffset::Offset(editEnd)},
      .replacement = oversizedReplacement,
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, xml::ReparseScope::ElementSubtree);
  ASSERT_TRUE(result.diagnostic.has_value());
  EXPECT_THAT(result.diagnostic->reason, testing::HasSubstr("too many chunks"));
  SVGElement text = document.querySelector("#label").value();
  const auto& textComponent = text.entityHandle().get<components::TextComponent>();
  EXPECT_EQ(textComponent.text, "stable");
  EXPECT_THAT(textComponent.textChunks,
              testing::ElementsAre(RcString("stable"), RcString(""), RcString("")));
}

TEST(SVGDocument, IncrementalAttributePayloadReplacementEnforcesCapPlusOne) {
  parser::SVGParser::Options options;
  options.maximumParsedPayloadSize = 4608;
  auto document =
      ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" x="0"/></svg>)", options);

  const std::string acceptedValue(31, '1');
  const std::size_t attributeOffset = document.source().find(R"(x="0")");
  ASSERT_NE(attributeOffset, std::string_view::npos);
  std::size_t valueOffset = attributeOffset + 3;
  xml::ApplySourceEditResult accepted = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset), FileOffset::Offset(valueOffset + 1)},
      .replacement = acceptedValue,
      .sourceVersion = document.sourceVersion(),
  });
  ASSERT_TRUE(accepted.applied);
  ASSERT_THAT(accepted.diagnostic, Eq(std::nullopt));

  const auto* budget = document.registry().ctx().find<components::ParsedPayloadResourceBudget>();
  ASSERT_NE(budget, nullptr);
  EXPECT_EQ(budget->securityStats().attributeBytes, options.maximumParsedPayloadSize);

  valueOffset = document.source().find(acceptedValue);
  ASSERT_NE(valueOffset, std::string_view::npos);
  const std::string rejectedValue(32, '2');
  xml::ApplySourceEditResult rejected = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(valueOffset),
                           FileOffset::Offset(valueOffset + acceptedValue.size())},
      .replacement = rejectedValue,
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(rejected.applied);
  ASSERT_TRUE(rejected.diagnostic.has_value());
  EXPECT_THAT(rejected.diagnostic->reason, testing::HasSubstr("parsed-payload budget"));
  EXPECT_EQ(budget->securityStats().attributeBytes, options.maximumParsedPayloadSize);
  EXPECT_TRUE(budget->securityStats().rejected);
}

TEST(SVGDocument, AttributePayloadCountsQualifiedNameBytesAtBoundary) {
  parser::SVGParser::Options options;
  options.maximumParsedPayloadSize = 4224;
  const auto makeSource = [](std::size_t nameBytes) {
    return std::string(R"(<svg xmlns="http://www.w3.org/2000/svg"><rect )") +
           std::string(nameBytes, 'a') + R"(=""/></svg>)";
  };

  SVGDocument accepted = ParseSVG(makeSource(31), options);
  const auto* acceptedBudget =
      accepted.registry().ctx().find<components::ParsedPayloadResourceBudget>();
  ASSERT_NE(acceptedBudget, nullptr);
  EXPECT_FALSE(acceptedBudget->securityStats().rejected);
  EXPECT_EQ(acceptedBudget->securityStats().attributeBytes, options.maximumParsedPayloadSize);

  SVGDocument rejected = ParseSVG(makeSource(32), options);
  const auto* rejectedBudget =
      rejected.registry().ctx().find<components::ParsedPayloadResourceBudget>();
  ASSERT_NE(rejectedBudget, nullptr);
  EXPECT_TRUE(rejectedBudget->securityStats().rejected);
  EXPECT_EQ(rejectedBudget->securityStats().attributeBytes, 2112u);
}

TEST(SVGDocument, IncrementalTextPayloadReplacementReleasesAfterDetachedHandle) {
  parser::SVGParser::Options options;
  options.maximumParsedPayloadSize = 4608;
  auto document =
      ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><text id="t"></text></svg>)", options);
  std::optional<SVGElement> text = document.querySelector("#t");
  ASSERT_TRUE(text.has_value());

  const std::string acceptedText(2112, ' ');
  xml::ApplySourceEditResult accepted = document.setElementTextContent(*text, acceptedText);
  ASSERT_TRUE(accepted.applied);
  ASSERT_THAT(accepted.diagnostic, Eq(std::nullopt));

  const auto* budget = document.registry().ctx().find<components::ParsedPayloadResourceBudget>();
  ASSERT_NE(budget, nullptr);
  EXPECT_EQ(budget->securityStats().retainedBytes, options.maximumParsedPayloadSize);

  const std::string rejectedText(2113, 'b');
  xml::ApplySourceEditResult rejected = document.setElementTextContent(*text, rejectedText);
  EXPECT_TRUE(rejected.applied);
  ASSERT_TRUE(rejected.diagnostic.has_value());
  EXPECT_THAT(rejected.diagnostic->reason, testing::HasSubstr("parsed-payload budget"));
  EXPECT_EQ(text->entityHandle().get<components::TextComponent>().text, acceptedText);
  EXPECT_TRUE(text->isEmptyForSelector(nullptr));
  EXPECT_EQ(budget->securityStats().retainedBytes, options.maximumParsedPayloadSize);

  xml::ApplySourceEditResult removed = document.removeElement(*text);
  ASSERT_TRUE(removed.applied);
  EXPECT_EQ(budget->securityStats().retainedBytes, options.maximumParsedPayloadSize);
  EXPECT_GT(budget->securityStats().projectedTextBytes, 0u);

  text.reset();
  EXPECT_LT(budget->securityStats().retainedBytes, options.maximumParsedPayloadSize);
  EXPECT_EQ(budget->securityStats().projectedTextBytes, 0u);
}

TEST(SVGDocument, IncrementalStylesheetPreflightsFamilyCapacityBeforeReplacingProjection) {
  components::DocumentResourceFamilyBudget::Limits familyLimits;
  familyLimits.parsedPayloadBytes = 5800;
  familyLimits.maximumTotalRetainedBytes = 5800;
  auto family = std::make_shared<components::DocumentResourceFamilyBudget>(familyLimits);
  SVGDocument::Settings settings;
  settings.resourceFamilyBudget = family;
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><style id="s">a{x:0}</style></svg>)", warnings,
      parser::SVGParser::Options(), std::move(settings));
  ASSERT_THAT(parsed, NoParseError());
  SVGDocument document = std::move(parsed).result();

  const std::string_view initialCss = "a{x:0}";
  std::size_t cssOffset = document.source().find(initialCss);
  ASSERT_NE(cssOffset, std::string_view::npos);
  const std::string acceptedCss = "a{x:1}";
  xml::ApplySourceEditResult accepted = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(cssOffset),
                           FileOffset::Offset(cssOffset + initialCss.size())},
      .replacement = acceptedCss,
      .sourceVersion = document.sourceVersion(),
  });
  ASSERT_TRUE(accepted.applied);
  ASSERT_THAT(accepted.diagnostic, Eq(std::nullopt));
  const std::size_t retainedBeforeRejection = family->totalRetainedBytes();
  const auto* payloadBudget =
      document.registry().ctx().find<components::ParsedPayloadResourceBudget>();
  ASSERT_NE(payloadBudget, nullptr);
  const SVGElement styleElement = *document.querySelector("#s");
  const Entity styleEntity = styleElement.unsafeEntityHandle().entity();
  EXPECT_FALSE(styleElement.entityHandle().all_of<components::TextComponent>());
  EXPECT_TRUE(styleElement.entityHandle().get<components::StylesheetComponent>().isCssType());
  const auto rejectedPreflight =
      components::ParsedPayloadResourceBudget::estimateStylesheetPreflightBytes(7, 7 + 64);
  ASSERT_TRUE(rejectedPreflight.has_value());
  EXPECT_EQ(*rejectedPreflight, 7u * 512u + 7u + 64u);
  EXPECT_FALSE(
      payloadBudget->canReserve(styleEntity, *rejectedPreflight,
                                components::ParsedPayloadResourceBudget::Category::Stylesheet));

  cssOffset = document.source().find(acceptedCss);
  ASSERT_NE(cssOffset, std::string_view::npos);
  const std::string rejectedCss = "a{x:00}";
  xml::ApplySourceEditResult rejected = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(cssOffset),
                           FileOffset::Offset(cssOffset + acceptedCss.size())},
      .replacement = rejectedCss,
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(rejected.applied);
  ASSERT_THAT(rejected.mutations, testing::Not(testing::IsEmpty()));
  EXPECT_EQ(rejected.mutations.front().kind, xml::XMLMutation::Kind::NodeValueChanged);
  EXPECT_TRUE(payloadBudget->securityStats().rejected);
  const auto styleNode = xml::XMLNode::TryCast(styleElement.entityHandle());
  ASSERT_TRUE(styleNode.has_value());
  EXPECT_THAT(styleNode->value(), Optional(Eq(rejectedCss)));
  ASSERT_TRUE(rejected.diagnostic.has_value());
  EXPECT_THAT(rejected.diagnostic->reason, testing::HasSubstr("parsed-payload budget"));
  const auto& stylesheet =
      document.querySelector("#s")->entityHandle().get<components::StylesheetComponent>();
  EXPECT_EQ(stylesheet.text, acceptedCss);
  EXPECT_EQ(family->totalRetainedBytes(), retainedBeforeRejection);
}

TEST(SVGDocument, IncrementalSourceEditsEnforceCumulativeTreeNodeLimitTransactionally) {
  parser::SVGParser::Options options;
  options.maximumTreeNodes = 4;
  auto document = ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><g id="host"><rect id="a"/></g></svg>)", options);

  const std::string_view firstChild = R"(<rect id="a"/>)";
  std::size_t editStart = document.source().find(firstChild);
  ASSERT_NE(editStart, std::string_view::npos);
  xml::ApplySourceEditResult accepted = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart),
                           FileOffset::Offset(editStart + firstChild.size())},
      .replacement = R"(<rect id="a"/><circle id="b"/>)",
      .sourceVersion = document.sourceVersion(),
  });
  ASSERT_TRUE(accepted.applied);
  ASSERT_THAT(accepted.diagnostic, Eq(std::nullopt));
  EXPECT_EQ(document.elementCount(), 4u);

  const std::string_view currentChildren = R"(<rect id="a"/><circle id="b"/>)";
  editStart = document.source().find(currentChildren);
  ASSERT_NE(editStart, std::string_view::npos);
  const std::string sourceBefore = std::string(document.source());
  const std::uint64_t versionBefore = document.sourceVersion();
  xml::ApplySourceEditResult rejected = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart),
                           FileOffset::Offset(editStart + currentChildren.size())},
      .replacement = R"(<rect id="a"/><circle id="b"/><ellipse id="c"/>)",
      .sourceVersion = versionBefore,
  });

  EXPECT_FALSE(rejected.applied);
  EXPECT_EQ(rejected.scope, xml::ReparseScope::ElementSubtree);
  ASSERT_TRUE(rejected.diagnostic.has_value());
  EXPECT_THAT(rejected.diagnostic->reason, testing::HasSubstr("tree-node limit"));
  EXPECT_EQ(document.source(), sourceBefore);
  EXPECT_EQ(document.sourceVersion(), versionBefore);
  EXPECT_THAT(document.querySelector("#c"), Eq(std::nullopt));
  EXPECT_EQ(document.elementCount(), 4u);
}

TEST(SVGDocument, IncrementalSourceEditAllowsIdentityPreservingMoveAtNodeLimit) {
  parser::SVGParser::Options options;
  options.maximumTreeNodes = 4;
  auto document = ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><g id="host"><rect id="a"/><circle id="b"/></g></svg>)",
      options);
  const SVGElement originalA = *document.querySelector("#a");
  const SVGElement originalB = *document.querySelector("#b");

  const std::string_view children = R"(<rect id="a"/><circle id="b"/>)";
  const std::size_t editStart = document.source().find(children);
  ASSERT_NE(editStart, std::string_view::npos);
  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart),
                           FileOffset::Offset(editStart + children.size())},
      .replacement = R"(<circle id="b"/><rect id="a"/>)",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_EQ(*document.querySelector("#a"), originalA);
  EXPECT_EQ(*document.querySelector("#b"), originalB);
  SVGElement host = *document.querySelector("#host");
  ASSERT_TRUE(host.firstChild().has_value());
  EXPECT_EQ(host.firstChild()->id(), "b");
  ASSERT_TRUE(host.firstChild()->nextSibling().has_value());
  EXPECT_EQ(host.firstChild()->nextSibling()->id(), "a");
}

TEST(SVGDocument, IncrementalSourceEditsEnforceResultingGlobalTreeDepthTransactionally) {
  parser::SVGParser::Options options;
  options.maximumTreeNodes = 10;
  options.maximumTreeDepth = 4;
  auto document = ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><g id="host"><rect id="a"/></g></svg>)", options);

  const std::string_view initialChild = R"(<rect id="a"/>)";
  std::size_t editStart = document.source().find(initialChild);
  ASSERT_NE(editStart, std::string_view::npos);
  xml::ApplySourceEditResult accepted = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart),
                           FileOffset::Offset(editStart + initialChild.size())},
      .replacement = R"(<g id="nested"><rect id="a"/></g>)",
      .sourceVersion = document.sourceVersion(),
  });
  ASSERT_TRUE(accepted.applied);
  ASSERT_THAT(accepted.diagnostic, Eq(std::nullopt));

  const std::string_view nestedChild = R"(<rect id="a"/>)";
  editStart = document.source().find(nestedChild);
  ASSERT_NE(editStart, std::string_view::npos);
  const std::string sourceBefore = std::string(document.source());
  const std::uint64_t versionBefore = document.sourceVersion();
  xml::ApplySourceEditResult rejected = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart),
                           FileOffset::Offset(editStart + nestedChild.size())},
      .replacement = R"(<g id="too-deep"><rect id="a"/></g>)",
      .sourceVersion = versionBefore,
  });

  EXPECT_FALSE(rejected.applied);
  EXPECT_EQ(rejected.scope, xml::ReparseScope::ElementSubtree);
  ASSERT_TRUE(rejected.diagnostic.has_value());
  EXPECT_THAT(rejected.diagnostic->reason, testing::HasSubstr("tree-depth limit"));
  EXPECT_EQ(document.source(), sourceBefore);
  EXPECT_EQ(document.sourceVersion(), versionBefore);
  EXPECT_THAT(document.querySelector("#too-deep"), Eq(std::nullopt));
  EXPECT_THAT(document.querySelector("#nested"), Optional(ElementIdEq("nested")));
}

TEST(SVGDocument, SourceBackedInsertAndRemoveElementUpdateSource) {
  auto document = ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><g id="group"></g></svg>)");
  SVGElement group = document.querySelector("#group").value();
  SVGRectElement rect = SVGRectElement::Create(document);
  rect.setId("inserted");

  xml::ApplySourceEditResult insertResult = document.insertElement(group, rect);
  EXPECT_TRUE(insertResult.applied);
  EXPECT_THAT(document.querySelector("#inserted"), Optional(ElementIdEq("inserted")));
  EXPECT_THAT(document.source(), testing::HasSubstr(R"(<rect id="inserted"/>)"));

  xml::ApplySourceEditResult removeResult = document.removeElement(rect);
  EXPECT_TRUE(removeResult.applied);
  EXPECT_THAT(document.querySelector("#inserted"), Eq(std::nullopt));
  EXPECT_THAT(document.source(), testing::Not(testing::HasSubstr("inserted")));
}

TEST(SVGDocument, SourceBackedInsertBeforeXmlReferenceUpdatesSourceOrder) {
  auto document =
      ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="a"/><rect id="b"/></svg>)");
  SVGElement reference = document.querySelector("#b").value();
  SVGRectElement inserted = SVGRectElement::Create(document);
  inserted.setId("inserted");

  xml::ApplySourceEditResult result =
      document.insertElement(document.svgElement(), inserted, reference);

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  const std::string_view source = document.source();
  const std::size_t aOffset = source.find(R"(id="a")");
  const std::size_t insertedOffset = source.find(R"(id="inserted")");
  const std::size_t bOffset = source.find(R"(id="b")");
  ASSERT_NE(aOffset, std::string_view::npos);
  ASSERT_NE(insertedOffset, std::string_view::npos);
  ASSERT_NE(bOffset, std::string_view::npos);
  EXPECT_LT(aOffset, insertedOffset);
  EXPECT_LT(insertedOffset, bOffset);
}

TEST(SVGDocument, SourceBackedMoveElementBetweenParentsMarksOldParentRemoved) {
  auto document = ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg">
    <g id="a"><rect id="moved"/></g>
    <g id="b"></g>
  </svg>)");
  SVGElement sourceParent = document.querySelector("#a").value();
  SVGElement targetParent = document.querySelector("#b").value();
  SVGElement moved = document.querySelector("#moved").value();

  xml::ApplySourceEditResult result = document.insertElement(targetParent, moved);

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(sourceParent.firstChild(), Eq(std::nullopt));
  ASSERT_THAT(targetParent.firstChild(), Optional(ElementIdEq("moved")));
  EXPECT_THAT(document.source(), testing::HasSubstr(R"(<g id="a"></g>)"));
  EXPECT_THAT(document.source(), testing::HasSubstr(R"(<g id="b"><rect id="moved"/></g>)"));
}

constexpr std::string_view kRenderedShadowSource = R"(
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
     width="100" height="100">
  <defs><g id="shape"><rect width="10" height="10"/></g></defs>
  <use id="host" xlink:href="#shape"/>
  <g id="ordinary"><rect id="old"/></g>
  <g id="destination"></g>
</svg>)";

void InstantiateRenderedShadowTree(SVGDocument& document) {
  document.setCanvasSize(100, 100);
  Renderer renderer;
  renderer.draw(document);
  ASSERT_FALSE(renderer.takeSnapshot().empty());
}

TEST(SVGDocument, SourceBackedInsertIgnoresRenderedShadowNodes) {
  SVGDocument document = ParseSVG(kRenderedShadowSource);
  InstantiateRenderedShadowTree(document);
  SVGRectElement inserted = SVGRectElement::Create(document);
  inserted.setId("inserted");

  xml::ApplySourceEditResult result =
      document.insertElement(*document.querySelector("#destination"), inserted);

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(document.querySelector("#inserted"), Optional(ElementIdEq("inserted")));
  EXPECT_THAT(document.source(), testing::HasSubstr(R"(<rect id="inserted"/>)"));
}

TEST(SVGDocument, SourceBackedSubtreeEditIgnoresRenderedShadowNodes) {
  SVGDocument document = ParseSVG(kRenderedShadowSource);
  InstantiateRenderedShadowTree(document);
  const std::size_t oldOffset = document.source().find(R"(<rect id="old"/>)");
  ASSERT_NE(oldOffset, std::string_view::npos);

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(oldOffset), FileOffset::Offset(oldOffset + 16)},
      .replacement = R"(<circle id="edited"/>)",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(document.querySelector("#old"), Eq(std::nullopt));
  EXPECT_THAT(document.querySelector("#edited"), Optional(ElementIdEq("edited")));
}

TEST(SVGDocument, SourceBackedMoveOfRenderedShadowHostIgnoresComputedChildren) {
  SVGDocument document = ParseSVG(kRenderedShadowSource);
  InstantiateRenderedShadowTree(document);
  SVGElement host = *document.querySelector("#host");
  SVGElement destination = *document.querySelector("#destination");

  xml::ApplySourceEditResult result = document.insertElement(destination, host);

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(destination.firstChild(), Optional(ElementIdEq("host")));
  const std::string_view source = document.source();
  EXPECT_LT(source.find(R"(id="destination")"), source.find(R"(id="host")"));
}

namespace {

/// Tag names of \p node's XML children walked with firstChild()/nextSibling(). `std::optional<
/// xml::XMLNode>` has no printer, so listing names is what names a leaked entity on failure.
std::vector<std::string> ForwardXmlChildTagNames(const xml::XMLNode& node) {
  std::vector<std::string> names;
  for (std::optional<xml::XMLNode> child = node.firstChild(); child.has_value();
       child = child->nextSibling()) {
    names.emplace_back(child->tagName().name);
  }
  return names;
}

/// Tag names of \p node's XML children walked with lastChild()/previousSibling(), reverse order.
std::vector<std::string> ReverseXmlChildTagNames(const xml::XMLNode& node) {
  std::vector<std::string> names;
  for (std::optional<xml::XMLNode> child = node.lastChild(); child.has_value();
       child = child->previousSibling()) {
    names.emplace_back(child->tagName().name);
  }
  return names;
}

}  // namespace

TEST(SVGDocument, RenderedShadowContentIsNotVisibleThroughTheXmlTree) {
  SVGDocument document = ParseSVG(kRenderedShadowSource);
  InstantiateRenderedShadowTree(document);
  SVGElement host = *document.querySelector("#host");
  std::optional<xml::XMLNode> hostNode = xml::XMLNode::TryCast(host.entityHandle());
  ASSERT_THAT(hostNode, testing::Ne(std::nullopt));

  // Rendering instantiated the referenced `<g>` as entities parented under `<use>`. They are not
  // XML nodes, so the authored document still sees `<use>` as childless.
  EXPECT_THAT(ForwardXmlChildTagNames(*hostNode), testing::IsEmpty());
  EXPECT_THAT(ReverseXmlChildTagNames(*hostNode), testing::IsEmpty());
  EXPECT_THAT(std::string(std::string_view(hostNode->serializeToString(0, /*prettyPrint=*/false))),
              testing::AllOf(testing::StartsWith("<use "), testing::EndsWith("/>")));
}

TEST(SVGDocument, SourceBackedInsertInvalidElementReportsProjectionDiagnostic) {
  auto document = ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><g id="group"></g></svg>)");
  SVGElement group = document.querySelector("#group").value();
  SVGRectElement rect = SVGRectElement::Create(document);
  rect.setId("bad");
  rect.setAttribute("width", "not-a-length");

  xml::ApplySourceEditResult result = document.insertElement(group, rect);

  EXPECT_TRUE(result.applied);
  ASSERT_TRUE(result.diagnostic.has_value());
  EXPECT_THAT(result.diagnostic->reason, testing::HasSubstr("Invalid length or percentage"));
  EXPECT_THAT(document.source(), testing::HasSubstr(R"(width="not-a-length")"));
}

TEST(SVGDocument, SourceBackedInsertBeforeProgrammaticReferenceReportsDiagnostic) {
  auto document = ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"></svg>)");
  SVGRectElement reference = SVGRectElement::Create(document);
  reference.setId("reference");

  SVGRectElement inserted = SVGRectElement::Create(document);
  inserted.setId("inserted");

  xml::ApplySourceEditResult result =
      document.insertElement(document.svgElement(), inserted, reference);

  EXPECT_FALSE(result.applied);
  EXPECT_EQ(result.scope, xml::ReparseScope::ElementSubtree);
  ASSERT_TRUE(result.diagnostic.has_value());
  EXPECT_THAT(result.diagnostic->reason, testing::HasSubstr("without XML source identity"));
  EXPECT_THAT(document.querySelector("#inserted"), Eq(std::nullopt));
}

TEST(SVGDocument, ProgrammaticInsertElementFallbackDoesNotMutateSourceLessDocument) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);
  rect.setId("detached");

  xml::ApplySourceEditResult result = document.insertElement(document.svgElement(), rect);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(document.querySelector("#detached"), Eq(std::nullopt));
}

TEST(SVGDocument, SourceBackedInsertElementWithoutXmlParentIdentityFallsBackToUnapplied) {
  auto document = ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"/>)");
  SVGRectElement detachedParent = SVGRectElement::Create(document);
  SVGRectElement child = SVGRectElement::Create(document);
  child.setId("child");

  xml::ApplySourceEditResult result = document.insertElement(detachedParent, child);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(document.querySelector("#child"), Eq(std::nullopt));
  EXPECT_THAT(document.source(), testing::Not(testing::HasSubstr("child")));
}

TEST(SVGDocument, SourceBackedRemoveProgrammaticElementWithXmlIdentityUpdatesSource) {
  auto document = ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"/>)");
  SVGRectElement rect = SVGRectElement::Create(document);
  rect.setId("programmatic");
  document.svgElement().appendChild(rect);
  ASSERT_THAT(document.querySelector("#programmatic"), Optional(ElementIdEq("programmatic")));

  xml::ApplySourceEditResult result = document.removeElement(rect);

  EXPECT_TRUE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(document.querySelector("#programmatic"), Eq(std::nullopt));
  EXPECT_THAT(document.source(), testing::Not(testing::HasSubstr("programmatic")));
}

TEST(SVGDocument, ProgrammaticRemoveElementFallbackRemovesAttachedElement) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);
  rect.setId("attached");
  document.svgElement().appendChild(rect);
  ASSERT_THAT(document.querySelector("#attached"), Optional(ElementIdEq("attached")));

  xml::ApplySourceEditResult result = document.removeElement(rect);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(document.querySelector("#attached"), Eq(std::nullopt));
}

TEST(SVGDocument, SourceBackedRemoveDetachedElementWithoutXmlIdentityIsUnapplied) {
  auto document = ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"/>)");
  SVGRectElement detached = SVGRectElement::Create(document);
  detached.setId("detached");

  xml::ApplySourceEditResult result = document.removeElement(detached);

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result.diagnostic, Eq(std::nullopt));
  EXPECT_THAT(document.querySelector("#detached"), Eq(std::nullopt));
  EXPECT_THAT(document.source(), testing::Not(testing::HasSubstr("detached")));
}

TEST(SVGDocument, SourceBackedRemoveRootElementIsRefused) {
  auto document = ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="20"><rect id="child"/></svg>)");
  const std::string sourceBefore(document.source());

  xml::ApplySourceEditResult result = document.removeElement(document.svgElement());

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result.diagnostic,
              Optional(testing::Field("reason", &ParseDiagnostic::reason,
                                      testing::HasSubstr("document root element"))));
  EXPECT_EQ(document.source(), sourceBefore);
  EXPECT_THAT(document.querySelector("#child"), Optional(ElementIdEq("child")));
  // Sizing the document reads the root's sized-element properties.
  EXPECT_THAT(document.canvasSize(), Vector2Eq(10, 20));
}

TEST(SVGDocument, SourceBackedRemoveOfRootThroughElementIsRefused) {
  auto document = ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="20"><rect id="child"/></svg>)");
  const std::string sourceBefore(document.source());

  document.svgElement().remove();

  EXPECT_EQ(document.source(), sourceBefore);
  EXPECT_THAT(document.querySelector("#child"), Optional(ElementIdEq("child")));
  EXPECT_THAT(document.canvasSize(), Vector2Eq(10, 20));
}

TEST(SVGDocument, ProgrammaticRemoveRootElementIsRefused) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);
  rect.setId("child");
  document.svgElement().appendChild(rect);
  const Vector2i sizeBefore = document.canvasSize();

  xml::ApplySourceEditResult result = document.removeElement(document.svgElement());
  document.svgElement().remove();

  EXPECT_FALSE(result.applied);
  EXPECT_THAT(result.diagnostic,
              Optional(testing::Field("reason", &ParseDiagnostic::reason,
                                      testing::HasSubstr("document root element"))));
  EXPECT_THAT(document.querySelector("#child"), Optional(ElementIdEq("child")));
  EXPECT_THAT(document.canvasSize(), Vector2Eq(sizeBefore.x, sizeBefore.y));
}

TEST(SVGDocument, TypedMutationHelperWrapsDomOperations) {
  SVGDocument document;
  SVGRectElement kept = SVGRectElement::Create(document);
  SVGRectElement inserted = SVGRectElement::Create(document);
  SVGRectElement replacement = SVGRectElement::Create(document);

  document.withWriteAccess([&](SVGDocumentMutation& mutation) {
    EXPECT_EQ(&mutation.access().registry(), &document.registry());
    mutation.setCanvasSize(64, 32);
    mutation.useAutomaticCanvasSize();
    mutation.appendChild(document.svgElement(), kept);
    mutation.setAttribute(kept, "id", "kept");
    mutation.removeAttribute(kept, "id");
    mutation.insertBefore(document.svgElement(), inserted, kept);
    mutation.replaceChild(document.svgElement(), replacement, inserted);
    mutation.removeChild(document.svgElement(), replacement);
    mutation.remove(kept);
  });

  EXPECT_EQ(document.canvasSize(), Vector2i(512, 512));
  EXPECT_EQ(document.svgElement().firstChild(), std::nullopt);
}

/**
 * Verify that the document's root element is an `<svg>` element.
 */
TEST(SVGDocument, RootElementTag) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 300 300">
      <circle id="c1" cx="150" cy="150" r="50"/>
    </svg>
  )");

  EXPECT_EQ(document.svgElement().type(), ElementType::SVG);
}

TEST(SVGDocument, ProjectsKnownElementTypes) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <a id="anchor"><tspan id="anchor-tspan">link</tspan></a>
      <circle id="circle"/>
      <clipPath id="clip"><rect id="clip-rect"/></clipPath>
      <defs id="defs"/>
      <ellipse id="ellipse"/>
      <filter id="filter"><feGaussianBlur id="blur"/></filter>
      <g id="group"/>
      <line id="line"/>
      <path id="path"/>
      <polygon id="polygon"/>
      <polyline id="polyline"/>
      <rect id="rect"/>
      <style id="style">rect { fill: red; }</style>
      <switch id="switch"/>
      <text id="text"><tspan id="tspan">hello</tspan></text>
      <custom id="custom"/>
    </svg>
  )");

  EXPECT_EQ(document.querySelector("#anchor")->type(), ElementType::A);
  EXPECT_EQ(document.querySelector("#circle")->type(), ElementType::Circle);
  EXPECT_EQ(document.querySelector("#clip")->type(), ElementType::ClipPath);
  EXPECT_EQ(document.querySelector("#defs")->type(), ElementType::Defs);
  EXPECT_EQ(document.querySelector("#ellipse")->type(), ElementType::Ellipse);
  EXPECT_EQ(document.querySelector("#filter")->type(), ElementType::Filter);
  EXPECT_EQ(document.querySelector("#blur")->type(), ElementType::FeGaussianBlur);
  EXPECT_EQ(document.querySelector("#group")->type(), ElementType::G);
  EXPECT_EQ(document.querySelector("#line")->type(), ElementType::Line);
  EXPECT_EQ(document.querySelector("#path")->type(), ElementType::Path);
  EXPECT_EQ(document.querySelector("#polygon")->type(), ElementType::Polygon);
  EXPECT_EQ(document.querySelector("#polyline")->type(), ElementType::Polyline);
  EXPECT_EQ(document.querySelector("#rect")->type(), ElementType::Rect);
  EXPECT_EQ(document.querySelector("#style")->type(), ElementType::Style);
  EXPECT_EQ(document.querySelector("#switch")->type(), ElementType::Switch);
  EXPECT_EQ(document.querySelector("#text")->type(), ElementType::Text);
  EXPECT_EQ(document.querySelector("#tspan")->type(), ElementType::TSpan);
  EXPECT_EQ(document.querySelector("#custom")->type(), ElementType::Unknown);

  EXPECT_TRUE(
      document.querySelector("#anchor")->entityHandle().all_of<components::TextComponent>());
  EXPECT_TRUE(
      document.querySelector("#text")->entityHandle().all_of<components::TextRootComponent>());
  EXPECT_TRUE(
      document.querySelector("#style")->entityHandle().all_of<components::StylesheetComponent>());
}

/**
 * Verify that the width() and height() accessors reflect the canvas size.
 */
TEST(SVGDocument, WidthHeightAccessors) {
  SVGDocument doc;
  doc.setCanvasSize(123, 456);
  EXPECT_EQ(doc.width(), 123);
  EXPECT_EQ(doc.height(), 456);
}

/**
 * Verify that when the viewBox and canvas size are identical,
 * the `canvasFromDocumentTransform()` is the identity transform.
 */
TEST(SVGDocument, CanvasFromDocumentTransformIdentity) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
    </svg>
  )");

  const Transform2d transform = document.canvasFromDocumentTransform();
  EXPECT_TRUE(transform.isIdentity()) << "transform=" << transform;
}

/**
 * Verify that when the canvas size differs from the viewBox, \c canvasFromDocumentTransform()
 * returns a transformation in `destinationFromSource` notation that maps coordinates from the
 * document's viewBox (source) to the canvas-scaled output space (destination).
 *
 * For a viewBox of 200×200 and a canvas size of 100×200, the transformation scales the x-coordinate
 * by 0.5 (i.e. a point (50, 100) in the viewBox is mapped to (25, 100) in canvas space),
 * while the y-coordinate remains unchanged.
 */
TEST(SVGDocument, CanvasFromDocumentTransformScaling) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200" width="100" height="200">
    </svg>
  )");

  const Transform2d transform = document.canvasFromDocumentTransform();
  EXPECT_EQ(transform.transformPosition(Vector2d(50, 100)), Vector2d(25, 100));
}

/**
 * Verify that the equality operator distinguishes between different documents.
 *
 * Documents referencing the same underlying registry (via copy construction) compare equal,
 * while independently created documents are not equal.
 */
TEST(SVGDocument, EqualityOperator) {
  SVGDocument doc1;
  SVGDocument doc2 =
      doc1.svgElement().ownerDocument();  // Should refer to the same underlying registry.
  EXPECT_TRUE(doc1 == doc2);

  SVGDocument doc3;
  EXPECT_FALSE(doc1 == doc3);
}

/**
 * Verify that more advanced query selectors work correctly.
 *
 * This includes using attribute selectors and descendant combinators.
 */
TEST(SVGDocument, QuerySelectorAdvanced) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 400 400">
       <g id="group1">
           <rect id="r1" x="10" y="10" width="50" height="50" data-type="foo"/>
       </g>
       <g id="group2">
           <rect id="r2" x="70" y="10" width="50" height="50" data-type="bar"/>
       </g>
    </svg>
  )");
  // Query by attribute.
  EXPECT_THAT(document.querySelector("[data-type='bar']"), Optional(ElementIdEq("r2")));
  // Query using descendant combinator and id selectors.
  EXPECT_THAT(document.querySelector("svg > g#group1 > rect"), Optional(ElementIdEq("r1")));
}

TEST(SVGDocument, RetiringAnUnmappedDuplicateIdKeepsTheMappedElement) {
  auto document = ParseSVG(
      "<svg xmlns='http://www.w3.org/2000/svg'><rect id='same'/><circle id='same'/></svg>");
  auto first = document.querySelector("rect");
  auto duplicate = document.querySelector("circle");
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(duplicate.has_value());
  auto& context = document.registry().ctx().get<components::SVGDocumentContext>();
  EXPECT_EQ(context.getEntityById(RcString("same")), first->entityHandle().entity());
  duplicate->setId("other");
  EXPECT_EQ(context.getEntityById(RcString("same")), first->entityHandle().entity());
  first->setId("moved");
  EXPECT_EQ(context.getEntityById(RcString("same")), Entity(entt::null));
  EXPECT_EQ(context.getEntityById(RcString("moved")), first->entityHandle().entity());
}

TEST(SVGDocument, AttachedReplacementIdSurvivesRetainedDetachedNodeCollection) {
  SVGDocument document;
  auto root = document.svgElement();
  std::optional<SVGRectElement> original = SVGRectElement::Create(document);
  root.appendChild(*original);
  original->setId("same");
  const Entity originalEntity = original->entityHandle().entity();
  root.removeChild(*original);
  ASSERT_TRUE(document.registry().valid(originalEntity));

  auto replacement = SVGRectElement::Create(document);
  root.appendChild(replacement);
  replacement.setId("same");
  auto& context = document.registry().ctx().get<components::SVGDocumentContext>();
  EXPECT_EQ(context.getEntityById(RcString("same")), replacement.entityHandle().entity());
  original.reset();
  EXPECT_FALSE(document.registry().valid(originalEntity));
  EXPECT_EQ(context.getEntityById(RcString("same")), replacement.entityHandle().entity());
}

TEST(SVGDocument, DetachedDuplicateIdCannotDisplaceAttachedMapping) {
  SVGDocument document;
  auto attached = SVGRectElement::Create(document);
  document.svgElement().appendChild(attached);
  attached.setId("same");
  {
    auto detached = SVGRectElement::Create(document);
    detached.setId("same");
    auto& context = document.registry().ctx().get<components::SVGDocumentContext>();
    EXPECT_EQ(context.getEntityById(RcString("same")), attached.entityHandle().entity());
  }
  auto& context = document.registry().ctx().get<components::SVGDocumentContext>();
  EXPECT_EQ(context.getEntityById(RcString("same")), attached.entityHandle().entity());
}

TEST(SVGDocument, AnimationReferenceBudgetIsSharedAndSticky) {
  auto document =
      ParseSVG("<svg xmlns='http://www.w3.org/2000/svg'><g><g><rect id='r'/></g></g></svg>");
  components::AnimationResourceBudget budget;
  ASSERT_EQ(
      budget.reserveReferenceWork(components::AnimationResourceBudget::kMaximumReferenceWork - 32),
      true);
  EXPECT_EQ(components::FindAnimationReference(document.registry(), "missing", budget),
            Entity(entt::null));
  EXPECT_EQ(budget.rejected(), false);
  EXPECT_LT(budget.remainingReferenceWork(), 32u);
  EXPECT_EQ(components::FindAnimationReference(document.registry(), "missing", budget),
            Entity(entt::null));
  EXPECT_EQ(budget.rejected(), true);
  EXPECT_EQ(components::FindAnimationReference(document.registry(), "r", budget),
            Entity(entt::null));
}

TEST(SVGDocument, AnimationReferenceBudgetRejectsLongIdBeforeLookup) {
  SVGDocument document;
  components::AnimationResourceBudget budget;
  const std::string id(components::AnimationResourceBudget::kMaximumReferenceWork + 1, 'x');
  EXPECT_EQ(components::FindAnimationReference(document.registry(), id, budget),
            Entity(entt::null));
  EXPECT_EQ(budget.rejected(), true);
  EXPECT_EQ(budget.remainingReferenceWork(),
            components::AnimationResourceBudget::kMaximumReferenceWork);
}

TEST(SVGDocument, AnimationReferenceExhaustionDoesNotPublishPartialEvents) {
  std::string source =
      "<svg xmlns='http://www.w3.org/2000/svg'><rect id='button'>"
      "<set id='first' attributeName='width' to='12' begin='button.click' dur='2s'/>";
  for (int i = 0; i < 1500; ++i) {
    source += "<set attributeName='width' to='8' begin='missing" + std::to_string(i) +
              ".click' dur='2s'/>";
  }
  source += "</rect></svg>";
  auto document = ParseSVG(source);
  const auto target = document.querySelector("#button");
  const auto first = document.querySelector("#first");
  ASSERT_THAT(target, testing::Ne(std::nullopt));
  ASSERT_THAT(first, testing::Ne(std::nullopt));
  EXPECT_EQ(document.dispatchAnimationEvent("click", *target), false);
  const auto& state = first->entityHandle().get<components::AnimationStateComponent>();
  EXPECT_THAT(state.eventBeginTimes, testing::IsEmpty());
  EXPECT_THAT(state.eventEndTimes, testing::IsEmpty());
}

TEST(SVGDocument, AnimationFullHistoryDoesNotPartiallyAcceptAnotherTarget) {
  auto document = ParseSVG(
      "<svg xmlns='http://www.w3.org/2000/svg'><rect id='button'>"
      "<set id='first' attributeName='width' to='12' begin='button.click' dur='1s'/>"
      "<set id='full' attributeName='width' to='8' begin='button.click' dur='1s'/></rect></svg>");
  const auto target = document.querySelector("#button");
  const auto first = document.querySelector("#first");
  const auto full = document.querySelector("#full");
  ASSERT_THAT(target, testing::Ne(std::nullopt));
  ASSERT_THAT(first, testing::Ne(std::nullopt));
  ASSERT_THAT(full, testing::Ne(std::nullopt));
  auto& fullState = full->entityHandle().get_or_emplace<components::AnimationStateComponent>();
  for (int i = 0; i < 1024; ++i) {
    fullState.eventBeginTimes.push_back(i);
  }
  document.setTime(2048);
  EXPECT_EQ(document.dispatchAnimationEvent("click", *target), false);
  EXPECT_THAT(first->entityHandle().get<components::AnimationStateComponent>().eventBeginTimes,
              testing::IsEmpty());
  EXPECT_THAT(fullState.eventBeginTimes, testing::SizeIs(1024));
  EXPECT_EQ(fullState.eventBeginTimes.back(), 1023);
}

TEST(SVGDocument, AnimationHistoryAllocationExhaustionDoesNotPublishEvents) {
  std::string source = "<svg xmlns='http://www.w3.org/2000/svg'><rect id='button'>";
  for (int i = 0; i < 600; ++i) {
    source += "<set attributeName='width' to='8' begin='button.click' dur='1s'/>";
  }
  source += "</rect></svg>";
  auto document = ParseSVG(source);
  const auto target = document.querySelector("#button");
  ASSERT_THAT(target, testing::Ne(std::nullopt));
  EXPECT_EQ(document.dispatchAnimationEvent("click", *target), false);
  for (const auto& [entity, state] :
       document.registry().view<components::AnimationStateComponent>().each()) {
    SCOPED_TRACE(static_cast<unsigned>(entity));
    EXPECT_THAT(state.eventBeginTimes, testing::IsEmpty());
    EXPECT_THAT(state.eventEndTimes, testing::IsEmpty());
  }
}

TEST(SVGDocument, AnimationTargetQueryUsesAttachedIdentityAndNamespace) {
  auto document = ParseSVG(
      "<svg xmlns='http://www.w3.org/2000/svg'><rect id='r'/><circle id='r'/>"
      "<set href='#r' attributeName='opacity' to='0.5'/>"
      "<g id='foreign'><set xmlns='urn:foreign' attributeName='opacity' to='0.5'/></g></svg>");
  auto first = document.querySelector("rect");
  auto duplicate = document.querySelector("circle");
  auto foreign = document.querySelector("#foreign");
  ASSERT_THAT(first, testing::Ne(std::nullopt));
  ASSERT_THAT(duplicate, testing::Ne(std::nullopt));
  ASSERT_THAT(foreign, testing::Ne(std::nullopt));
  EXPECT_EQ(IsAnimationTarget(*first), true);
  EXPECT_EQ(IsAnimationTarget(*duplicate), false);
  EXPECT_EQ(IsAnimationTarget(*foreign), false);
  ASSERT_EQ(document.removeElement(*first).applied, true);
  EXPECT_EQ(IsAnimationTarget(*first), false);
  EXPECT_EQ(IsAnimationTarget(*duplicate), true);
}

TEST(SVGDocument, AnimationTargetQueryObservesHrefEditsBeforeSampling) {
  const std::pair<const char*, const char*> declarations[] = {
      {"set", "attributeName='opacity' to='0.5'"},
      {"animate", "attributeName='opacity' to='0.5' dur='1s'"},
      {"animateTransform", "attributeName='transform' type='translate' to='10' dur='1s'"},
      {"animateMotion", "path='M0 0H10' dur='1s'"},
  };
  for (const auto& [tag, attributes] : declarations) {
    SCOPED_TRACE(tag);
    auto document = ParseSVG(
        "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'>"
        "<rect id='a'/><circle id='b'/><g id='parent'><" +
        std::string(tag) + " id='animation' href='#a' " + attributes + "/></g></svg>");
    auto animation = document.querySelector("#animation");
    const auto first = document.querySelector("#a");
    const auto second = document.querySelector("#b");
    const auto parent = document.querySelector("#parent");
    ASSERT_TRUE(animation && first && second && parent);
    EXPECT_TRUE(IsAnimationTarget(*first));
    animation->setAttribute("href", "#b");
    EXPECT_FALSE(IsAnimationTarget(*first));
    EXPECT_TRUE(IsAnimationTarget(*second));
    animation->setAttribute(xml::XMLQualifiedNameRef("xlink", "href"), "#a");
    animation->setAttribute("href", "");
    EXPECT_FALSE(IsAnimationTarget(*first));
    EXPECT_FALSE(IsAnimationTarget(*second));
    EXPECT_FALSE(IsAnimationTarget(*parent));
    animation->removeAttribute("href");
    EXPECT_TRUE(IsAnimationTarget(*first));
    animation->removeAttribute(xml::XMLQualifiedNameRef("xlink", "href"));
    const std::string source(document.source());
    EXPECT_FALSE(IsAnimationTarget(*first));
    EXPECT_FALSE(IsAnimationTarget(*second));
    EXPECT_TRUE(IsAnimationTarget(*parent));
    EXPECT_EQ(document.source(), source);
  }
}

TEST(SVGDocument, AnimationTargetQueryConservativelyProtectsOnReferenceExhaustion) {
  std::string source = "<svg xmlns='http://www.w3.org/2000/svg'><rect id='target'/>";
  for (int i = 0; i < 1500; ++i) {
    source += "<set href='#missing' attributeName='width' to='8'/>";
  }
  source += "<set href='#target' attributeName='width' to='12'/></svg>";
  auto document = ParseSVG(source);
  const auto target = document.querySelector("#target");
  ASSERT_THAT(target, testing::Ne(std::nullopt));
  EXPECT_EQ(IsAnimationTarget(*target), true);
}

TEST(SVGDocument, AnimationTargetQueryConservativelyProtectsOnDeclarationOverflow) {
  std::string source = "<svg xmlns='http://www.w3.org/2000/svg'><rect id='target'/>";
  for (int i = 0; i < 4097; ++i) {
    source += "<set attributeName='opacity' to='1'/>";
  }
  source += "</svg>";
  auto document = ParseSVG(source);
  const auto target = document.querySelector("#target");
  ASSERT_THAT(target, testing::Ne(std::nullopt));
  EXPECT_EQ(IsAnimationTarget(*target), true);
}

TEST(SVGDocument, AnimationControlsAndLifecyclePreserveSourceAndClock) {
  constexpr std::string_view source = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><rect>
      <animate id="a" attributeName="width" from="0" to="100" begin="indefinite" dur="1s" repeatCount="3"/>
    </rect></svg>)svg";
  parser::SVGParser::Options options;
  options.enableExperimental = true;
  auto document = ParseSVG(source, options);
  auto animation = document.querySelector("#a");
  ASSERT_THAT(animation, testing::Ne(std::nullopt));
  document.setTime(1);
  EXPECT_EQ(document.beginAnimation(*animation), true);
  auto batch = document.animationEvents(0, 5);
  EXPECT_EQ(batch.complete, true);
  ASSERT_THAT(batch.events, testing::SizeIs(4));
  EXPECT_THAT(
      batch.events,
      testing::ElementsAre(
          testing::AllOf(testing::Field(&SVGAnimationEvent::type, SVGAnimationEvent::Type::Begin),
                         testing::Field(&SVGAnimationEvent::time, 1)),
          testing::AllOf(testing::Field(&SVGAnimationEvent::type, SVGAnimationEvent::Type::Repeat),
                         testing::Field(&SVGAnimationEvent::time, 2)),
          testing::AllOf(testing::Field(&SVGAnimationEvent::repeatIndex, 2),
                         testing::Field(&SVGAnimationEvent::time, 3)),
          testing::AllOf(testing::Field(&SVGAnimationEvent::type, SVGAnimationEvent::Type::End),
                         testing::Field(&SVGAnimationEvent::time, 4))));
  document.setTime(2.5);
  EXPECT_EQ(document.endAnimation(*animation), true);
  batch = document.animationEvents(2, 5);
  ASSERT_THAT(batch.events, testing::SizeIs(1));
  EXPECT_EQ(batch.events.front().time, 2.5);
  EXPECT_EQ(document.currentTime(), 2.5);
  EXPECT_EQ(document.source(), source);
}

TEST(SVGDocument, AnimationControlsRejectForeignDetachedAndSecureTargets) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;
  constexpr std::string_view source = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="r"><set id="a" attributeName="fill" to="red" begin="focus"/></rect></svg>)svg";
  auto first = ParseSVG(source, options);
  auto second = ParseSVG(source, options);
  auto local = first.querySelector("#a");
  auto foreign = second.querySelector("#a");
  ASSERT_THAT(local, testing::Ne(std::nullopt));
  ASSERT_THAT(foreign, testing::Ne(std::nullopt));
  EXPECT_EQ(first.beginAnimation(*foreign), false);
  local->remove();
  EXPECT_EQ(first.beginAnimation(*local), false);
  ParseWarningSink warnings;
  SVGDocument::Settings settings;
  settings.processingMode = ProcessingMode::SecureAnimated;
  auto secure = parser::SVGParser::ParseSVG(source, warnings, options, std::move(settings));
  ASSERT_THAT(secure, NoParseError());
  auto animation = secure.result().querySelector("#a");
  ASSERT_THAT(animation, testing::Ne(std::nullopt));
  EXPECT_EQ(secure.result().beginAnimation(*animation), false);
  EXPECT_EQ(secure.result().dispatchAnimationEvent("focus", *animation), false);
}

TEST(SVGDocument, AnimationLifecycleQueryFailsClosedOnExcessiveEvents) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;
  auto document = ParseSVG(R"svg(<svg xmlns="http://www.w3.org/2000/svg"><rect>
    <set attributeName="fill" to="red" dur="0.001s" repeatCount="indefinite"/>
    </rect></svg>)svg",
                           options);
  const auto batch = document.animationEvents(-1, 100);
  EXPECT_EQ(batch.complete, false);
  EXPECT_THAT(batch.events, testing::IsEmpty());
}

TEST(SVGDocument, AnimationLifecycleQueryRejectsNonprogressingRepeatTimes) {
  auto document = ParseSVG(R"svg(<svg xmlns="http://www.w3.org/2000/svg"><rect>
    <animate attributeName="width" from="0" to="100" begin="1s" dur="1e-300s" repeatCount="indefinite"/>
    </rect></svg>)svg");
  const auto batch = document.animationEvents(1, 2);
  EXPECT_EQ(batch.complete, false);
  EXPECT_THAT(batch.events, testing::IsEmpty());
}

TEST(SVGDocument, SourceInsertedForeignAnimationStaysUnknown) {
  auto document =
      ParseSVG(R"svg(<svg xmlns="http://www.w3.org/2000/svg"><g id="group"/></svg>)svg");
  const std::string source(document.source());
  const auto start = source.find("<g");
  const auto end = source.find("/>", start) + 2;
  const auto result = document.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(start), FileOffset::Offset(end)},
      .replacement =
          R"svg(<g xmlns:x="urn:foreign"><x:animate id="foreign" attributeName="width" from="0" to="100" dur="1s"/></g>)svg",
      .sourceVersion = document.sourceVersion(),
  });
  EXPECT_EQ(result.applied, true);
  const auto foreign = document.querySelector("#foreign");
  ASSERT_THAT(foreign, testing::Ne(std::nullopt));
  EXPECT_EQ(foreign->type(), ElementType::Unknown);
  EXPECT_THAT(document.animationEvents(-1, 2).events, testing::IsEmpty());
}

TEST(SVGDocument, AnimationNamespaceRoundtripDiscardsOldEventSessionOnlyOnUpdate) {
  auto document = ParseSVG(
      "<svg xmlns='http://www.w3.org/2000/svg'><rect id='button'>"
      "<set id='animation' attributeName='width' to='12' begin='button.click' dur='2s' "
      "restart='never'/></rect></svg>");
  const auto target = document.querySelector("#button");
  auto animation = document.querySelector("#animation");
  ASSERT_THAT(target, testing::Ne(std::nullopt));
  ASSERT_THAT(animation, testing::Ne(std::nullopt));
  EXPECT_EQ(document.dispatchAnimationEvent("click", *target), true);
  animation->setAttribute("xmlns", "urn:foreign");
  EXPECT_EQ(IsAnimationTarget(*target), false);
  EXPECT_THAT(animation->entityHandle().get<components::AnimationStateComponent>().eventBeginTimes,
              testing::ElementsAre(0.0));
  EXPECT_THAT(document.animationEvents(-1, 3).events, testing::IsEmpty());
  EXPECT_EQ(animation->entityHandle().try_get<components::AnimationStateComponent>(), nullptr);
  animation->removeAttribute("xmlns");
  EXPECT_THAT(document.animationEvents(-1, 3).events, testing::IsEmpty());
  document.setTime(4);
  EXPECT_EQ(document.dispatchAnimationEvent("click", *target), true);
  EXPECT_THAT(animation->entityHandle().get<components::AnimationStateComponent>().eventBeginTimes,
              testing::ElementsAre(4.0));
}

TEST(SVGDocument, AnimationControlsRejectRetainedForeignNamespaceHandle) {
  auto document = ParseSVG(R"svg(<svg xmlns="http://www.w3.org/2000/svg"><rect>
    <animate id="a" attributeName="width" from="0" to="100" dur="1s" begin="indefinite"/>
    </rect></svg>)svg");
  auto animation = document.querySelector("#a").value();
  animation.setAttribute("xmlns", "urn:foreign");
  EXPECT_EQ(document.beginAnimation(animation), false);
  EXPECT_EQ(document.endAnimation(animation), false);
  animation.removeAttribute("xmlns");
  EXPECT_EQ(document.beginAnimation(animation), true);
}

}  // namespace donner::svg
