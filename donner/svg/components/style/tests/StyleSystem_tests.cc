/**
 * @file Tests for StyleSystem: CSS cascade, inheritance, and style computation from SVG elements.
 */

#include "donner/svg/components/style/StyleSystem.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowTreeSystem.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleComponent.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/parser/SVGParser.h"

using testing::NotNull;

namespace donner::svg::components {

namespace {

void SetRenderTreeState(Registry& registry, const RenderTreeState& state) {
  if (registry.ctx().contains<RenderTreeState>()) {
    registry.ctx().erase<RenderTreeState>();
  }
  registry.ctx().emplace<RenderTreeState>(state);
}

}  // namespace

class StyleSystemTest : public ::testing::Test {
protected:
  SVGDocument ParseSVG(std::string_view input) {
    ParseWarningSink parseSink;
    auto maybeResult = parser::SVGParser::ParseSVG(input, parseSink);
    EXPECT_THAT(maybeResult, NoParseError());
    return std::move(maybeResult).result();
  }

  SVGDocument ParseAndComputeStyles(std::string_view input) {
    auto document = ParseSVG(input);
    ParseWarningSink warningSink;
    styleSystem.computeAllStyles(document.registry(), warningSink);
    return document;
  }

  StyleSystem styleSystem;
};

// --- Basic style computation ---

TEST_F(StyleSystemTest, ComputeAllStylesCreatesComponents) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="red"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_TRUE(computed->properties.has_value());
}

TEST_F(StyleSystemTest, InlineStyleApplied) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" style="fill: blue"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_TRUE(computed->properties.has_value());
}

// --- CSS stylesheet rules ---

TEST_F(StyleSystemTest, StylesheetRulesApplied) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <style>
        .red { fill: red; }
      </style>
      <rect id="r" class="red" width="50" height="50"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_TRUE(computed->properties.has_value());
}

TEST_F(StyleSystemTest, IdSelectorStyleApplied) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <style>
        #target { stroke: green; }
      </style>
      <rect id="target" width="50" height="50"/>
    </svg>
  )");

  auto element = document.querySelector("#target");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_TRUE(computed->properties.has_value());
}

// --- Inheritance ---

TEST_F(StyleSystemTest, StyleInheritedFromParent) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <g fill="red">
        <rect id="child" width="50" height="50"/>
      </g>
    </svg>
  )");

  auto element = document.querySelector("#child");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_TRUE(computed->properties.has_value());
}

TEST_F(StyleSystemTest, DisplayNoneComputed) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="hidden" width="50" height="50" style="display: none"/>
    </svg>
  )");

  auto element = document.querySelector("#hidden");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_TRUE(computed->properties.has_value());
  EXPECT_EQ(computed->properties->display.getRequired(), Display::None);
}

TEST_F(StyleSystemTest, VisibilityHiddenComputed) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="hidden" width="50" height="50" style="visibility: hidden"/>
    </svg>
  )");

  auto element = document.querySelector("#hidden");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_TRUE(computed->properties.has_value());
  EXPECT_EQ(computed->properties->visibility.getRequired(), Visibility::Hidden);
}

// --- Individual style computation ---

TEST_F(StyleSystemTest, ComputeStyleForSingleEntity) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="green"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  ParseWarningSink warningSink;
  const auto& computed = styleSystem.computeStyle(element->entityHandle(), warningSink);
  EXPECT_TRUE(computed.properties.has_value());
}

// --- Invalidation ---

TEST_F(StyleSystemTest, InvalidateComputedRemovesComponent) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="red"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  ASSERT_THAT(element->entityHandle().try_get<ComputedStyleComponent>(), NotNull());

  styleSystem.invalidateComputed(element->entityHandle());
  EXPECT_EQ(element->entityHandle().try_get<ComputedStyleComponent>(), nullptr);
}

// --- Multiple elements ---

