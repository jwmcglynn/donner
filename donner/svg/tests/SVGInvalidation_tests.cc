#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGElement.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGStyleElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/SVGUnknownElement.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/shape/PathComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::svg {

namespace {

using components::DirtyFlagsComponent;
using components::RenderTreeState;

class SVGInvalidationTests : public testing::Test {
protected:
  SVGInvalidationTests() { document_.setCanvasSize(800, 600); }

  SVGDocument parseSVG(std::string_view input) {
    parser::SVGParser::Options options;
    options.parseAsInlineSVG = true;

    ParseWarningSink disabledSink = ParseWarningSink::Disabled();
    auto maybeResult = parser::SVGParser::ParseSVG(input, disabledSink, options);
    EXPECT_THAT(maybeResult, NoParseError());
    return std::move(maybeResult).result();
  }

  /// Helper to clear dirty flags on an element.
  static void clearDirty(SVGElement element) {
    element.entityHandle().remove<DirtyFlagsComponent>();
  }

  /// Helper to check whether an element has specific dirty flags set.
  static bool hasDirtyFlags(SVGElement element, uint16_t flags) {
    const auto* dirty = element.entityHandle().try_get<DirtyFlagsComponent>();
    return dirty != nullptr && dirty->test(flags);
  }

  /// Helper to check whether an element has any dirty flags at all.
  static bool isDirty(SVGElement element) {
    const auto* dirty = element.entityHandle().try_get<DirtyFlagsComponent>();
    return dirty != nullptr && dirty->flags != DirtyFlagsComponent::None;
  }

  /// Simulate a completed first render by setting RenderTreeState appropriately.
  static void simulateRenderComplete(SVGDocument& doc) {
    auto& registry = doc.registry();
    if (!registry.ctx().contains<RenderTreeState>()) {
      registry.ctx().emplace<RenderTreeState>();
    }
    auto& renderState = registry.ctx().get<RenderTreeState>();
    renderState.hasBeenBuilt = true;
    renderState.needsFullRebuild = false;
    renderState.needsFullStyleRecompute = false;

    // Clear all per-entity dirty flags.
    auto view = registry.view<DirtyFlagsComponent>();
    for (auto entity : view) {
      registry.remove<DirtyFlagsComponent>(entity);
    }
  }

  static void expectNoDiagnostic(const xml::ApplySourceEditResult& result) {
    if (result.diagnostic.has_value()) {
      ADD_FAILURE() << *result.diagnostic;
    }
  }

  SVGDocument document_;
};

// ---------------------------------------------------------------------------
// setStyle
// ---------------------------------------------------------------------------

TEST_F(SVGInvalidationTests, SetStyleMarksDirtyWithStyleCascade) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" />
    </svg>
  )");
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  target->setStyle("fill: red");

  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Style));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Paint));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::RenderInstance));
  // StyleCascade is Style | Paint | Filter | RenderInstance.
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::StyleCascade));
}

TEST_F(SVGInvalidationTests, SetStyleDoesNotMarkTransformDirty) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" />
    </svg>
  )");
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  target->setStyle("fill: blue");

  // A fill change should not set Transform or Layout flags.
  EXPECT_FALSE(hasDirtyFlags(*target, DirtyFlagsComponent::Transform));
  EXPECT_FALSE(hasDirtyFlags(*target, DirtyFlagsComponent::Layout));
}

// ---------------------------------------------------------------------------
// setClassName
// ---------------------------------------------------------------------------

TEST_F(SVGInvalidationTests, SetClassNameMarksDirtyWithStyleCascade) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" />
    </svg>
  )");
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  target->setClassName("highlight");

  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::StyleCascade));
}

TEST_F(SVGInvalidationTests, SetClassNameRequestsFullStyleRecompute) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" />
    </svg>
  )");
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  target->setClassName("highlight");

  auto& renderState = doc.registry().ctx().get<RenderTreeState>();
  EXPECT_TRUE(renderState.needsFullStyleRecompute);
}

// ---------------------------------------------------------------------------
// setAttribute (presentation attribute and generic attribute)
// ---------------------------------------------------------------------------

TEST_F(SVGInvalidationTests, SetAttributePresentationAttributeMarksDirty) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" />
    </svg>
  )");
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  target->setAttribute("fill", "green");

  // Presentation attributes mark StyleCascade and Shape dirty.
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Style));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Shape));
}

TEST_F(SVGInvalidationTests, SetAttributeTransformMarksDirtyTransform) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" />
    </svg>
  )");
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  target->setAttribute("transform", "translate(10 20)");

  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Transform));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::WorldTransform));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::RenderInstance));
}

TEST_F(SVGInvalidationTests, SetAttributeGenericRequestsFullStyleRecompute) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" />
    </svg>
  )");
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  // A non-presentation, non-special attribute still requests full style recompute because
  // it may affect CSS attribute selectors.
  target->setAttribute("data-custom", "value");

  auto& renderState = doc.registry().ctx().get<RenderTreeState>();
  EXPECT_TRUE(renderState.needsFullStyleRecompute);
}

TEST_F(SVGInvalidationTests, SourceEditAttributeValueUpdatesPresentationAttribute) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" fill="red" />
    </svg>
  )";
  const std::size_t fillValueOffset = input.find("red");
  ASSERT_NE(fillValueOffset, std::string::npos);

  auto doc = parseSVG(input);
  ASSERT_TRUE(doc.hasSourceStore());
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  xml::ApplySourceEditResult result = doc.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(fillValueOffset), FileOffset::Offset(fillValueOffset + 3)},
      .replacement = "green",
      .sourceVersion = doc.sourceVersion(),
  });

  expectNoDiagnostic(result);
  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, xml::ReparseScope::AttributeValue);
  ASSERT_THAT(result.mutations, testing::SizeIs(1));
  EXPECT_EQ(result.mutations[0].kind, xml::XMLMutation::Kind::AttributeSet);

  std::optional<RcString> fill = target->getAttribute("fill");
  ASSERT_TRUE(fill.has_value());
  EXPECT_EQ(*fill, RcString("green"));
  EXPECT_NE(doc.source().find(R"(fill="green")"), std::string_view::npos);
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Style));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Shape));
}

