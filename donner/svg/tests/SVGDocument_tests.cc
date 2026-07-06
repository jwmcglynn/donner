#include "donner/svg/SVGDocument.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Transform.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGStyleElement.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/paint/ClipPathComponent.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/components/text/TextRootComponent.h"
#include "donner/svg/parser/SVGParser.h"

using testing::Eq;
using testing::Optional;

namespace donner::svg {

namespace {

/// Helper to parse an SVG string and return the resulting document.
SVGDocument ParseSVG(std::string_view input) {
  parser::SVGParser::Options options;
  options.disableUserAttributes = false;

  ParseWarningSink disabled = ParseWarningSink::Disabled();
  auto maybeResult = parser::SVGParser::ParseSVG(input, disabled, options);
  EXPECT_THAT(maybeResult, NoParseError());
  return std::move(maybeResult).result();
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

TEST(SVGDocument, CanvasSize) {
  SVGDocument document;
  EXPECT_EQ(document.canvasSize(), Vector2i(512, 512));

  document.setCanvasSize(100, 200);
  EXPECT_EQ(document.canvasSize(), Vector2i(100, 200));

  document.useAutomaticCanvasSize();
  EXPECT_EQ(document.canvasSize(), Vector2i(512, 512));
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
  EXPECT_TRUE(stylesheet.sourceMap.empty());
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

TEST(SVGDocument, ApplySourceEditReportsOpeningTagRenameUnsupported) {
  auto document = ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><custom id="shape"/></svg>)");
  SVGElement element = document.querySelector("#shape").value();
  ASSERT_EQ(element.type(), ElementType::Unknown);
  const std::size_t openNameOffset = document.source().find("custom");
  ASSERT_NE(openNameOffset, std::string_view::npos);

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range =
          SourceRange{FileOffset::Offset(openNameOffset), FileOffset::Offset(openNameOffset + 6)},
      .replacement = "rect",
      .sourceVersion = document.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  ASSERT_TRUE(result.diagnostic.has_value());
  EXPECT_THAT(result.diagnostic->reason,
              testing::HasSubstr("Opening tag element rename is not implemented"));
  ASSERT_THAT(document.querySelector("#shape"), Optional(ElementIdEq("shape")));
  EXPECT_EQ(document.querySelector("#shape")->type(), ElementType::Unknown);
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

}  // namespace donner::svg