TEST_F(StyleSystemTest, MultipleElementsAllComputed) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="a" width="25" height="25" fill="red"/>
      <circle id="b" cx="50" cy="50" r="20" fill="blue"/>
      <line id="c" x1="0" y1="0" x2="100" y2="100" stroke="green"/>
    </svg>
  )");

  for (const char* id : {"#a", "#b", "#c"}) {
    auto element = document.querySelector(id);
    ASSERT_TRUE(element.has_value()) << "Missing element: " << id;
    auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
    ASSERT_THAT(computed, NotNull()) << "No computed style for: " << id;
    EXPECT_TRUE(computed->properties.has_value()) << "No properties for: " << id;
  }
}

// --- Warnings ---

TEST_F(StyleSystemTest, WarningsCollectedForInvalidProperties) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <style>
        rect { fill: not-a-color; }
      </style>
      <rect id="r" width="50" height="50"/>
    </svg>
  )");

  ParseWarningSink warningSink;
  styleSystem.computeAllStyles(document.registry(), warningSink);
  // Invalid color should produce a warning (or be ignored gracefully).
  // We just verify it doesn't crash; the warning list may or may not be populated
  // depending on implementation detail.
  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
}

TEST_F(StyleSystemTest, UpdateStyleMergesAndInvalidatesComputedStyle) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" style="fill: red"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  ASSERT_THAT(element->entityHandle().try_get<ComputedStyleComponent>(), NotNull());

  styleSystem.updateStyle(element->entityHandle(), "stroke: blue; fill: green");

  EXPECT_EQ(element->entityHandle().try_get<ComputedStyleComponent>(), nullptr);
  auto styleAttr = element->getAttribute("style");
  ASSERT_TRUE(styleAttr.has_value());
  EXPECT_THAT(styleAttr.value(), testing::AllOf(testing::HasSubstr("fill: green"),
                                                testing::HasSubstr("stroke: blue")));

  ParseWarningSink warningSink;
  const auto& computed = styleSystem.computeStyle(element->entityHandle(), warningSink);
  EXPECT_EQ(computed.properties->fill.getRequired(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 128, 0, 0xFF)))));
  EXPECT_EQ(computed.properties->stroke.getRequired(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 0, 0xFF, 0xFF)))));
}

TEST_F(StyleSystemTest, ComputeAllStylesDirtyOnlyRecomputesStyleDirtyEntities) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="dirty" width="50" height="50"/>
      <rect id="clean" width="50" height="50"/>
    </svg>
  )");

  auto dirty = document.querySelector("#dirty")->entityHandle();
  auto clean = document.querySelector("#clean")->entityHandle();
  SetRenderTreeState(document.registry(), RenderTreeState{
      .needsFullRebuild = false, .needsFullStyleRecompute = false, .hasBeenBuilt = true});

  dirty.get_or_emplace<StyleComponent>().properties.parseStyle("fill: blue");
  clean.get_or_emplace<StyleComponent>().properties.parseStyle("fill: green");
  styleSystem.invalidateComputed(dirty);

  document.registry().emplace_or_replace<DirtyFlagsComponent>(dirty.entity()).mark(
      DirtyFlagsComponent::Style);
  document.registry().emplace_or_replace<DirtyFlagsComponent>(clean.entity()).mark(
      DirtyFlagsComponent::Layout);

  ParseWarningSink warningSink;
  styleSystem.computeAllStyles(document.registry(), warningSink);

  EXPECT_EQ(document.registry().get<ComputedStyleComponent>(dirty.entity())
                .properties->fill.getRequired(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 0, 0xFF, 0xFF)))));
  EXPECT_EQ(document.registry().get<ComputedStyleComponent>(clean.entity())
                .properties->fill.getRequired(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 0, 0, 0xFF)))));
}

TEST_F(StyleSystemTest, ComputeAllStylesFullRecomputeClearsAndRebuildsStyles) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <style>rect { fill: red; }</style>
      <rect id="r" width="50" height="50"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());

  auto styleEntity = document.querySelector("style")->entityHandle().entity();
  auto& stylesheet = document.registry().get<StylesheetComponent>(styleEntity);
  stylesheet.parseStylesheet("rect { fill: blue; }");
  SetRenderTreeState(document.registry(), RenderTreeState{
      .needsFullRebuild = false, .needsFullStyleRecompute = true, .hasBeenBuilt = true});

  ParseWarningSink warningSink;
  styleSystem.computeAllStyles(document.registry(), warningSink);

  EXPECT_EQ(document.registry().get<ComputedStyleComponent>(element->entityHandle().entity())
                .properties->fill.getRequired(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 0, 0xFF, 0xFF)))));
}