TEST_F(SVGInvalidationTests, SourceEditOpeningTagRemovalClearsPresentationAttribute) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" fill="red" />
    </svg>
  )";
  const std::string attributeSource = R"( fill="red")";
  const std::size_t attributeOffset = input.find(attributeSource);
  ASSERT_NE(attributeOffset, std::string::npos);

  auto doc = parseSVG(input);
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  xml::ApplySourceEditResult result = doc.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(attributeOffset),
                FileOffset::Offset(attributeOffset + attributeSource.size())},
      .replacement = "",
      .sourceVersion = doc.sourceVersion(),
  });

  expectNoDiagnostic(result);
  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, xml::ReparseScope::OpeningTag);
  ASSERT_THAT(result.mutations, testing::SizeIs(1));
  EXPECT_EQ(result.mutations[0].kind, xml::XMLMutation::Kind::AttributeRemoved);

  EXPECT_FALSE(target->getAttribute("fill").has_value());
  EXPECT_EQ(doc.source().find(R"(fill="red")"), std::string_view::npos);
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Style));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Shape));
}

TEST_F(SVGInvalidationTests, SetAttributeUpdatesSourceThroughXMLDocument) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" fill="red" />
    </svg>
  )";

  auto doc = parseSVG(input);
  ASSERT_TRUE(doc.hasSourceStore());
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  target->setAttribute("fill", "green");

  std::optional<RcString> fill = target->getAttribute("fill");
  ASSERT_TRUE(fill.has_value());
  EXPECT_EQ(*fill, RcString("green"));
  EXPECT_NE(doc.source().find(R"(fill="green")"), std::string_view::npos);
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Style));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Shape));
}

TEST_F(SVGInvalidationTests, SetAttributeInsertsSourceAttributeThroughXMLDocument) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" />
    </svg>
  )";

  auto doc = parseSVG(input);
  ASSERT_TRUE(doc.hasSourceStore());

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  target->setAttribute("data-label", "blue & white");

  std::optional<RcString> dataLabel = target->getAttribute("data-label");
  ASSERT_TRUE(dataLabel.has_value());
  EXPECT_EQ(*dataLabel, RcString("blue & white"));
  EXPECT_NE(doc.source().find(R"(data-label="blue &amp; white")"), std::string_view::npos);
}

TEST_F(SVGInvalidationTests, RemoveAttributeUpdatesSourceThroughXMLDocument) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" fill="red" />
    </svg>
  )";

  auto doc = parseSVG(input);
  ASSERT_TRUE(doc.hasSourceStore());
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  target->removeAttribute("fill");

  EXPECT_FALSE(target->getAttribute("fill").has_value());
  EXPECT_EQ(doc.source().find(R"(fill="red")"), std::string_view::npos);
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Style));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Shape));
}

TEST_F(SVGInvalidationTests, RemoveElementUpdatesSourceThroughXMLDocument) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent"><rect id="target" width="10" height="10" /><circle id="sibling" /></g>
    </svg>
  )";

  auto doc = parseSVG(input);
  ASSERT_TRUE(doc.hasSourceStore());
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  ASSERT_TRUE(parent.has_value());
  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  target->remove();

  EXPECT_EQ(doc.source().find(R"(<rect id="target")"), std::string_view::npos);
  EXPECT_FALSE(doc.querySelector("#target").has_value());
  ASSERT_TRUE(parent->firstChild().has_value());
  EXPECT_THAT(parent->firstChild()->getAttribute("id"), testing::Optional(RcString("sibling")));
  EXPECT_EQ(target->parentElement(), std::nullopt);
  EXPECT_TRUE(hasDirtyFlags(*parent, DirtyFlagsComponent::All));
}

TEST_F(SVGInvalidationTests, RemoveElementConsumesNodeRemovedMutation) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent"><rect id="target" width="10" height="10" /><circle id="sibling" /></g>
    </svg>
  )";

  auto doc = parseSVG(input);
  ASSERT_TRUE(doc.hasSourceStore());
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  ASSERT_TRUE(parent.has_value());
  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  xml::ApplySourceEditResult result = doc.removeElement(*target);

  expectNoDiagnostic(result);
  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, xml::ReparseScope::ElementSubtree);
  ASSERT_THAT(result.mutations, testing::SizeIs(1));
  EXPECT_EQ(result.mutations[0].kind, xml::XMLMutation::Kind::NodeRemoved);

  EXPECT_EQ(doc.source().find(R"(<rect id="target")"), std::string_view::npos);
  EXPECT_FALSE(doc.querySelector("#target").has_value());
  ASSERT_TRUE(parent->firstChild().has_value());
  EXPECT_THAT(parent->firstChild()->getAttribute("id"), testing::Optional(RcString("sibling")));
  EXPECT_EQ(target->parentElement(), std::nullopt);
  EXPECT_TRUE(hasDirtyFlags(*parent, DirtyFlagsComponent::All));
}

