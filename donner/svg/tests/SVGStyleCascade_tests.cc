#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string_view>
#include <tuple>
#include <vector>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGElement.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGStyleElement.h"
#include "donner/svg/SVGStyleQuery.h"
#include "donner/svg/properties/PropertyRegistry.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::Optional;

namespace donner::svg {
namespace {

using css::Color;
using css::RGBA;

TEST(SVGStyleCascadeTests, CustomPropertySourceUsesNormalAndImportantLayerWinners) {
  auto doc = instantiateSubtree(R"svg(
    <style><![CDATA[
      @layer base, theme;
      @layer base { #parent { --normal: red; --priority: green !important; } }
      @layer theme {
        g { --n\6frmal: blue; --priority: red !important; }
        rect { --normal: inherit; --priority: unset; fill: var(--normal); }
      }
    ]]></style>
    <g id="parent"><rect id="target"/></g>
  )svg");
  const auto target = doc.querySelector("#target");
  ASSERT_THAT(target, Optional(testing::_));

  EXPECT_THAT(FindCustomPropertyDeclaration(*target, "--normal"), testing::Eq(std::nullopt));
  std::ignore = target->getComputedStyle();
  const auto normal = FindCustomPropertyDeclaration(*target, "--normal");
  ASSERT_THAT(normal, Optional(testing::_));
  EXPECT_EQ(normal->start, FileOffset::Offset(doc.source().find(R"(--n\6frmal)")));
  EXPECT_EQ(normal->end, FileOffset::Offset(doc.source().find(R"(--n\6frmal)") + 10));
  const auto priority = FindCustomPropertyDeclaration(*target, "--priority");
  ASSERT_THAT(priority, Optional(testing::_));
  EXPECT_EQ(priority->start, FileOffset::Offset(doc.source().find("--priority: green")));
  EXPECT_EQ(priority->end, FileOffset::Offset(doc.source().find("--priority: green") + 10));
  EXPECT_THAT(FindCustomPropertyDeclaration(*target, "--Normal"), testing::Eq(std::nullopt));
}

TEST(SVGStyleCascadeTests, CustomPropertySourceMapsInlineXmlReferencesAndLiveEdits) {
  auto doc = instantiateSubtree(R"svg(
    <style>rect { --t&#111;ne: red; --blocked: blue; --cycle: red; }</style>
    <rect id="target" style="--other: &amp;; --t&#111;n&#x65;: blue; --blocked: initial;
        --cycle: var(--cycle); fill: var(--tone)" />
  )svg");
  const auto target = doc.querySelector("#target");
  ASSERT_THAT(target, Optional(testing::_));
  std::ignore = target->getComputedStyle();
  constexpr std::string_view authoredName = "--t&#111;n&#x65;";
  const auto before = FindCustomPropertyDeclaration(*target, "--tone");
  ASSERT_THAT(before, Optional(testing::_));
  const auto start = doc.source().find(authoredName);
  EXPECT_EQ(before->start, FileOffset::Offset(start));
  EXPECT_EQ(before->end, FileOffset::Offset(start + authoredName.size()));
  EXPECT_THAT(FindCustomPropertyDeclaration(*target, "--blocked"), testing::Eq(std::nullopt));
  EXPECT_THAT(FindCustomPropertyDeclaration(*target, "--cycle"), testing::Eq(std::nullopt));

  const auto result = doc.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(start), FileOffset::Offset(start + authoredName.size())},
      .replacement = "--Tone",
      .sourceVersion = doc.sourceVersion(),
  });
  ASSERT_TRUE(result.applied);
  std::ignore = target->getComputedStyle();
  const auto after = FindCustomPropertyDeclaration(*target, "--tone");
  ASSERT_THAT(after, Optional(testing::_));
  EXPECT_EQ(after->start, FileOffset::Offset(doc.source().find("--t&#111;ne: red")));
  EXPECT_EQ(after->end, FileOffset::Offset(doc.source().find("--t&#111;ne: red") + 11));
  const auto sheet = doc.querySelector("style");
  ASSERT_THAT(sheet, Optional(testing::_));
  const auto stylesheet = sheet->tryCast<SVGStyleElement>();
  ASSERT_THAT(stylesheet, Optional(testing::_));
  const auto projected =
      MapStylesheetSourceRange(*stylesheet, {FileOffset::Offset(7), FileOffset::Offset(13)});
  EXPECT_THAT(projected,
              Optional(testing::AllOf(
                  testing::Field("start", &SourceRange::start, testing::Eq(after->start)),
                  testing::Field("end", &SourceRange::end, testing::Eq(after->end)))));
  const auto uppercase = FindCustomPropertyDeclaration(*target, "--Tone");
  ASSERT_THAT(uppercase, Optional(testing::_));
  EXPECT_EQ(uppercase->start, FileOffset::Offset(doc.source().find("--Tone: blue")));
  EXPECT_EQ(uppercase->end, FileOffset::Offset(doc.source().find("--Tone: blue") + 6));
}

TEST(SVGStyleCascadeTests, CustomPropertySourceTracksCompleteMergedInlineStyle) {
  auto doc = instantiateSubtree(R"svg(
    <rect id="target" style="fill: red; --Tone: blue !important; --tone: green" />
    <rect id="escaped" style="--T\6fne: purple" />
  )svg");
  auto target = doc.querySelector("#target");
  const auto escapedTarget = doc.querySelector("#escaped");
  ASSERT_THAT(target, Optional(testing::_));
  ASSERT_THAT(escapedTarget, Optional(testing::_));
  std::ignore = target->getComputedStyle();
  std::ignore = escapedTarget->getComputedStyle();

  constexpr std::string_view escapedName = R"(--T\6fne)";
  const auto escaped = FindCustomPropertyDeclaration(*escapedTarget, "--Tone");
  ASSERT_THAT(escaped, Optional(testing::_));
  EXPECT_EQ(escaped->start, FileOffset::Offset(doc.source().find(escapedName)));
  EXPECT_EQ(escaped->end, FileOffset::Offset(doc.source().find(escapedName) + escapedName.size()));
  EXPECT_THAT(FindCustomPropertyDeclaration(*escapedTarget, "--tone"), testing::Eq(std::nullopt));

  // This semantically and textually identical update leaves both the attribute and source intact.
  // Refresh metadata from the complete merged attribute so the declaration offset still points
  // after fill, rather than to byte zero of the update fragment.
  target->updateStyle("--tone: green");
  std::ignore = target->getComputedStyle();
  const auto unchanged = FindCustomPropertyDeclaration(*target, "--tone");
  ASSERT_THAT(unchanged, Optional(testing::_));
  const auto unchangedStart = doc.source().find("--tone: green");
  EXPECT_EQ(unchanged->start, FileOffset::Offset(unchangedStart));
  EXPECT_EQ(unchanged->end, FileOffset::Offset(unchangedStart + 6));
  const auto important = FindCustomPropertyDeclaration(*target, "--Tone");
  ASSERT_THAT(important, Optional(testing::_));
  const auto importantStart = doc.source().find("--Tone: blue !important");
  EXPECT_EQ(important->start, FileOffset::Offset(importantStart));
  EXPECT_EQ(important->end, FileOffset::Offset(importantStart + 6));

  target->updateStyle("--tone: gold !important");
  std::ignore = target->getComputedStyle();
  EXPECT_THAT(FindCustomPropertyDeclaration(*target, "--tone"), testing::Eq(std::nullopt))
      << "a semantic attribute edit invalidates the old document-source projection";
  const auto merged = target->getAttribute("style");
  ASSERT_THAT(merged, Optional(testing::_));
  auto reparsed = instantiateSubtree(R"(<rect id="target" style=")" +
                                     std::string(std::string_view(*merged)) + R"("/>)");
  const auto reparsedTarget = reparsed.querySelector("#target");
  ASSERT_THAT(reparsedTarget, Optional(testing::_));
  std::ignore = reparsedTarget->getComputedStyle();
  const auto changed = FindCustomPropertyDeclaration(*reparsedTarget, "--tone");
  ASSERT_THAT(changed, Optional(testing::_));
  const auto changedStart = reparsed.source().find("--tone: gold !important");
  EXPECT_EQ(changed->start, FileOffset::Offset(changedStart));
  EXPECT_EQ(changed->end, FileOffset::Offset(changedStart + 6));
}