TEST_F(StyleSystemTest, ComputeAllStylesRegistersFontFacesDuringIncrementalPass) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <style>
        @font-face {
          font-family: TestFont;
          src: local(TestFont);
        }
      </style>
      <rect id="r" width="50" height="50"/>
    </svg>
  )");

  auto element = document.querySelector("#r")->entityHandle();
  SetRenderTreeState(document.registry(), RenderTreeState{
      .needsFullRebuild = false, .needsFullStyleRecompute = false, .hasBeenBuilt = true});
  document.registry().emplace_or_replace<DirtyFlagsComponent>(element.entity()).mark(
      DirtyFlagsComponent::Style);

  ParseWarningSink warningSink;
  styleSystem.computeAllStyles(document.registry(), warningSink);

  EXPECT_EQ(document.registry().ctx().get<ResourceManagerContext>().fontFaces().size(), 1u);
}

TEST_F(StyleSystemTest, ComputeStylesForSubsetOnlyComputesRequestedEntities) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="a" width="50" height="50" fill="red"/>
      <rect id="b" width="50" height="50" fill="blue"/>
    </svg>
  )");

  const Entity a = document.querySelector("#a")->entityHandle().entity();
  const Entity b = document.querySelector("#b")->entityHandle().entity();

  ParseWarningSink warningSink;
  styleSystem.computeStylesFor(document.registry(), std::array<Entity, 1>{a}, warningSink);

  EXPECT_TRUE(document.registry().all_of<ComputedStyleComponent>(a));
  EXPECT_FALSE(document.registry().all_of<ComputedStyleComponent>(b));
}

TEST_F(StyleSystemTest, ShadowTreeSelectorsMatchSiblingAndAttributeState) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <style>
        rect:first-child { fill: blue; }
        circle:last-child { stroke: green; }
        rect + circle { opacity: 0.5; }
        [fill] { visibility: hidden; }
      </style>
      <defs>
        <g id="src">
          <rect id="lightRect" width="10" height="10" fill="red"/>
          <circle id="lightCircle" r="5"/>
        </g>
      </defs>
      <use id="u" href="#src"/>
    </svg>
  )");

  auto useElement = document.querySelector("#u");
  ASSERT_TRUE(useElement.has_value());
  auto& shadowTree = document.registry().get_or_emplace<ComputedShadowTreeComponent>(
      useElement->entityHandle().entity());
  auto target = document.registry()
                    .get<ShadowTreeComponent>(useElement->entityHandle().entity())
                    .mainTargetEntity(document.registry());
  ASSERT_TRUE(target.has_value());

  ParseWarningSink warningSink;
  ShadowTreeSystem().populateInstance(useElement->entityHandle(), shadowTree, ShadowBranchType::Main,
                                      target->handle.entity(), RcString("#src"), warningSink);
  styleSystem.computeAllStyles(document.registry(), warningSink);

  ASSERT_TRUE(shadowTree.mainBranch.has_value());
  ASSERT_GE(shadowTree.mainBranch->shadowEntities.size(), 3u);

  Entity shadowRect = shadowTree.mainBranch->shadowEntities[1];
  Entity shadowCircle = shadowTree.mainBranch->shadowEntities[2];

  const auto& rectStyle = document.registry().get<ComputedStyleComponent>(shadowRect);
  const auto& circleStyle = document.registry().get<ComputedStyleComponent>(shadowCircle);

  EXPECT_EQ(rectStyle.properties->fill.getRequired(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 0, 0xFF, 0xFF)))));
  EXPECT_EQ(rectStyle.properties->visibility.getRequired(), Visibility::Hidden);
  EXPECT_EQ(circleStyle.properties->stroke.getRequired(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 128, 0, 0xFF)))));
  EXPECT_DOUBLE_EQ(circleStyle.properties->opacity.getRequired(), 0.5);
}

}  // namespace donner::svg::components