TEST_F(SVGInvalidationTests, SourceEditInvalidPresentationAttributeKeepsLastValidStyle) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" fill="red" />
    </svg>
  )";
  const std::size_t fillValueOffset = input.find("red");
  ASSERT_NE(fillValueOffset, std::string::npos);

  auto doc = parseSVG(input);
  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EXPECT_THAT(
      target->getComputedStyle().fill.get(),
      testing::Optional(PaintServer(PaintServer::Solid(css::Color(css::RGBA(0xFF, 0, 0, 0xFF))))));
  simulateRenderComplete(doc);

  xml::ApplySourceEditResult result = doc.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(fillValueOffset), FileOffset::Offset(fillValueOffset + 3)},
      .replacement = "#12",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  ASSERT_TRUE(result.diagnostic.has_value());
  EXPECT_EQ(result.scope, xml::ReparseScope::AttributeValue);

  std::optional<RcString> fill = target->getAttribute("fill");
  ASSERT_TRUE(fill.has_value());
  EXPECT_EQ(*fill, RcString("#12"));
  EXPECT_NE(doc.source().find(R"(fill="#12")"), std::string_view::npos);
  EXPECT_THAT(
      target->getComputedStyle().fill.get(),
      testing::Optional(PaintServer(PaintServer::Solid(css::Color(css::RGBA(0xFF, 0, 0, 0xFF))))));

  const std::size_t invalidValueOffset = doc.source().find("#12");
  ASSERT_NE(invalidValueOffset, std::string_view::npos);
  xml::ApplySourceEditResult recoveryResult = doc.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(invalidValueOffset), FileOffset::Offset(invalidValueOffset + 3)},
      .replacement = "blue",
      .sourceVersion = doc.sourceVersion(),
  });

  expectNoDiagnostic(recoveryResult);
  EXPECT_TRUE(recoveryResult.applied);
  EXPECT_EQ(recoveryResult.scope, xml::ReparseScope::AttributeValue);

  fill = target->getAttribute("fill");
  ASSERT_TRUE(fill.has_value());
  EXPECT_EQ(*fill, RcString("blue"));
  EXPECT_NE(doc.source().find(R"(fill="blue")"), std::string_view::npos);
  EXPECT_THAT(
      target->getComputedStyle().fill.get(),
      testing::Optional(PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 0, 0xFF, 0xFF))))));
}

TEST_F(SVGInvalidationTests, SourceEditPathDataUpdatesGeometryAttribute) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <path id="target" d="M 0 0 L 10 10" />
    </svg>
  )";
  const std::string originalPath = "M 0 0 L 10 10";
  const std::string updatedPath = "M 1 2 L 3 4";
  const std::size_t pathValueOffset = input.find(originalPath);
  ASSERT_NE(pathValueOffset, std::string::npos);

  auto doc = parseSVG(input);
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  xml::ApplySourceEditResult result = doc.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(pathValueOffset),
                FileOffset::Offset(pathValueOffset + originalPath.size())},
      .replacement = updatedPath,
      .sourceVersion = doc.sourceVersion(),
  });

  expectNoDiagnostic(result);
  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, xml::ReparseScope::AttributeValue);

  std::optional<RcString> d = target->getAttribute("d");
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(*d, RcString(updatedPath));

  const auto* path = target->entityHandle().try_get<components::PathComponent>();
  ASSERT_NE(path, nullptr);
  EXPECT_EQ(path->d.get().value(), RcString(updatedPath));
  EXPECT_NE(doc.source().find(R"(d="M 1 2 L 3 4")"), std::string_view::npos);
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Style));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Shape));
}

TEST_F(SVGInvalidationTests, SourceEditElementSubtreeAttributeRemovalClearsPathProjection) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="layer"><path id="target" d="M 0 0 L 10 10" /></g>
    </svg>
  )";
  const std::string replacement = R"(<path id="target" />)";
  const std::size_t pathOffset = input.find(R"(<path id="target")");
  ASSERT_NE(pathOffset, std::string::npos);
  const std::size_t pathEnd = input.find("/>", pathOffset);
  ASSERT_NE(pathEnd, std::string::npos);

  auto doc = parseSVG(input);
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const auto* path = target->entityHandle().try_get<components::PathComponent>();
  ASSERT_NE(path, nullptr);
  EXPECT_EQ(path->d.get().value(), RcString("M 0 0 L 10 10"));

  xml::ApplySourceEditResult result = doc.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(pathOffset), FileOffset::Offset(pathEnd + 2)},
      .replacement = replacement,
      .sourceVersion = doc.sourceVersion(),
  });

  expectNoDiagnostic(result);
  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, xml::ReparseScope::ElementSubtree);
  ASSERT_GE(result.mutations.size(), 2u);
  EXPECT_EQ(result.mutations[0].kind, xml::XMLMutation::Kind::SubtreeReplaced);
  bool sawPathDataRemoved = false;
  for (const xml::XMLMutation& mutation : result.mutations) {
    sawPathDataRemoved |= mutation.kind == xml::XMLMutation::Kind::AttributeRemoved &&
                          mutation.attributeName == xml::XMLQualifiedName("d");
  }
  EXPECT_TRUE(sawPathDataRemoved);

  EXPECT_FALSE(target->getAttribute("d").has_value());
  EXPECT_TRUE(path->d.get().value().empty());
  EXPECT_EQ(doc.source().find(R"(d="M 0 0 L 10 10")"), std::string_view::npos);
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Shape));
}

TEST_F(SVGInvalidationTests, SourceEditFeColorMatrixValuesRemovalClearsParsedValues) {
  const std::string input = R"SVG(
    <svg xmlns="http://www.w3.org/2000/svg">
      <defs>
        <filter id="filter">
          <feColorMatrix id="target" type="saturate" values="0.5" />
        </filter>
      </defs>
      <rect width="10" height="10" filter="url(#filter)" />
    </svg>
  )SVG";
  const std::string attributeSource = R"( values="0.5")";
  const std::size_t attributeOffset = input.find(attributeSource);
  ASSERT_NE(attributeOffset, std::string::npos);

  auto doc = parseSVG(input);
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const auto* colorMatrix = target->entityHandle().try_get<components::FEColorMatrixComponent>();
  ASSERT_NE(colorMatrix, nullptr);
  EXPECT_THAT(colorMatrix->values, testing::ElementsAre(0.5));

  xml::ApplySourceEditResult result = doc.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(attributeOffset),
                FileOffset::Offset(attributeOffset + attributeSource.size())},
      .replacement = "",
      .sourceVersion = doc.sourceVersion(),
  });

  expectNoDiagnostic(result);
  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, xml::ReparseScope::OpeningTag);

  EXPECT_FALSE(target->getAttribute("values").has_value());
  EXPECT_TRUE(colorMatrix->values.empty());
  EXPECT_EQ(doc.source().find(R"(values="0.5")"), std::string_view::npos);
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Filter));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::RenderInstance));
}

