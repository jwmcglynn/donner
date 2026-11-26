#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <vector>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGElement.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGStyleQuery.h"
#include "donner/svg/properties/PropertyRegistry.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::Optional;

namespace donner::svg {
namespace {

using css::Color;
using css::RGBA;

TEST(SVGStyleCascadeTests, LayerPrecedenceComputesCustomValuesBeforeInheritance) {
  auto doc = instantiateSubtree(R"(
    <style>
      @layer base, theme;
      @layer base { #parent { --normal: red; } g { --priority: green !important; } }
      @layer theme {
        g { --normal: blue; --inherited: var(--normal); }
        #parent { --priority: red !important; }
        rect { fill: var(--inherited); stroke: var(--priority); --normal: red; }
        circle { fill: var(--normal); stroke: var(--priority); }
      }
    </style>
    <g id="parent">
      <rect id="fixed" width="10" height="10" />
      <circle id="inherited" r="5" />
    </g>
  )");
  const auto fixed = doc.querySelector("#fixed");
  const auto inherited = doc.querySelector("#inherited");
  ASSERT_THAT(fixed, Optional(testing::_));
  ASSERT_THAT(inherited, Optional(testing::_));
  const PaintServer blue = PaintServer::Solid(Color(RGBA(0, 0, 255, 255)));
  const PaintServer green = PaintServer::Solid(Color(RGBA(0, 128, 0, 255)));
  for (const auto* element : {&*fixed, &*inherited}) {
    const auto& style = element->getComputedStyle();
    EXPECT_THAT(style.fill.get(), Optional(blue));
    EXPECT_THAT(style.stroke.get(), Optional(green));
  }
}

TEST(SVGStyleCascadeTests, NestedLayerNormalAndImportantOrder) {
  auto doc = instantiateSubtree(R"(
    <style>
      @layer a, b;
      @layer b { rect { fill: blue; stroke: blue !important; } }
      @layer a { #r { fill: green; stroke: green !important; } }
      @layer a.child { rect { fill: red; stroke: red !important; } }
    </style>
    <rect id="r" fill="yellow" stroke="yellow" width="10" height="10" />
  )");
  auto rect = doc.querySelector("#r");
  ASSERT_THAT(rect, Optional(testing::_));
  const auto& style = rect->getComputedStyle();
  EXPECT_THAT(style.fill.get(), Optional(PaintServer::Solid(Color(RGBA(0, 0, 255, 255)))));
  EXPECT_THAT(style.stroke.get(), Optional(PaintServer::Solid(Color(RGBA(255, 0, 0, 255)))));
}

TEST(SVGStyleCascadeTests, MatchedStyleQueryReportsLayerPrecedenceInTreeOrder) {
  auto doc = instantiateSubtree(R"(
    <style>@layer base { #target { fill: red; } }</style>
    <style>@layer theme { rect { fill: blue; } }</style>
    <rect id="target" width="10" height="10" />
  )");
  const auto target = doc.querySelector("#target");
  ASSERT_THAT(target, Optional(testing::_));

  auto rules = CollectMatchedStyleRules(*target);
  std::erase_if(rules, [](const auto& rule) { return rule.isUserAgentStylesheet; });
  ASSERT_THAT(rules, testing::SizeIs(2));
  EXPECT_THAT(rules[0].specificity.abc().a, testing::Eq(1));
  EXPECT_THAT(rules[1].specificity.abc().a, testing::Eq(0));
  EXPECT_THAT(rules[1].specificity, testing::Gt(rules[0].specificity));
  EXPECT_THAT(css::Specificity::Important(rules[0].specificity),
              testing::Gt(css::Specificity::Important(rules[1].specificity)));
}

TEST(SVGStyleCascadeTests, LayeredPropertyDoesNotOverrideExplicitChildViaInheritance) {
  auto doc = instantiateSubtree(R"(
    <style>
      @layer early, late;
      @layer early { rect { fill: red; } }
      @layer late { #parent { fill: blue; stroke: blue !important; } }
    </style>
    <g id="parent">
      <rect id="child" stroke="red" width="10" height="10" />
      <circle id="inherited" r="5" />
    </g>
  )");
  const auto child = doc.querySelector("#child");
  const auto inherited = doc.querySelector("#inherited");
  ASSERT_THAT(child, Optional(testing::_));
  ASSERT_THAT(inherited, Optional(testing::_));
  const auto& childStyle = child->getComputedStyle();
  EXPECT_THAT(childStyle.fill.get(), Optional(PaintServer::Solid(Color(RGBA(255, 0, 0, 255)))));
  EXPECT_THAT(childStyle.stroke.get(), Optional(PaintServer::Solid(Color(RGBA(255, 0, 0, 255)))));
  EXPECT_THAT(inherited->getComputedStyle().fill.get(),
              Optional(PaintServer::Solid(Color(RGBA(0, 0, 255, 255)))));
}

TEST(SVGStyleCascadeTests, UnlayeredRulesAndInlineStylesKeepTheirPrecedence) {
  auto doc = instantiateSubtree(R"(
    <style>
      @layer base { #r { fill: blue; stroke: blue !important; } }
      rect { fill: red; stroke: red !important; }
    </style>
    <rect id="r" fill="yellow" style="stroke: green !important" width="10" height="10" />
  )");
  auto rect = doc.querySelector("#r");
  ASSERT_THAT(rect, Optional(testing::_));
  const auto& style = rect->getComputedStyle();
  EXPECT_THAT(style.fill.get(), Optional(PaintServer::Solid(Color(RGBA(255, 0, 0, 255)))));
  EXPECT_THAT(style.stroke.get(), Optional(PaintServer::Solid(Color(RGBA(0, 128, 0, 255)))));
}

// ---------------------------------------------------------------------------
// 1. Inline style overrides presentation attribute
// ---------------------------------------------------------------------------

TEST(SVGStyleCascadeTests, InlineStyleOverridesAttribute) {
  // fill="blue" sets fill via presentation attribute (specificity 0,0,0),
  // style="fill: red" sets fill via inline style (higher specificity).
  // Inline style should win.
  auto doc = instantiateSubtree(R"(
    <rect id="r" fill="blue" style="fill: red" width="10" height="10" />
  )");
  auto rect = doc.querySelector("rect");
  ASSERT_TRUE(rect.has_value());

  const auto& style = rect->getComputedStyle();
  EXPECT_THAT(style.fill.get(),
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0xFF, 0, 0, 0xFF))))));
}

// ---------------------------------------------------------------------------
// 2. CSS class selector
// ---------------------------------------------------------------------------

TEST(SVGStyleCascadeTests, CSSClassSelector) {
  auto doc = instantiateSubtree(R"(
    <style>.foo { fill: green }</style>
    <rect class="foo" width="10" height="10" />
  )");
  auto rect = doc.querySelector("rect");
  ASSERT_TRUE(rect.has_value());

  const auto& style = rect->getComputedStyle();
  EXPECT_THAT(style.fill.get(),
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0, 0x80, 0, 0xFF))))));
}