TEST(SVGStyleCascadeTests, CustomPropertySourceBatchSharesCascadeAndPreservesRequestOrder) {
  auto doc = instantiateSubtree(R"svg(
    <style><![CDATA[
      @layer base, theme;
      @layer base { g { --tone: red; --priority: green !important; } }
      @layer theme {
        #parent { --t\6fne: blue; --priority: red !important; }
        rect { --tone: inherit; }
      }
    ]]></style>
    <g id="parent"><rect id="target" style="--local: gold"/></g>
  )svg");
  const auto target = doc.querySelector("#target");
  ASSERT_THAT(target, Optional(testing::_));
  std::ignore = target->getComputedStyle();

  const std::vector<std::string_view> names{"--tone",    "--priority", "--local",
                                            "--missing", "--tone",     "--Tone"};
  const auto batch = FindCustomPropertyDeclarations(*target, names, 1024 * 1024);
  ASSERT_TRUE(batch.complete);
  EXPECT_FALSE(batch.budgetExceeded);
  const auto atName = [&](std::string_view authored) {
    const auto start = doc.source().find(authored);
    return Optional(testing::AllOf(
        testing::Field("start", &SourceRange::start, FileOffset::Offset(start)),
        testing::Field("end", &SourceRange::end, FileOffset::Offset(start + authored.size()))));
  };
  const auto missing = testing::Eq(std::nullopt);
  EXPECT_THAT(batch.declarations,
              testing::ElementsAre(atName(R"(--t\6fne)"), atName("--priority"), atName("--local"),
                                   missing, atName(R"(--t\6fne)"), missing));
  EXPECT_THAT(FindCustomPropertyDeclaration(*target, "--tone"), atName(R"(--t\6fne)"));

  std::size_t separateWork = 0;
  for (const std::string_view name : names) {
    const std::vector<std::string_view> one{name};
    const auto separate = FindCustomPropertyDeclarations(*target, one, 1024 * 1024);
    ASSERT_TRUE(separate.complete);
    separateWork += separate.work;
  }
  EXPECT_LT(batch.work, separateWork);

  ASSERT_GT(batch.work, 0u);
  const auto exact = FindCustomPropertyDeclarations(*target, names, batch.work);
  EXPECT_TRUE(exact.complete);
  EXPECT_FALSE(exact.budgetExceeded);
  EXPECT_EQ(exact.work, batch.work);
  EXPECT_EQ(exact.declarations, batch.declarations);
  const auto refused = FindCustomPropertyDeclarations(*target, names, batch.work - 1);
  EXPECT_FALSE(refused.complete);
  EXPECT_TRUE(refused.budgetExceeded);
  EXPECT_EQ(refused.work, batch.work - 1);
  EXPECT_THAT(refused.declarations,
              testing::ElementsAre(missing, missing, missing, missing, missing, missing));
}

TEST(SVGStyleCascadeTests, CustomPropertySourceBatchDistinguishesUnpreparedFromBudgetRefusal) {
  auto doc = instantiateSubtree(R"(<rect id="target" style="--tone: blue"/>)");
  const auto target = doc.querySelector("#target");
  ASSERT_THAT(target, Optional(testing::_));
  const std::vector<std::string_view> names{"--tone", "--tone", "fill"};

  const auto unprepared = FindCustomPropertyDeclarations(*target, names, 128);
  EXPECT_FALSE(unprepared.complete);
  EXPECT_FALSE(unprepared.budgetExceeded);
  EXPECT_GT(unprepared.work, 0u);
  EXPECT_LT(unprepared.work, 128u);
  const auto missing = testing::Eq(std::nullopt);
  EXPECT_THAT(unprepared.declarations, testing::ElementsAre(missing, missing, missing));

  std::ignore = target->getComputedStyle();
  const auto noWork = FindCustomPropertyDeclarations(*target, names, 0);
  EXPECT_FALSE(noWork.complete);
  EXPECT_TRUE(noWork.budgetExceeded);
  EXPECT_EQ(noWork.work, 0u);
}

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