TEST_F(SVGInvalidationTests, SourceEditTextNodeUpdatesTextContent) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <text id="target">hello</text>
    </svg>
  )";
  const std::size_t textOffset = input.find("hello");
  ASSERT_NE(textOffset, std::string::npos);

  auto doc = parseSVG(input);
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->cast<SVGTextElement>().textContent(), RcString("hello"));

  xml::ApplySourceEditResult result = doc.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(textOffset), FileOffset::Offset(textOffset + 5)},
      .replacement = "world",
      .sourceVersion = doc.sourceVersion(),
  });

  expectNoDiagnostic(result);
  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, xml::ReparseScope::TextNode);
  ASSERT_THAT(result.mutations, testing::SizeIs(1));
  EXPECT_EQ(result.mutations[0].kind, xml::XMLMutation::Kind::NodeValueChanged);

  EXPECT_EQ(target->cast<SVGTextElement>().textContent(), RcString("world"));
  EXPECT_NE(doc.source().find(">world<"), std::string_view::npos);
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::TextGeometry));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::RenderInstance));
}

TEST_F(SVGInvalidationTests, SourceEditTextDirtyDiagnosticClearsThroughMutation) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <text id="target">a&#65;b</text>
    </svg>
  )";
  const std::size_t entityOffset = input.find("&#65;");
  ASSERT_NE(entityOffset, std::string::npos);

  auto doc = parseSVG(input);
  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->cast<SVGTextElement>().textContent(), RcString("aAb"));

  xml::ApplySourceEditResult dirtyResult = doc.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(entityOffset + 4), FileOffset::Offset(entityOffset + 5)},
      .replacement = "",
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(dirtyResult.applied);
  EXPECT_EQ(dirtyResult.scope, xml::ReparseScope::TextNode);
  ASSERT_TRUE(dirtyResult.diagnostic.has_value());
  ASSERT_THAT(dirtyResult.mutations, testing::SizeIs(1));
  EXPECT_EQ(dirtyResult.mutations[0].kind, xml::XMLMutation::Kind::SourceDiagnosticChanged);
  EXPECT_TRUE(dirtyResult.mutations[0].diagnostic.has_value());
  EXPECT_EQ(target->cast<SVGTextElement>().textContent(), RcString("aAb"));

  xml::ApplySourceEditResult recoveryResult = doc.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(entityOffset + 4), FileOffset::Offset(entityOffset + 4)},
      .replacement = ";",
      .sourceVersion = doc.sourceVersion(),
  });

  expectNoDiagnostic(recoveryResult);
  EXPECT_TRUE(recoveryResult.applied);
  EXPECT_EQ(recoveryResult.scope, xml::ReparseScope::TextNode);
  ASSERT_THAT(recoveryResult.mutations, testing::SizeIs(2));
  EXPECT_EQ(recoveryResult.mutations[0].kind, xml::XMLMutation::Kind::NodeValueChanged);
  EXPECT_EQ(recoveryResult.mutations[1].kind, xml::XMLMutation::Kind::SourceDiagnosticChanged);
  EXPECT_EQ(recoveryResult.mutations[1].diagnostic, std::nullopt);
  EXPECT_EQ(target->cast<SVGTextElement>().textContent(), RcString("aAb"));
}

TEST_F(SVGInvalidationTests, SourceEditElementSubtreeInsertedChildProjectsToSVG) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="layer"><rect id="existing" width="10" height="10" /></g>
    </svg>
  )";
  const std::string inserted = R"(<circle id="inserted" cx="5" cy="6" r="3" fill="blue" />)";
  const std::size_t insertOffset = input.find("</g>");
  ASSERT_NE(insertOffset, std::string::npos);

  auto doc = parseSVG(input);
  simulateRenderComplete(doc);

  auto layer = doc.querySelector("#layer");
  ASSERT_TRUE(layer.has_value());

  xml::ApplySourceEditResult result = doc.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(insertOffset), FileOffset::Offset(insertOffset)},
      .replacement = inserted,
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, xml::ReparseScope::ElementSubtree);
  ASSERT_FALSE(result.diagnostic.has_value()) << *result.diagnostic;
  ASSERT_THAT(result.mutations, testing::SizeIs(2));
  EXPECT_EQ(result.mutations[0].kind, xml::XMLMutation::Kind::SubtreeReplaced);
  EXPECT_EQ(result.mutations[1].kind, xml::XMLMutation::Kind::NodeInserted);

  auto insertedElement = doc.querySelector("#inserted");
  ASSERT_TRUE(insertedElement.has_value());
  EXPECT_THAT(insertedElement->getAttribute("fill"), testing::Optional(RcString("blue")));
  EXPECT_TRUE(hasDirtyFlags(*layer, DirtyFlagsComponent::All));

  auto& renderState = doc.registry().ctx().get<RenderTreeState>();
  EXPECT_TRUE(renderState.needsFullRebuild);
  EXPECT_TRUE(renderState.needsFullStyleRecompute);
}

TEST_F(SVGInvalidationTests, SourceEditElementSubtreeInsertedTextProjectsTextContent) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="layer"><rect id="existing" width="10" height="10" /></g>
    </svg>
  )";
  const std::string inserted = R"(<text id="label" x="1" y="2">hello</text>)";
  const std::size_t insertOffset = input.find("</g>");
  ASSERT_NE(insertOffset, std::string::npos);

  auto doc = parseSVG(input);
  simulateRenderComplete(doc);

  xml::ApplySourceEditResult result = doc.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(insertOffset), FileOffset::Offset(insertOffset)},
      .replacement = inserted,
      .sourceVersion = doc.sourceVersion(),
  });

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, xml::ReparseScope::ElementSubtree);
  ASSERT_FALSE(result.diagnostic.has_value()) << *result.diagnostic;

  auto label = doc.querySelector("#label");
  ASSERT_TRUE(label.has_value());
  ASSERT_TRUE(label->isa<SVGTextElement>());
  EXPECT_EQ(label->cast<SVGTextElement>().textContent(), RcString("hello"));
  EXPECT_TRUE(hasDirtyFlags(*label, DirtyFlagsComponent::All));
}