// ---------------------------------------------------------------------------
// 3. CSS id selector
// ---------------------------------------------------------------------------

TEST(SVGStyleCascadeTests, CSSIdSelector) {
  auto doc = instantiateSubtree(R"(
    <style>#bar { stroke: blue }</style>
    <rect id="bar" width="10" height="10" />
  )");
  auto rect = doc.querySelector("#bar");
  ASSERT_TRUE(rect.has_value());

  const auto& style = rect->getComputedStyle();
  EXPECT_THAT(style.stroke.get(),
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0, 0, 0xFF, 0xFF))))));
}

// ---------------------------------------------------------------------------
// 4. Specificity: id selector beats class selector
// ---------------------------------------------------------------------------

TEST(SVGStyleCascadeTests, SpecificityIdBeatsClass) {
  // Both rules match, but #box (0,1,0,0) has higher specificity than .red (0,0,1,0).
  auto doc = instantiateSubtree(R"(
    <style>
      .red { fill: red }
      #box { fill: blue }
    </style>
    <rect id="box" class="red" width="10" height="10" />
  )");
  auto rect = doc.querySelector("#box");
  ASSERT_TRUE(rect.has_value());

  const auto& style = rect->getComputedStyle();
  EXPECT_THAT(style.fill.get(),
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0, 0, 0xFF, 0xFF))))));
}

// ---------------------------------------------------------------------------
// 5. !important overrides higher-specificity rule
// ---------------------------------------------------------------------------

TEST(SVGStyleCascadeTests, ImportantOverridesHigherSpecificity) {
  // .red has lower specificity than #box, but !important should win.
  auto doc = instantiateSubtree(R"(
    <style>
      .red { fill: red !important }
      #box { fill: blue }
    </style>
    <rect id="box" class="red" width="10" height="10" />
  )");
  auto rect = doc.querySelector("#box");
  ASSERT_TRUE(rect.has_value());

  const auto& style = rect->getComputedStyle();
  EXPECT_THAT(style.fill.get(),
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0xFF, 0, 0, 0xFF))))));
}

// ---------------------------------------------------------------------------
// 6. Inheritance: fill on parent <g> inherits to child <rect>
// ---------------------------------------------------------------------------

TEST(SVGStyleCascadeTests, InheritedPropertyFromParent) {
  // fill has PropertyCascade::PaintInherit, so it inherits from parent to child.
  auto doc = instantiateSubtree(R"(
    <g fill="lime">
      <rect id="child" width="10" height="10" />
    </g>
  )");
  auto rect = doc.querySelector("#child");
  ASSERT_TRUE(rect.has_value());

  const auto& style = rect->getComputedStyle();
  EXPECT_THAT(style.fill.get(),
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0, 0xFF, 0, 0xFF))))));
}