TEST_F(SVGInvalidationTests, SourceEditElementSubtreeInsertedStyleProjectsStylesheet) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="layer"><rect id="target" width="10" height="10" /></g>
    </svg>
  )";
  const std::string inserted = R"(<style>rect { fill: blue; }</style>)";
  const std::size_t insertOffset = input.find(R"(<g id="layer">)");
  ASSERT_NE(insertOffset, std::string::npos);

  auto doc = parseSVG(input);
  simulateRenderComplete(doc);

  xml::ApplySourceEditResult result = doc.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(insertOffset), FileOffset::Offset(insertOffset)},
      .replacement = inserted,
      .sourceVersion = doc.sourceVersion(),
  });

  expectNoDiagnostic(result);
  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, xml::ReparseScope::ElementSubtree);

  std::optional<SVGElement> style = doc.querySelector("style");
  ASSERT_TRUE(style.has_value());
  EXPECT_TRUE(style->isa<SVGStyleElement>());

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EXPECT_THAT(
      target->getComputedStyle().fill.get(),
      testing::Optional(PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 0, 0xFF, 0xFF))))));
}

TEST_F(SVGInvalidationTests, SourceEditStyleTextNodeUpdatesStylesheet) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>rect { fill: red; }</style>
      <rect id="target" width="10" height="10" />
    </svg>
  )";
  const std::size_t colorOffset = input.find("red");
  ASSERT_NE(colorOffset, std::string::npos);

  auto doc = parseSVG(input);
  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EXPECT_THAT(
      target->getComputedStyle().fill.get(),
      testing::Optional(PaintServer(PaintServer::Solid(css::Color(css::RGBA(0xFF, 0, 0, 0xFF))))));
  simulateRenderComplete(doc);

  xml::ApplySourceEditResult result = doc.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(colorOffset), FileOffset::Offset(colorOffset + 3)},
      .replacement = "blue",
      .sourceVersion = doc.sourceVersion(),
  });

  expectNoDiagnostic(result);
  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, xml::ReparseScope::TextNode);
  ASSERT_THAT(result.mutations, testing::SizeIs(1));
  EXPECT_EQ(result.mutations[0].kind, xml::XMLMutation::Kind::NodeValueChanged);

  EXPECT_THAT(
      target->getComputedStyle().fill.get(),
      testing::Optional(PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 0, 0xFF, 0xFF))))));
}

// ---------------------------------------------------------------------------
// appendChild
// ---------------------------------------------------------------------------

TEST_F(SVGInvalidationTests, AppendChildMarksParentDirtyAll) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent"></g>
    </svg>
  )");
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  ASSERT_TRUE(parent.has_value());

  auto newChild = SVGRectElement::Create(doc);
  parent->appendChild(newChild);

  EXPECT_TRUE(hasDirtyFlags(*parent, DirtyFlagsComponent::All));
  EXPECT_TRUE(hasDirtyFlags(newChild, DirtyFlagsComponent::All));
}

TEST_F(SVGInvalidationTests, AppendChildSetsNeedsFullRebuild) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent"></g>
    </svg>
  )");
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  ASSERT_TRUE(parent.has_value());

  auto newChild = SVGRectElement::Create(doc);
  parent->appendChild(newChild);

  auto& renderState = doc.registry().ctx().get<RenderTreeState>();
  EXPECT_TRUE(renderState.needsFullRebuild);
}

TEST_F(SVGInvalidationTests, InsertElementAppendsSourceBackedProgrammaticChild) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent"></g>
    </svg>
  )";

  auto doc = parseSVG(input);
  ASSERT_TRUE(doc.hasSourceStore());
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  ASSERT_TRUE(parent.has_value());
  auto newChild = SVGRectElement::Create(doc);
  newChild.setAttribute("id", "inserted");
  newChild.setAttribute("width", "10");

  xml::ApplySourceEditResult result = doc.insertElement(*parent, newChild);

  expectNoDiagnostic(result);
  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.scope, xml::ReparseScope::ElementSubtree);
  ASSERT_THAT(result.mutations, testing::SizeIs(1));
  EXPECT_EQ(result.mutations[0].kind, xml::XMLMutation::Kind::NodeInserted);

  EXPECT_NE(doc.source().find(R"(<rect id="inserted" width="10"/>)"), std::string_view::npos);
  auto inserted = doc.querySelector("#inserted");
  ASSERT_TRUE(inserted.has_value());
  EXPECT_EQ(*inserted, newChild);
  EXPECT_EQ(parent->firstChild(), inserted);
  EXPECT_TRUE(hasDirtyFlags(*parent, DirtyFlagsComponent::All));
  EXPECT_TRUE(hasDirtyFlags(newChild, DirtyFlagsComponent::All));
}

TEST_F(SVGInvalidationTests, SetElementTextContentRewritesSourceText) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <text id="t" x="10" y="20">old</text>
    </svg>
  )";

  auto doc = parseSVG(input);
  ASSERT_TRUE(doc.hasSourceStore());
  simulateRenderComplete(doc);

  auto text = doc.querySelector("#t");
  ASSERT_TRUE(text.has_value());

  xml::ApplySourceEditResult result = doc.setElementTextContent(*text, "updated");

  expectNoDiagnostic(result);
  EXPECT_TRUE(result.applied);
  EXPECT_NE(doc.source().find(">updated</text>"), std::string_view::npos)
      << "source: " << doc.source();
  EXPECT_EQ(doc.source().find(">old<"), std::string_view::npos);

  // Clearing the content removes the text node from the source.
  xml::ApplySourceEditResult cleared = doc.setElementTextContent(*text, "");
  expectNoDiagnostic(cleared);
  EXPECT_TRUE(cleared.applied);
  EXPECT_EQ(doc.source().find("updated"), std::string_view::npos) << "source: " << doc.source();
}