// ---------------------------------------------------------------------------
// 7. Inherited property override: child overrides inherited fill
// ---------------------------------------------------------------------------

TEST(SVGStyleCascadeTests, ChildOverridesInheritedProperty) {
  auto doc = instantiateSubtree(R"(
    <g fill="lime">
      <rect id="child" fill="red" width="10" height="10" />
    </g>
  )");
  auto rect = doc.querySelector("#child");
  ASSERT_TRUE(rect.has_value());

  const auto& style = rect->getComputedStyle();
  EXPECT_THAT(style.fill.get(),
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0xFF, 0, 0, 0xFF))))));
}

// ---------------------------------------------------------------------------
// 8. Non-inherited property: opacity does NOT inherit
// ---------------------------------------------------------------------------

TEST(SVGStyleCascadeTests, NonInheritedPropertyDoesNotInherit) {
  // opacity has PropertyCascade::None, so the child should get the default (1.0),
  // not the parent's value (0.5).
  auto doc = instantiateSubtree(R"(
    <g opacity="0.5">
      <rect id="child" width="10" height="10" />
    </g>
  )");
  auto rect = doc.querySelector("#child");
  ASSERT_TRUE(rect.has_value());

  const auto& style = rect->getComputedStyle();
  // opacity should be the initial value 1.0, not the parent's 0.5.
  EXPECT_THAT(style.opacity.get(), Optional(1.0));
}

// ---------------------------------------------------------------------------
// 9. Later rule wins for equal specificity within a single stylesheet
// ---------------------------------------------------------------------------

TEST(SVGStyleCascadeTests, LaterRuleWinsEqualSpecificity) {
  // Two rules with the same selector and specificity in one <style> block:
  // the later rule in source order should win.
  auto doc = instantiateSubtree(R"(
    <style>
      rect { fill: red }
      rect { fill: blue }
    </style>
    <rect width="10" height="10" />
  )");
  auto rect = doc.querySelector("rect");
  ASSERT_TRUE(rect.has_value());

  const auto& style = rect->getComputedStyle();
  EXPECT_THAT(style.fill.get(),
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0, 0, 0xFF, 0xFF))))));
}

// ---------------------------------------------------------------------------
// 10. Presentation attribute sets fill
// ---------------------------------------------------------------------------

TEST(SVGStyleCascadeTests, PresentationAttribute) {
  auto doc = instantiateSubtree(R"(
    <rect fill="blue" width="10" height="10" />
  )");
  auto rect = doc.querySelector("rect");
  ASSERT_TRUE(rect.has_value());

  const auto& style = rect->getComputedStyle();
  EXPECT_THAT(style.fill.get(),
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0, 0, 0xFF, 0xFF))))));
}

// ---------------------------------------------------------------------------
// 11. Descendant selector: g rect { fill: purple }
// ---------------------------------------------------------------------------

TEST(SVGStyleCascadeTests, DescendantSelector) {
  auto doc = instantiateSubtree(R"(
    <style>g rect { fill: purple }</style>
    <g>
      <rect id="inside" width="10" height="10" />
    </g>
    <rect id="outside" width="10" height="10" />
  )");

  // The rect inside the <g> should match the descendant selector.
  auto inside = doc.querySelector("#inside");
  ASSERT_TRUE(inside.has_value());
  EXPECT_THAT(inside->getComputedStyle().fill.get(),
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0x80, 0, 0x80, 0xFF))))));

  // The rect outside the <g> should NOT match "g rect", so fill defaults to black.
  auto outside = doc.querySelector("#outside");
  ASSERT_TRUE(outside.has_value());
  EXPECT_THAT(outside->getComputedStyle().fill.get(),
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0, 0, 0, 0xFF))))));
}

// ---------------------------------------------------------------------------
// 12. Computed style after mutation: change class, verify style updates
// ---------------------------------------------------------------------------

TEST(SVGStyleCascadeTests, ComputedStyleAfterClassMutation) {
  auto doc = instantiateSubtree(R"(
    <style>
      .a { fill: red }
      .b { fill: blue }
    </style>
    <rect id="r" class="a" width="10" height="10" />
  )");
  auto rect = doc.querySelector("#r");
  ASSERT_TRUE(rect.has_value());

  // Initially has class "a", so fill should be red.
  EXPECT_THAT(rect->getComputedStyle().fill.get(),
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0xFF, 0, 0, 0xFF))))));

  // Mutate the class to "b".
  rect->setClassName("b");

  // After mutation, fill should be blue.
  EXPECT_THAT(rect->getComputedStyle().fill.get(),
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0, 0, 0xFF, 0xFF))))));
}