TEST_F(SVGInvalidationTests, AppendChildUpdatesSourceThroughXMLDocument) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent"></g>
    </svg>
  )";

  auto doc = parseSVG(input);
  ASSERT_TRUE(doc.hasSourceStore());
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  ASSERT_TRUE(parent.has_value());
  auto newChild = SVGRectElement::Create(doc);
  newChild.setAttribute("id", "inserted");

  parent->appendChild(newChild);

  EXPECT_NE(doc.source().find(R"(<rect id="inserted"/>)"), std::string_view::npos);
  auto inserted = doc.querySelector("#inserted");
  ASSERT_TRUE(inserted.has_value());
  EXPECT_EQ(*inserted, newChild);
  EXPECT_EQ(parent->firstChild(), inserted);
  EXPECT_TRUE(hasDirtyFlags(*parent, DirtyFlagsComponent::All));
  EXPECT_TRUE(hasDirtyFlags(newChild, DirtyFlagsComponent::All));
}

TEST_F(SVGInvalidationTests, InsertBeforeUpdatesSourceThroughXMLDocument) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent"><circle id="sibling" /></g>
    </svg>
  )";

  auto doc = parseSVG(input);
  ASSERT_TRUE(doc.hasSourceStore());
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  ASSERT_TRUE(parent.has_value());
  auto sibling = doc.querySelector("#sibling");
  ASSERT_TRUE(sibling.has_value());
  auto newChild = SVGRectElement::Create(doc);
  newChild.setAttribute("id", "inserted");

  parent->insertBefore(newChild, sibling);

  const std::size_t insertedOffset = doc.source().find(R"(<rect id="inserted"/>)");
  const std::size_t siblingOffset = doc.source().find(R"(<circle id="sibling")");
  ASSERT_NE(insertedOffset, std::string_view::npos);
  ASSERT_NE(siblingOffset, std::string_view::npos);
  EXPECT_LT(insertedOffset, siblingOffset);

  auto inserted = doc.querySelector("#inserted");
  ASSERT_TRUE(inserted.has_value());
  EXPECT_EQ(*inserted, newChild);
  EXPECT_EQ(parent->firstChild(), inserted);
  ASSERT_TRUE(inserted->nextSibling().has_value());
  EXPECT_EQ(*inserted->nextSibling(), *sibling);
}

TEST_F(SVGInvalidationTests, AppendChildExpandsSelfClosingSourceBackedParent) {
  const std::string input = R"(<svg xmlns="http://www.w3.org/2000/svg"><g id="parent"/></svg>)";

  auto doc = parseSVG(input);
  ASSERT_TRUE(doc.hasSourceStore());
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  ASSERT_TRUE(parent.has_value());
  auto newChild = SVGRectElement::Create(doc);
  newChild.setAttribute("id", "inserted");

  parent->appendChild(newChild);

  EXPECT_NE(doc.source().find(R"(<g id="parent"><rect id="inserted"/></g>)"),
            std::string_view::npos);
  auto inserted = doc.querySelector("#inserted");
  ASSERT_TRUE(inserted.has_value());
  EXPECT_EQ(*inserted, newChild);
  EXPECT_EQ(parent->firstChild(), inserted);
  EXPECT_TRUE(hasDirtyFlags(*parent, DirtyFlagsComponent::All));
  EXPECT_TRUE(hasDirtyFlags(newChild, DirtyFlagsComponent::All));
}

TEST_F(SVGInvalidationTests, ReplaceChildUpdatesSourceThroughXMLDocument) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent"><circle id="old" /></g>
    </svg>
  )";

  auto doc = parseSVG(input);
  ASSERT_TRUE(doc.hasSourceStore());
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  ASSERT_TRUE(parent.has_value());
  auto oldChild = doc.querySelector("#old");
  ASSERT_TRUE(oldChild.has_value());
  auto newChild = SVGRectElement::Create(doc);
  newChild.setAttribute("id", "replacement");

  parent->replaceChild(newChild, *oldChild);

  EXPECT_EQ(doc.source().find(R"(<circle id="old")"), std::string_view::npos);
  EXPECT_NE(doc.source().find(R"(<rect id="replacement"/>)"), std::string_view::npos);
  EXPECT_FALSE(doc.querySelector("#old").has_value());
  auto replacement = doc.querySelector("#replacement");
  ASSERT_TRUE(replacement.has_value());
  EXPECT_EQ(*replacement, newChild);
  EXPECT_EQ(parent->firstChild(), replacement);
  EXPECT_EQ(oldChild->parentElement(), std::nullopt);
  EXPECT_TRUE(hasDirtyFlags(*parent, DirtyFlagsComponent::All));
  EXPECT_TRUE(hasDirtyFlags(newChild, DirtyFlagsComponent::All));
}

TEST_F(SVGInvalidationTests, ReplaceChildMovesExistingSourceBackedReplacement) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent"><circle id="old" /><rect id="replacement" /><path id="after" /></g>
    </svg>
  )";

  auto doc = parseSVG(input);
  ASSERT_TRUE(doc.hasSourceStore());
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  ASSERT_TRUE(parent.has_value());
  auto oldChild = doc.querySelector("#old");
  ASSERT_TRUE(oldChild.has_value());
  auto replacement = doc.querySelector("#replacement");
  ASSERT_TRUE(replacement.has_value());
  auto after = doc.querySelector("#after");
  ASSERT_TRUE(after.has_value());

  parent->replaceChild(*replacement, *oldChild);

  const std::string_view source = doc.source();
  EXPECT_EQ(source.find(R"(<circle id="old")"), std::string_view::npos);
  const std::size_t replacementOffset = source.find(R"(<rect id="replacement")");
  const std::size_t afterOffset = source.find(R"(<path id="after")");
  ASSERT_NE(replacementOffset, std::string_view::npos);
  ASSERT_NE(afterOffset, std::string_view::npos);
  EXPECT_LT(replacementOffset, afterOffset);

  EXPECT_FALSE(doc.querySelector("#old").has_value());
  EXPECT_EQ(parent->firstChild(), replacement);
  EXPECT_EQ(replacement->nextSibling(), after);
  EXPECT_EQ(oldChild->parentElement(), std::nullopt);
  EXPECT_TRUE(hasDirtyFlags(*parent, DirtyFlagsComponent::All));
  EXPECT_TRUE(hasDirtyFlags(*replacement, DirtyFlagsComponent::All));
}

TEST_F(SVGInvalidationTests, AppendChildMovesExistingSourceBackedChildToEnd) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent"><rect id="a" /><circle id="b" /></g>
    </svg>
  )";

  auto doc = parseSVG(input);
  ASSERT_TRUE(doc.hasSourceStore());
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  ASSERT_TRUE(parent.has_value());
  auto rect = doc.querySelector("#a");
  ASSERT_TRUE(rect.has_value());
  auto circle = doc.querySelector("#b");
  ASSERT_TRUE(circle.has_value());

  parent->appendChild(*rect);

  const std::size_t circleOffset = doc.source().find(R"(<circle id="b")");
  const std::size_t rectOffset = doc.source().find(R"(<rect id="a")");
  ASSERT_NE(circleOffset, std::string_view::npos);
  ASSERT_NE(rectOffset, std::string_view::npos);
  EXPECT_LT(circleOffset, rectOffset);

  ASSERT_TRUE(parent->firstChild().has_value());
  EXPECT_EQ(*parent->firstChild(), *circle);
  ASSERT_TRUE(circle->nextSibling().has_value());
  EXPECT_EQ(*circle->nextSibling(), *rect);
  EXPECT_TRUE(hasDirtyFlags(*parent, DirtyFlagsComponent::All));
}

TEST_F(SVGInvalidationTests, InsertBeforeMovesExistingSourceBackedChildBeforeReference) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent"><rect id="a" /><circle id="b" /></g>
    </svg>
  )";

  auto doc = parseSVG(input);
  ASSERT_TRUE(doc.hasSourceStore());
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  ASSERT_TRUE(parent.has_value());
  auto rect = doc.querySelector("#a");
  ASSERT_TRUE(rect.has_value());
  auto circle = doc.querySelector("#b");
  ASSERT_TRUE(circle.has_value());

  parent->insertBefore(*circle, rect);

  const std::size_t circleOffset = doc.source().find(R"(<circle id="b")");
  const std::size_t rectOffset = doc.source().find(R"(<rect id="a")");
  ASSERT_NE(circleOffset, std::string_view::npos);
  ASSERT_NE(rectOffset, std::string_view::npos);
  EXPECT_LT(circleOffset, rectOffset);

  ASSERT_TRUE(parent->firstChild().has_value());
  EXPECT_EQ(*parent->firstChild(), *circle);
  ASSERT_TRUE(circle->nextSibling().has_value());
  EXPECT_EQ(*circle->nextSibling(), *rect);
  EXPECT_TRUE(hasDirtyFlags(*parent, DirtyFlagsComponent::All));
}

TEST_F(SVGInvalidationTests, AppendChildMovesExistingSourceBackedChildIntoSelfClosingParent) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="moved" x="4" y="5" width="10" height="11" fill="red" />
      <g id="target" />
    </svg>
  )";

  auto doc = parseSVG(input);
  ASSERT_TRUE(doc.hasSourceStore());
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  auto moved = doc.querySelector("#moved");
  ASSERT_TRUE(moved.has_value());

  target->appendChild(*moved);

  const std::string_view source = doc.source();
  const std::size_t targetOffset = source.find(R"(<g id="target">)");
  const std::size_t movedOffset = source.find(R"(<rect id="moved")");
  ASSERT_NE(targetOffset, std::string_view::npos);
  ASSERT_NE(movedOffset, std::string_view::npos);
  EXPECT_LT(targetOffset, movedOffset);
  EXPECT_NE(source.find(R"(</g>)"), std::string_view::npos);

  EXPECT_EQ(target->firstChild(), moved);
  EXPECT_EQ(moved->parentElement(), target);
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::All));
  EXPECT_TRUE(hasDirtyFlags(*moved, DirtyFlagsComponent::All));
}

// ---------------------------------------------------------------------------
// removeChild
// ---------------------------------------------------------------------------

TEST_F(SVGInvalidationTests, RemoveChildMarksParentDirtyAll) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent">
        <rect id="child" width="10" height="10" />
      </g>
    </svg>
  )");
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  auto child = doc.querySelector("#child");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());

  parent->removeChild(*child);

  EXPECT_TRUE(hasDirtyFlags(*parent, DirtyFlagsComponent::All));
}

TEST_F(SVGInvalidationTests, RemoveChildSetsNeedsFullRebuild) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent">
        <rect id="child" width="10" height="10" />
      </g>
    </svg>
  )");
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  auto child = doc.querySelector("#child");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());

  parent->removeChild(*child);

  auto& renderState = doc.registry().ctx().get<RenderTreeState>();
  EXPECT_TRUE(renderState.needsFullRebuild);
}

TEST_F(SVGInvalidationTests, RemoveChildUpdatesSourceThroughXMLDocument) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent"><rect id="target" width="10" height="10" /><circle id="sibling" /></g>
    </svg>
  )";

  auto doc = parseSVG(input);
  ASSERT_TRUE(doc.hasSourceStore());
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  ASSERT_TRUE(parent.has_value());
  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  parent->removeChild(*target);

  EXPECT_EQ(doc.source().find(R"(<rect id="target")"), std::string_view::npos);
  EXPECT_FALSE(doc.querySelector("#target").has_value());
  ASSERT_TRUE(parent->firstChild().has_value());
  EXPECT_THAT(parent->firstChild()->getAttribute("id"), testing::Optional(RcString("sibling")));
  EXPECT_EQ(target->parentElement(), std::nullopt);
  EXPECT_TRUE(hasDirtyFlags(*parent, DirtyFlagsComponent::All));
}

// ---------------------------------------------------------------------------
// Dirty propagation to descendants
// ---------------------------------------------------------------------------