// ---------------------------------------------------------------------------
// 13. currentColor: color: red; fill: currentColor -> fill resolves to red
// ---------------------------------------------------------------------------

TEST(SVGStyleCascadeTests, CurrentColorResolvesToColor) {
  // When fill is set to currentColor, it should store a paint server with
  // Color(CurrentColor), and the 'color' property provides the resolved value.
  auto doc = instantiateSubtree(R"(
    <rect id="r" style="color: red; fill: currentColor" width="10" height="10" />
  )");
  auto rect = doc.querySelector("#r");
  ASSERT_TRUE(rect.has_value());

  const auto& style = rect->getComputedStyle();
  // Verify the color property is red.
  EXPECT_THAT(style.color.get(), Optional(Color(RGBA(0xFF, 0, 0, 0xFF))));

  // Verify fill is set to currentColor (not yet resolved to RGBA at the property level).
  auto fillValue = style.fill.get();
  ASSERT_TRUE(fillValue.has_value());
  ASSERT_TRUE(fillValue->is<PaintServer::Solid>());
  EXPECT_TRUE(fillValue->get<PaintServer::Solid>().color.isCurrentColor());
}

// ---------------------------------------------------------------------------
// 14. CSS type selector (element selector) with lower specificity than class
// ---------------------------------------------------------------------------

TEST(SVGStyleCascadeTests, TypeSelectorLowerSpecificityThanClass) {
  auto doc = instantiateSubtree(R"(
    <style>
      rect { fill: red }
      .cls { fill: blue }
    </style>
    <rect class="cls" width="10" height="10" />
  )");
  auto rect = doc.querySelector("rect");
  ASSERT_TRUE(rect.has_value());

  const auto& style = rect->getComputedStyle();
  // .cls (0,0,1,0) beats rect (0,0,0,1), so fill should be blue.
  EXPECT_THAT(style.fill.get(),
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0, 0, 0xFF, 0xFF))))));
}

// ---------------------------------------------------------------------------
// 15. Stylesheet rule overrides presentation attribute (stylesheet > attr)
// ---------------------------------------------------------------------------

TEST(SVGStyleCascadeTests, StylesheetOverridesPresentationAttribute) {
  // A CSS rule with type selector has specificity (0,0,0,1), while a presentation
  // attribute has specificity (0,0,0,0). The stylesheet rule should win.
  auto doc = instantiateSubtree(R"(
    <style>rect { fill: green }</style>
    <rect fill="red" width="10" height="10" />
  )");
  auto rect = doc.querySelector("rect");
  ASSERT_TRUE(rect.has_value());

  const auto& style = rect->getComputedStyle();
  EXPECT_THAT(style.fill.get(),
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0, 0x80, 0, 0xFF))))));
}

// ---------------------------------------------------------------------------
// 16. Inherited visibility from parent
// ---------------------------------------------------------------------------

TEST(SVGStyleCascadeTests, InheritedVisibility) {
  // visibility has PropertyCascade::Inherit, so it should inherit from parent.
  auto doc = instantiateSubtree(R"(
    <g visibility="hidden">
      <rect id="child" width="10" height="10" />
    </g>
  )");
  auto rect = doc.querySelector("#child");
  ASSERT_TRUE(rect.has_value());

  const auto& style = rect->getComputedStyle();
  EXPECT_THAT(style.visibility.get(), Optional(Visibility::Hidden));
}

// ---------------------------------------------------------------------------
// Tree-position dependence of the author stylesheets in a document
// ---------------------------------------------------------------------------

TEST(SVGStyleCascadeTests, AuthorStyleDependsOnTreePositionIgnoresTheUserAgentStylesheet) {
  // The user agent stylesheet is always present and does use combinators, so a document with no
  // author rules must still report no dependence.
  auto doc = instantiateSubtree(R"(
    <rect id="r" width="10" height="10" />
  )");
  EXPECT_FALSE(AuthorStyleDependsOnTreePosition(doc));
}

TEST(SVGStyleCascadeTests, AuthorStyleDependsOnTreePositionReportsElementOnlySelectors) {
  auto doc = instantiateSubtree(R"(
    <style>rect { fill: red } #layer { fill: blue }</style>
    <g id="layer"><rect id="r" width="10" height="10" /></g>
  )");
  EXPECT_FALSE(AuthorStyleDependsOnTreePosition(doc));
}

TEST(SVGStyleCascadeTests, AuthorStyleDependsOnTreePositionReportsDescendantSelectors) {
  auto doc = instantiateSubtree(R"(
    <style>#layer rect { fill: blue }</style>
    <g id="layer"><rect id="r" width="10" height="10" /></g>
  )");
  EXPECT_TRUE(AuthorStyleDependsOnTreePosition(doc));
}

}  // namespace
}  // namespace donner::svg