TEST_F(SVGInvalidationTests, SetStylePropagatesDirtyToDescendants) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent">
        <rect id="child" width="10" height="10" />
        <g id="nested">
          <rect id="grandchild" width="5" height="5" />
        </g>
      </g>
    </svg>
  )");
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  auto child = doc.querySelector("#child");
  auto grandchild = doc.querySelector("#grandchild");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());
  ASSERT_TRUE(grandchild.has_value());

  parent->setStyle("opacity: 0.5");

  // The parent itself gets StyleCascade.
  EXPECT_TRUE(hasDirtyFlags(*parent, DirtyFlagsComponent::Style));

  // Descendants should get style-related dirty flags propagated.
  EXPECT_TRUE(hasDirtyFlags(*child, DirtyFlagsComponent::Style));
  EXPECT_TRUE(hasDirtyFlags(*child, DirtyFlagsComponent::Paint));
  EXPECT_TRUE(hasDirtyFlags(*child, DirtyFlagsComponent::RenderInstance));

  EXPECT_TRUE(hasDirtyFlags(*grandchild, DirtyFlagsComponent::Style));
  EXPECT_TRUE(hasDirtyFlags(*grandchild, DirtyFlagsComponent::Paint));
  EXPECT_TRUE(hasDirtyFlags(*grandchild, DirtyFlagsComponent::RenderInstance));
}

TEST_F(SVGInvalidationTests, SetTransformPropagatesWorldTransformToDescendants) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent">
        <rect id="child" width="10" height="10" />
      </g>
    </svg>
  )");
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  auto child = doc.querySelector("#child");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());

  parent->setAttribute("transform", "scale(2)");

  // Parent gets Transform + WorldTransform + RenderInstance.
  EXPECT_TRUE(hasDirtyFlags(*parent, DirtyFlagsComponent::Transform));
  EXPECT_TRUE(hasDirtyFlags(*parent, DirtyFlagsComponent::WorldTransform));

  // Child should get WorldTransform + RenderInstance propagated.
  EXPECT_TRUE(hasDirtyFlags(*child, DirtyFlagsComponent::WorldTransform));
  EXPECT_TRUE(hasDirtyFlags(*child, DirtyFlagsComponent::RenderInstance));
}

TEST_F(SVGInvalidationTests, SetClassNamePropagatesDirtyToDescendants) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent">
        <rect id="child" width="10" height="10" />
      </g>
    </svg>
  )");
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  auto child = doc.querySelector("#child");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());

  parent->setClassName("highlight");

  // Style dirty flags should propagate to descendants.
  EXPECT_TRUE(hasDirtyFlags(*child, DirtyFlagsComponent::Style));
  EXPECT_TRUE(hasDirtyFlags(*child, DirtyFlagsComponent::Paint));
  EXPECT_TRUE(hasDirtyFlags(*child, DirtyFlagsComponent::RenderInstance));
}

TEST_F(SVGInvalidationTests, AppendChildPropagatesDirtyToInsertedSubtree) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent"></g>
    </svg>
  )");
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  ASSERT_TRUE(parent.has_value());

  // Build a subtree: g > rect
  auto newGroup = SVGUnknownElement::Create(doc, "g");
  auto newRect = SVGRectElement::Create(doc);
  newGroup.appendChild(newRect);

  // Clear dirty flags from creating the subtree.
  clearDirty(newGroup);
  clearDirty(newRect);

  parent->appendChild(newGroup);

  // Both the new group and its child rect should be dirty.
  EXPECT_TRUE(hasDirtyFlags(newGroup, DirtyFlagsComponent::All));
  EXPECT_TRUE(hasDirtyFlags(newRect, DirtyFlagsComponent::All));
}

// ---------------------------------------------------------------------------
// Clean state after simulated render
// ---------------------------------------------------------------------------

TEST_F(SVGInvalidationTests, DirtyFlagsClearedAfterSimulatedRender) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" />
    </svg>
  )");

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  // After parsing, elements may have dirty flags.
  // Simulate a completed render which clears them.
  simulateRenderComplete(doc);

  EXPECT_FALSE(isDirty(*target));
}

TEST_F(SVGInvalidationTests, MutationAfterRenderSetsDirtyAgain) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" />
    </svg>
  )");
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EXPECT_FALSE(isDirty(*target));

  // Mutate after render.
  target->setStyle("stroke: blue");

  // Should be dirty again.
  EXPECT_TRUE(isDirty(*target));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::StyleCascade));
}

TEST_F(SVGInvalidationTests, MultipleMutationsAccumulateFlags) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" />
    </svg>
  )");
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  target->setStyle("fill: red");
  target->setAttribute("transform", "rotate(45)");

  // Should have both style and transform flags accumulated.
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Style));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Transform));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::WorldTransform));
}

// ---------------------------------------------------------------------------
// remove() (self-removal) marks parent dirty
// ---------------------------------------------------------------------------

TEST_F(SVGInvalidationTests, RemoveSelfMarksParentDirty) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent">
        <rect id="child" width="10" height="10" />
      </g>
    </svg>
  )");
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  auto child = doc.querySelector("#child");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());

  child->remove();

  EXPECT_TRUE(hasDirtyFlags(*parent, DirtyFlagsComponent::All));
}

// ---------------------------------------------------------------------------
// Inherited presentation attribute propagation
// ---------------------------------------------------------------------------

TEST_F(SVGInvalidationTests, InheritedPresentationAttributePropagatesDirtyToDescendants) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent">
        <rect id="child" width="10" height="10" />
      </g>
    </svg>
  )");
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  auto child = doc.querySelector("#child");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());

  // "fill" is an inherited presentation attribute.
  parent->setAttribute("fill", "red");

  // Child should get style-related dirty flags because fill is inherited.
  EXPECT_TRUE(hasDirtyFlags(*child, DirtyFlagsComponent::Style));
  EXPECT_TRUE(hasDirtyFlags(*child, DirtyFlagsComponent::Paint));
  EXPECT_TRUE(hasDirtyFlags(*child, DirtyFlagsComponent::RenderInstance));
}

}  // namespace

}  // namespace donner::svg
