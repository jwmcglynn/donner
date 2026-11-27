#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/css/CSS.h"
#include "donner/css/parser/DeclarationListParser.h"
#include "donner/css/parser/ValueParser.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGStyleElement.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/CustomPropertyMap.h"
#include "donner/svg/components/style/MatchedPropertiesCache.h"
#include "donner/svg/components/style/StyleComponent.h"
#include "donner/svg/components/style/tests/StyleSystemTest.h"
#include "donner/svg/parser/SVGParser.h"

using testing::NotNull;

namespace donner::svg::components {

using ::testing::ElementsAre;
using ::testing::Eq;

namespace {

std::optional<RcString> CustomIdent(const ComputedStyle& style, std::string_view name) {
  const auto* value = style.customProperties ? style.customProperties->find(name) : nullptr;
  if (!value || value->tokens().size() != 1 ||
      !value->tokens().front().isToken<css::Token::Ident>()) {
    return std::nullopt;
  }
  return value->tokens().front().get<css::Token>().get<css::Token::Ident>().value;
}

const ComputedStyleComponent& ComputedFor(const SVGElement& element) {
  return element.entityHandle().get<ComputedStyleComponent>();
}

}  // namespace

TEST_F(StyleSystemTest, CustomVariableReferencesComputeBeforeInheritance) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>g { --tone:red; --parent:var(--tone); }</style>
      <g><rect id="target" style="--tone:blue; --own:var(--tone); --later:var(--next); --next:green"/></g>
    </svg>)svg");
  auto target = document.querySelector("#target");
  ASSERT_THAT(target, testing::Optional(testing::_));
  const auto& slot = ComputedFor(*target);
  ASSERT_THAT(slot.style, NotNull());
  EXPECT_THAT(CustomIdent(*slot.style, "--parent"), testing::Optional(Eq("red")));
  EXPECT_THAT(CustomIdent(*slot.style, "--own"), testing::Optional(Eq("blue")));
  EXPECT_THAT(CustomIdent(*slot.style, "--later"), testing::Optional(Eq("green")));
}

TEST_F(StyleSystemTest, LocalVariableCyclesDoNotExposeInheritedValues) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" style="--a:inherited; --p:available">
      <rect id="target" style="--a:var(--b); --b:var(--a); --c:var(--a,green); --empty:var(--absent,); --short:var(--p,var(--short))"/>
    </svg>)svg");
  auto target = document.querySelector("#target");
  ASSERT_THAT(target, testing::Optional(testing::_));
  const auto& slot = ComputedFor(*target);
  ASSERT_THAT(slot.style, NotNull());
  ASSERT_THAT(slot.style->customProperties, NotNull());
  EXPECT_THAT(slot.style->customProperties->find("--a"), testing::IsNull());
  EXPECT_THAT(slot.style->customProperties->find("--b"), testing::IsNull());
  EXPECT_THAT(CustomIdent(*slot.style, "--c"), testing::Optional(Eq("green")));
  EXPECT_THAT(CustomIdent(*slot.style, "--short"), testing::Optional(Eq("available")));
  const auto* empty = slot.style->customProperties->find("--empty");
  ASSERT_THAT(empty, NotNull());
  EXPECT_THAT(empty->tokens(), testing::IsEmpty());
}

TEST_F(StyleSystemTest, CustomDeclarationsCascadeAndResolveReferences) {
  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>
        #target { --rank: id; --important: rule !important; --inline-rank: rule !important; }
        rect { --rank: type; --order: first; --order: last; }
      </style>
      <g id="parent" style="--invalid: parent; --inherit: parent; --unset: parent">
        <rect id="target" style="--normal:inline; --important:ignored; --inline-rank:inline !important;
          --invalid:initial; --inherit:inherit; --unset:unset; --empty:; --raw:var(--normal);
          --Name:Upper; --name:Lower; --: reserved"/>
        <g id="shares"/>
      </g>
    </svg>)svg");
  ParseWarningSink warnings;
  styleSystem.computeAllStyles(document.registry(), warnings);
  const auto target = document.querySelector("#target");
  const auto parent = document.querySelector("#parent");
  const auto shares = document.querySelector("#shares");
  ASSERT_THAT(target, testing::Optional(testing::_));
  ASSERT_THAT(parent, testing::Optional(testing::_));
  ASSERT_THAT(shares, testing::Optional(testing::_));
  const auto& targetStyle = ComputedFor(*target);
  ASSERT_THAT(targetStyle.style, NotNull());
  ASSERT_THAT(targetStyle.style->customProperties, NotNull());
  const auto& computed = *targetStyle.style;
  EXPECT_THAT(CustomIdent(computed, "--rank"), testing::Optional(Eq("id")));
  EXPECT_THAT(CustomIdent(computed, "--order"), testing::Optional(Eq("last")));
  EXPECT_THAT(CustomIdent(computed, "--normal"), testing::Optional(Eq("inline")));
  EXPECT_THAT(CustomIdent(computed, "--important"), testing::Optional(Eq("rule")));
  EXPECT_THAT(CustomIdent(computed, "--inline-rank"), testing::Optional(Eq("inline")));
  EXPECT_THAT(CustomIdent(computed, "--inherit"), testing::Optional(Eq("parent")));
  EXPECT_THAT(CustomIdent(computed, "--unset"), testing::Optional(Eq("parent")));
  EXPECT_THAT(CustomIdent(computed, "--Name"), testing::Optional(Eq("Upper")));
  EXPECT_THAT(CustomIdent(computed, "--name"), testing::Optional(Eq("Lower")));
  EXPECT_THAT(computed.customProperties->find("--invalid"), testing::IsNull());
  EXPECT_THAT(computed.customProperties->find("--"), testing::IsNull());
  const auto* empty = computed.customProperties->find("--empty");
  const auto* raw = computed.customProperties->find("--raw");
  ASSERT_THAT(empty, NotNull());
  ASSERT_THAT(raw, NotNull());
  EXPECT_THAT(empty->tokens(), testing::IsEmpty());
  ASSERT_THAT(raw->tokens(), testing::SizeIs(1u));
  EXPECT_THAT(CustomIdent(computed, "--raw"), testing::Optional(Eq("inline")));
  EXPECT_THAT(computed.properties.unparsedProperties, testing::IsEmpty());
  EXPECT_THAT(warnings.warnings(), testing::IsEmpty());

  const auto& parentStyle = ComputedFor(*parent);
  const auto& sharedStyle = ComputedFor(*shares);
  ASSERT_THAT(parentStyle.style, NotNull());
  ASSERT_THAT(parentStyle.style->customProperties, NotNull());
  ASSERT_THAT(sharedStyle.style, NotNull());
  EXPECT_THAT(sharedStyle.style->customProperties, Eq(parentStyle.style->customProperties));
}

TEST_F(StyleSystemTest, CustomInlineCacheKeysAndEditsPreserveExactValues) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" style="--tone: parent">
      <rect id="first" style="--tone: red !important"/>
      <rect id="different" style="--tone: blue"/>
      <rect id="same" style="--tone: red !important"/>
    </svg>)svg");
  auto first = document.querySelector("#first");
  const auto different = document.querySelector("#different");
  const auto same = document.querySelector("#same");
  ASSERT_THAT(first, testing::Optional(testing::_));
  ASSERT_THAT(different, testing::Optional(testing::_));
  ASSERT_THAT(same, testing::Optional(testing::_));
  const auto original = ComputedFor(*first).style;
  ASSERT_THAT(original, NotNull());
  ASSERT_THAT(ComputedFor(*different).style, NotNull());
  EXPECT_THAT(ComputedFor(*same).style, Eq(original));
  EXPECT_THAT(ComputedFor(*different).style, testing::Ne(original));
  EXPECT_THAT(CustomIdent(*ComputedFor(*different).style, "--tone"), testing::Optional(Eq("blue")));

  first->updateStyle("--tone: orange; --other: earlier !important; --other: later");
  const auto updatedSource = first->getAttribute("style");
  ASSERT_THAT(updatedSource, testing::Optional(testing::_));
  auto reparsed = ParseAndComputeStyles(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="element" style=")" +
      std::string(std::string_view(*updatedSource)) + R"("/></svg>)");
  const auto fromSource = reparsed.querySelector("#element");
  ASSERT_THAT(fromSource, testing::Optional(testing::_));
  ASSERT_THAT(ComputedFor(*fromSource).style, NotNull());
  std::ignore = first->getComputedStyle();
  const auto updated = ComputedFor(*first).style;
  ASSERT_THAT(updated, NotNull());
  EXPECT_THAT(CustomIdent(*updated, "--tone"), testing::Optional(Eq("red")));
  EXPECT_THAT(CustomIdent(*updated, "--other"), testing::Optional(Eq("earlier")));
  EXPECT_THAT(CustomIdent(*updated, "--tone"),
              Eq(CustomIdent(*ComputedFor(*fromSource).style, "--tone")));
  EXPECT_THAT(CustomIdent(*updated, "--other"),
              Eq(CustomIdent(*ComputedFor(*fromSource).style, "--other")));

  first->setStyle("--tone: inherit");
  std::ignore = first->getComputedStyle();
  ASSERT_THAT(ComputedFor(*first).style, NotNull());
  EXPECT_THAT(CustomIdent(*ComputedFor(*first).style, "--tone"), testing::Optional(Eq("parent")));
  EXPECT_THAT(CustomIdent(*ComputedFor(*first).style, "--other"), Eq(std::nullopt));
  EXPECT_THAT(CustomIdent(*original, "--tone"), testing::Optional(Eq("red")));
}

TEST_F(StyleSystemTest, UpdatingInlineCustomPropertiesKeepsCaseAndImportantPriority) {
  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" style="--Name: red !important; --name: blue"/>
    </svg>)svg");
  auto target = document.querySelector("#target");
  ASSERT_THAT(target, testing::Optional(testing::_));

  target->updateStyle("--Name: stale; --name: green !important; --name: also-stale");
  std::ignore = target->getComputedStyle();
  const auto first = ComputedFor(*target).style;
  ASSERT_THAT(first, NotNull());
  EXPECT_THAT(CustomIdent(*first, "--Name"), testing::Optional(Eq("red")));
  EXPECT_THAT(CustomIdent(*first, "--name"), testing::Optional(Eq("green")));

  target->updateStyle("--Name: gold !important");
  std::ignore = target->getComputedStyle();
  const auto second = ComputedFor(*target).style;
  ASSERT_THAT(second, NotNull());
  EXPECT_THAT(CustomIdent(*second, "--Name"), testing::Optional(Eq("gold")));
  EXPECT_THAT(CustomIdent(*second, "--name"), testing::Optional(Eq("green")));

  const auto source = target->getAttribute("style");
  ASSERT_THAT(source, testing::Optional(testing::_));
  EXPECT_THAT(std::string_view(*source),
              testing::AllOf(testing::HasSubstr("--Name: gold !important"),
                             testing::HasSubstr("--name: green !important"),
                             testing::Not(testing::HasSubstr("stale"))));
}

TEST_F(StyleSystemTest, ImportedCustomDeclarationsKeepPriorityAndOrder) {
  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style id="sheet">rect { --chosen: local; --order: local }</style>
      <rect id="target"/>
    </svg>)svg");
  const auto stylesheet = document.querySelector("#sheet");
  const auto target = document.querySelector("#target");
  ASSERT_THAT(stylesheet, testing::Optional(testing::_));
  ASSERT_THAT(target, testing::Optional(testing::_));
  ParseWarningSink warnings;
  std::vector<css::Stylesheet> imported;
  imported.push_back(css::CSS::ParseStylesheet(
      "rect { --chosen: imported !important; --order: imported }", warnings));
  stylesheet->entityHandle().get<StylesheetComponent>().setImportedStylesheets(std::move(imported),
                                                                               1);
  styleSystem.computeAllStyles(document.registry(), warnings);
  const auto& computed = ComputedFor(*target);
  ASSERT_THAT(computed.style, NotNull());
  EXPECT_THAT(CustomIdent(*computed.style, "--chosen"), testing::Optional(Eq("imported")));
  EXPECT_THAT(CustomIdent(*computed.style, "--order"), testing::Optional(Eq("local")));
  EXPECT_THAT(warnings.warnings(), testing::IsEmpty());
}

TEST_F(StyleSystemTest, OrdinaryVariablesChooseTheWinningDeclarationBeforeSubstitution) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>.paint { fill: var(--ink); }.owned { fill: var(--ink) !important; }</style>
      <g fill="red" style="--ink: #010203">
        <rect id="stylesheet" class="paint"/>
        <rect id="presentation" fill="var(--ink)"/>
        <rect id="inline" style="fill:var(--ink)"/>
        <rect id="invalid" style="fill:blue;fill:var(--missing)"/>
        <rect id="malformed" style="fill:blue;fill:var(no)"/>
        <rect id="later" style="fill:var(--ink);fill:blue"/>
        <rect id="important" class="owned" style="fill:blue"/>
        <rect id="opacity" style="opacity:.5;opacity:var(--missing)"/>
      </g>
    </svg>)svg");
  const auto color = [&](std::string_view name, css::RGBA expected) {
    SCOPED_TRACE(name);
    const auto element = document.querySelector(name);
    ASSERT_THAT(element, testing::Optional(testing::_));
    EXPECT_THAT(element->getComputedStyle().fill.get(),
                testing::Optional(Eq(PaintServer::Solid(css::Color(expected)))));
  };
  constexpr auto ink = css::RGBA::RGB(1, 2, 3);
  color("#stylesheet", ink);
  color("#presentation", ink);
  color("#inline", ink);
  color("#important", ink);
  color("#invalid", css::RGBA::RGB(255, 0, 0));
  color("#malformed", css::RGBA::RGB(0, 0, 255));
  color("#later", css::RGBA::RGB(0, 0, 255));

  const auto opacity = document.querySelector("#opacity");
  ASSERT_THAT(opacity, testing::Optional(testing::_));
  EXPECT_THAT(opacity->getComputedStyle().opacity.get(), testing::Optional(Eq(1.0)));
}

TEST_F(StyleSystemTest, SelectedFallbackWhitespaceIsIgnoredAtDeclarationBoundaries) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" style="opacity:var(--missing, 0.5)"/>
    </svg>)svg");
  const auto target = document.querySelector("#target");
  ASSERT_THAT(target, testing::Optional(testing::_));
  EXPECT_THAT(target->getComputedStyle().opacity.get(), testing::Optional(Eq(0.5)));
}

// Adapted from CSS Variables WPT var-fallback-braces.html for SVG computed styles.
TEST_F(StyleSystemTest, BracedVariableFallbacksUnwrapAndRejectTrailingTokens) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>
        #tight { fill:var(--missing,{rgb(1,2,3)}); }
        #inside { fill:var(--missing, { rgb(1,2,3) }); }
        #outside { fill:var(--missing, {rgb(1,2,3)} ); }
        #invalid { fill:blue; fill:var(--missing,{rgb(1,2,3)} rgb(4,5,6)); }
        #family { font-family:var(--missing,{Arial, sans-serif}); }
        #invalid-family { font-family:monospace; font-family:var(--missing,{Arial}, sans-serif); }
      </style>
      <rect id="tight"/><rect id="inside"/><rect id="outside"/><rect id="invalid"/>
      <text id="family"/><text id="invalid-family"/>
    </svg>)svg");
  const auto color = [&](std::string_view selector, css::RGBA expected) {
    SCOPED_TRACE(selector);
    const auto element = document.querySelector(selector);
    ASSERT_THAT(element, testing::Optional(testing::_));
    EXPECT_THAT(element->getComputedStyle().fill.get(),
                testing::Optional(Eq(PaintServer::Solid(css::Color(expected)))));
  };
  constexpr auto ink = css::RGBA::RGB(1, 2, 3);
  color("#tight", ink);
  color("#inside", ink);
  color("#outside", ink);
  color("#invalid", css::RGBA::RGB(0, 0, 255));

  const auto family = document.querySelector("#family");
  const auto invalidFamily = document.querySelector("#invalid-family");
  ASSERT_THAT(family, testing::Optional(testing::_));
  ASSERT_THAT(invalidFamily, testing::Optional(testing::_));
  EXPECT_THAT(family->getComputedStyle().fontFamily.get(),
              testing::Optional(ElementsAre(RcString("Arial"), RcString("sans-serif"))));
  EXPECT_THAT(invalidFamily->getComputedStyle().fontFamily.get(),
              testing::Optional(ElementsAre(RcString("monospace"))));
}

TEST_F(StyleSystemTest, InvalidSubstitutedResourceOpacityShadowsPresentationValue) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" style="--bad:red">
      <defs>
        <linearGradient id="gradient">
          <stop id="stop" stop-opacity="0.25" style="stop-opacity:var(--bad)"/>
        </linearGradient>
        <filter id="filter">
          <feFlood id="flood" flood-opacity="0.375" style="flood-opacity:var(--bad)"/>
        </filter>
      </defs>
    </svg>)svg");
  const auto stop = document.querySelector("#stop");
  const auto flood = document.querySelector("#flood");
  ASSERT_THAT(stop, testing::Optional(testing::_));
  ASSERT_THAT(flood, testing::Optional(testing::_));
  const auto* stopOpacity = ComputedFor(*stop).resourceOpacities();
  const auto* floodOpacity = ComputedFor(*flood).resourceOpacities();
  ASSERT_THAT(stopOpacity, NotNull());
  ASSERT_THAT(floodOpacity, NotNull());
  EXPECT_EQ(stopOpacity->stopOpacity, 1.0);
  EXPECT_EQ(floodOpacity->floodOpacity, 1.0);
}

TEST_F(StyleSystemTest, DeferredTransformRetainsImportantPriorityAgainstVariables) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" style="--offset:translate(20px)">
      <rect id="important-concrete"
            style="transform:translate(10px) !important;transform:var(--offset)"/>
      <rect id="important-variable"
            style="transform:translate(10px) !important;transform:var(--offset) !important"/>
    </svg>)svg");
  const auto concrete = document.querySelector("#important-concrete");
  const auto variable = document.querySelector("#important-variable");
  ASSERT_THAT(concrete, testing::Optional(testing::_));
  ASSERT_THAT(variable, testing::Optional(testing::_));
  EXPECT_THAT(concrete->cast<SVGRectElement>().transform(), Eq(Transform2d::Translate(10, 0)));
  EXPECT_THAT(variable->cast<SVGRectElement>().transform(), Eq(Transform2d::Translate(20, 0)));
}

TEST_F(StyleSystemTest, SubstitutedFilterKeepsPresentationUserUnits) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" style="--radius:2">
      <style>rect { filter:blur(var(--radius)); }</style>
      <path id="direct" filter="blur(2)"/>
      <path id="variable" filter="blur(var(--radius))"/>
      <path id="inline" style="filter:blur(var(--radius))"/>
      <rect id="rule"/>
    </svg>)svg");
  const auto direct = document.querySelector("#direct");
  const auto variable = document.querySelector("#variable");
  const auto inlineStyle = document.querySelector("#inline");
  const auto rule = document.querySelector("#rule");
  ASSERT_THAT(direct, testing::Optional(testing::_));
  ASSERT_THAT(variable, testing::Optional(testing::_));
  ASSERT_THAT(inlineStyle, testing::Optional(testing::_));
  ASSERT_THAT(rule, testing::Optional(testing::_));
  const auto* directStyle = ComputedFor(*direct).properties();
  const auto* variableStyle = ComputedFor(*variable).properties();
  const auto* inlineProperties = ComputedFor(*inlineStyle).properties();
  const auto* ruleStyle = ComputedFor(*rule).properties();
  ASSERT_THAT(directStyle, NotNull());
  ASSERT_THAT(variableStyle, NotNull());
  ASSERT_THAT(inlineProperties, NotNull());
  ASSERT_THAT(ruleStyle, NotNull());
  ASSERT_THAT(directStyle->filter.get(), testing::Optional(testing::SizeIs(1)));
  EXPECT_THAT(variableStyle->filter.get(), Eq(directStyle->filter.get()));
  EXPECT_THAT(inlineProperties->filter.get(), testing::Optional(testing::IsEmpty()));
  EXPECT_THAT(ruleStyle->filter.get(), testing::Optional(testing::IsEmpty()));
}

TEST_F(StyleSystemTest, PresentationVariableEditsRetainOnlyTheCurrentAttribute) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" fill="green" style="--ink: red; --other: blue">
      <rect id="target" fill="var(--ink)"/>
    </svg>)svg");
  auto target = document.querySelector("#target");
  ASSERT_THAT(target, testing::Optional(testing::_));
  const auto fill = [&](css::RGBA expected) {
    EXPECT_THAT(target->getComputedStyle().fill.get(),
                testing::Optional(Eq(PaintServer::Solid(css::Color(expected)))));
  };
  fill(css::RGBA::RGB(255, 0, 0));

  target->setStyle("fill: blue");
  fill(css::RGBA::RGB(0, 0, 255));
  target->setStyle("");
  fill(css::RGBA::RGB(255, 0, 0));
  target->setStyle("fill: var(--other)");
  fill(css::RGBA::RGB(0, 0, 255));
  target->setStyle("");
  fill(css::RGBA::RGB(255, 0, 0));

  target->setAttribute("fill", "blue");
  fill(css::RGBA::RGB(0, 0, 255));
  target->setAttribute("fill", "var(--ink)");
  fill(css::RGBA::RGB(255, 0, 0));
  target->removeAttribute("fill");
  fill(css::RGBA::RGB(0, 128, 0));
}

TEST_F(StyleSystemTest, PresentationStrokeWidthUsesInheritedCustomProperty) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" style="--inherited-width: 5px">
      <g><rect id="target" stroke-width="var(--inherited-width)"/></g>
    </svg>)svg");
  const auto target = document.querySelector("#target");
  ASSERT_THAT(target, testing::Optional(testing::_));
  const auto* properties = ComputedFor(*target).properties();
  ASSERT_THAT(properties, NotNull());
  EXPECT_THAT(properties->strokeWidth.get(), testing::Optional(Eq(Lengthd(5, Lengthd::Unit::Px))));
}

TEST_F(StyleSystemTest, HiddenPresentationVariablesTrackReplacementAndRemoval) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" style="--ink:red;--next:green;fill:blue"/>
    </svg>)svg");
  auto target = document.querySelector("#target");
  ASSERT_THAT(target, testing::Optional(testing::_));
  const auto expectFill = [&](css::RGBA color) {
    EXPECT_THAT(target->getComputedStyle().fill.get(),
                testing::Optional(Eq(PaintServer::Solid(css::Color(color)))));
  };

  target->setAttribute("fill", "var(--ink)");
  target->setAttribute("fill", "var(--next)");
  expectFill(css::RGBA::RGB(0, 0, 255));
  target->setStyle("--ink:red;--next:green");
  expectFill(css::RGBA::RGB(0, 128, 0));

  target->setStyle("--ink:red;--next:green;fill:blue");
  target->setAttribute("fill", "var(--ink)");
  target->removeAttribute("fill");
  expectFill(css::RGBA::RGB(0, 0, 255));
  target->setStyle("--ink:red;--next:green");
  expectFill(css::RGBA::RGB(0, 0, 0));

  target->setStyle("--ink:red;--next:green;fill:var(--next)");
  target->setAttribute("fill", "var(--ink)");
  expectFill(css::RGBA::RGB(0, 128, 0));
  target->setStyle("--ink:red;--next:green");
  expectFill(css::RGBA::RGB(255, 0, 0));
}

TEST_F(StyleSystemTest, MarkerVariablesDiscoverAndOrderTheirLonghands) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" style="--m:url(#arrow)">
      <path id="target" style="marker:var(--m);marker-end:url(#other)"/>
      <path id="invalid" style="marker:url(#other);marker:var(--absent)"/>
      <g style="marker:url(#arrow)">
        <path id="inherited" style="marker:initial;marker:inherit"/>
      </g>
    </svg>)svg");
  const auto target = document.querySelector("#target");
  const auto invalid = document.querySelector("#invalid");
  const auto inherited = document.querySelector("#inherited");
  ASSERT_THAT(target, testing::Optional(testing::_));
  ASSERT_THAT(invalid, testing::Optional(testing::_));
  ASSERT_THAT(inherited, testing::Optional(testing::_));

  const Reference arrow(RcString("#arrow"));
  const Reference other(RcString("#other"));
  const auto& property = target->getComputedStyle();
  EXPECT_THAT(property.markerStart.get(), testing::Optional(Eq(arrow)));
  EXPECT_THAT(property.markerMid.get(), testing::Optional(Eq(arrow)));
  EXPECT_THAT(property.markerEnd.get(), testing::Optional(Eq(other)));

  const auto& invalidProperty = invalid->getComputedStyle();
  EXPECT_THAT(invalidProperty.markerStart.get(), Eq(std::nullopt));
  EXPECT_THAT(invalidProperty.markerMid.get(), Eq(std::nullopt));
  EXPECT_THAT(invalidProperty.markerEnd.get(), Eq(std::nullopt));

  const auto& inheritedProperty = inherited->getComputedStyle();
  EXPECT_THAT(inheritedProperty.markerStart.get(), testing::Optional(Eq(arrow)));
  EXPECT_THAT(inheritedProperty.markerMid.get(), testing::Optional(Eq(arrow)));
  EXPECT_THAT(inheritedProperty.markerEnd.get(), testing::Optional(Eq(arrow)));
}

TEST_F(StyleSystemTest, InlineVariableEditsKeepTheWinningPriorityInSource) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" style="--ink: red">
      <rect id="target" style="fill:var(--ink) !important"/>
    </svg>)svg");
  auto target = document.querySelector("#target");
  ASSERT_THAT(target, testing::Optional(testing::_));
  target->updateStyle("fill: blue; opacity: var(no)");
  const auto source = target->getAttribute("style");
  ASSERT_THAT(source, testing::Optional(testing::_));
  EXPECT_THAT(std::string(std::string_view(*source)), testing::HasSubstr("!important"));
  EXPECT_THAT(std::string(std::string_view(*source)), testing::Not(testing::HasSubstr("opacity")));
  EXPECT_THAT(target->getComputedStyle().fill.get(),
              testing::Optional(Eq(PaintServer::Solid(css::Color(css::RGBA::RGB(255, 0, 0))))));
}

TEST_F(StyleSystemTest, ResourceColorVariablesKeepAuthoredPresentationOrigins) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" style="--ink:red">
      <defs><linearGradient id="paint">
        <stop id="target" stop-color="green" style="stop-color:var(--ink)"/>
      </linearGradient></defs>
    </svg>)svg");
  auto stop = document.querySelector("#target");
  ASSERT_THAT(stop, testing::Optional(testing::_));
  const auto color = [&](css::RGBA expected) {
    EXPECT_THAT(stop->getComputedStyle().stopColor.get(),
                testing::Optional(Eq(css::Color(expected))));
  };
  color(css::RGBA::RGB(255, 0, 0));
  stop->setStyle("");
  color(css::RGBA::RGB(0, 128, 0));
  stop->setStyle("stop-color: blue");
  stop->setAttribute("stop-color", "var(--ink)");
  color(css::RGBA::RGB(0, 0, 255));
  stop->setStyle("");
  color(css::RGBA::RGB(255, 0, 0));
}

TEST_F(StyleSystemTest, SubstitutedRectangleGeometryKeepsPresentationUnits) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" style="--wide:18">
      <style>.stylesheet { width: var(--wide); }</style>
      <rect id="direct" width="18" height="10"/>
      <rect id="variable" width="var(--wide)" height="10"/>
      <rect id="strict" class="stylesheet" height="10"/>
    </svg>)svg");
  const auto direct = document.querySelector("#direct");
  auto variable = document.querySelector("#variable");
  const auto strict = document.querySelector("#strict");
  ASSERT_THAT(direct, testing::Optional(testing::_));
  ASSERT_THAT(variable, testing::Optional(testing::_));
  ASSERT_THAT(strict, testing::Optional(testing::_));
  const auto directRect = direct->cast<SVGRectElement>();
  const auto variableRect = variable->cast<SVGRectElement>();
  ASSERT_EQ(directRect.computedWidth().value, 18.0);
  EXPECT_EQ(variableRect.computedWidth().value, directRect.computedWidth().value);
  EXPECT_EQ(strict->cast<SVGRectElement>().computedWidth().value, 0.0);

  variable->setAttribute("width", "6");
  EXPECT_EQ(variableRect.computedWidth().value, 6.0);
  variable->setAttribute("width", "var(--wide)");
  EXPECT_EQ(variableRect.computedWidth().value, 18.0);
  variable->removeAttribute("width");
  EXPECT_EQ(variableRect.computedWidth().value, 0.0);
}

TEST_F(StyleSystemTest, SubstitutedPresentationTransformsUseSvgGrammar) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" style="--x:18;--pair:8 5;--pivot:30 7 2">
      <style>.strict { transform:translate(var(--x)); }</style>
      <rect id="literal-unit" transform="translate(18)"/>
      <rect id="variable-unit" transform="translate(var(--x))"/>
      <rect id="literal-combined" transform="translate(8 5),rotate(30 7 2)"/>
      <rect id="variable-combined" transform="translate(var(--pair)),rotate(var(--pivot))"/>
      <rect id="strict" class="strict"/>
    </svg>)svg");

  const auto directUnit = document.querySelector("#literal-unit");
  const auto variableUnit = document.querySelector("#variable-unit");
  const auto directCombined = document.querySelector("#literal-combined");
  const auto variableCombined = document.querySelector("#variable-combined");
  const auto strict = document.querySelector("#strict");
  ASSERT_THAT(directUnit, testing::Optional(testing::_));
  ASSERT_THAT(variableUnit, testing::Optional(testing::_));
  ASSERT_THAT(directCombined, testing::Optional(testing::_));
  ASSERT_THAT(variableCombined, testing::Optional(testing::_));
  ASSERT_THAT(strict, testing::Optional(testing::_));
  EXPECT_THAT(directUnit->cast<SVGRectElement>().transform(), Eq(Transform2d::Translate(18, 0)));
  EXPECT_THAT(variableUnit->cast<SVGRectElement>().transform(),
              Eq(directUnit->cast<SVGRectElement>().transform()));
  EXPECT_THAT(variableCombined->cast<SVGRectElement>().transform(),
              Eq(directCombined->cast<SVGRectElement>().transform()));
  EXPECT_THAT(strict->cast<SVGRectElement>().transform(), Eq(Transform2d()));
}

TEST_F(StyleSystemTest, TransformVariableEditsRefreshImmediateGeometry) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" style="--position:translate(20px)">
      <rect id="target" width="10" height="10" transform="var(--position)"/>
    </svg>)svg");
  auto target = document.querySelector("#target");
  ASSERT_THAT(target, testing::Optional(testing::_));
  const auto rectangle = target->cast<SVGRectElement>();
  EXPECT_THAT(rectangle.transform(), Eq(Transform2d::Translate(20, 0)));
  target->removeAttribute("transform");
  EXPECT_THAT(rectangle.transform(), Eq(Transform2d()));

  target->setAttribute("transform", "translate(7)");
  EXPECT_THAT(rectangle.transform(), Eq(Transform2d::Translate(7, 0)));
  target->setAttribute("transform", "var(--position)");
  EXPECT_THAT(rectangle.transform(), Eq(Transform2d::Translate(20, 0)));
  target->setAttribute("transform", "translate(9)");
  EXPECT_THAT(rectangle.transform(), Eq(Transform2d::Translate(9, 0)));

  target->setStyle("transform:var(--position)");
  EXPECT_THAT(rectangle.transform(), Eq(Transform2d::Translate(20, 0)));
  target->setStyle("");
  EXPECT_THAT(rectangle.transform(), Eq(Transform2d::Translate(9, 0)));
}

TEST_F(StyleSystemTest, InvalidSubstitutedWidthDoesNotExposeAnEarlierValue) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" style="--wide:18px; --invalid:red">
      <rect id="target" width="8" height="8" style="width:var(--wide)"/>
    </svg>)svg");
  auto target = document.querySelector("#target");
  ASSERT_THAT(target, testing::Optional(testing::_));
  auto rectangle = target->cast<SVGRectElement>();
  EXPECT_EQ(rectangle.computedWidth().value, 18.0);

  target->setStyle("width:var(--invalid)");
  EXPECT_EQ(rectangle.computedWidth().value, 0.0);

  target->setStyle("");
  target->setAttribute("width", "var(--wide)");
  EXPECT_EQ(rectangle.computedWidth().value, 18.0);
  target->setAttribute("width", "6");
  EXPECT_EQ(rectangle.computedWidth().value, 6.0);
}

TEST_F(StyleSystemTest, ExhaustedSubstitutionWorkResetsEveryWinnerAndWarnsOnce) {
  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" style="fill:blue;fill:var(--missing,red);
                               stroke:blue;stroke:var(--missing,red)"/>
    </svg>)svg");
  auto& registry = document.registry();
  for (auto entity : registry.view<StylesheetComponent>()) {
    ASSERT_THAT(registry.get<StylesheetComponent>(entity).isUserAgentStylesheet, testing::IsTrue());
  }
  registry.clear<StylesheetComponent>();
  StyleResourceBudget::Limits limits;
  limits.declarationComponentWork = 0;
  registry.ctx().emplace<StyleResourceBudget>(limits);
  ParseWarningSink warnings;
  styleSystem.computeAllStyles(registry, warnings);
  const auto target = document.querySelector("#target");
  ASSERT_THAT(target, testing::Optional(testing::_));
  const auto& properties = target->getComputedStyle();
  EXPECT_THAT(properties.fill.get(),
              testing::Optional(Eq(PaintServer::Solid(css::Color(css::RGBA::RGB(0, 0, 0))))));
  EXPECT_THAT(properties.stroke.get(), testing::Optional(Eq(PaintServer::None())));
  ASSERT_THAT(warnings.warnings(), testing::SizeIs(1u));
  EXPECT_THAT(std::string_view(warnings.warnings().front().reason),
              testing::HasSubstr("CSS variable expansion"));
  EXPECT_EQ(registry.ctx().get<StyleResourceBudget>().declarationApplications(), 0u);
}

TEST_F(StyleSystemTest, PendingShorthandOwnsOneTokenSequenceAndExpansion) {
  PropertyRegistry properties;
  ASSERT_THAT(properties.parseStyle("font:var(--face);font-size:19px"), testing::IsTrue());
  const auto italic = properties.unparsedProperties.find(RcString("font-style"));
  const auto weight = properties.unparsedProperties.find(RcString("font-weight"));
  ASSERT_NE(italic, properties.unparsedProperties.end());
  ASSERT_NE(weight, properties.unparsedProperties.end());
  ASSERT_THAT(italic->second.variableSource, NotNull());
  EXPECT_THAT(weight->second.variableSource, Eq(italic->second.variableSource));

  const auto value = css::parser::ValueParser::Parse("italic 28px serif");
  std::size_t chargedComponents = 0;
  EXPECT_THAT(
      properties.resolveVariableDeclarations(
          [&](std::string_view name) -> std::optional<std::span<const css::ComponentValue>> {
            return name == "--face" ? std::optional(std::span(value)) : std::nullopt;
          },
          [&](std::size_t components, std::size_t) {
            chargedComponents += components;
            return true;
          }),
      Eq(std::nullopt));
  EXPECT_EQ(chargedComponents, value.size());
  EXPECT_THAT(properties.fontStyle.get(), testing::Optional(Eq(FontStyle::Italic)));
  EXPECT_THAT(properties.fontSize.get(), testing::Optional(Eq(Lengthd(19, Lengthd::Unit::Px))));
}

TEST_F(StyleSystemTest, ShorthandSubstitutionDoesNotReplaceALaterLonghand) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" style="--face: italic 28px serif">
      <text id="text" style="font:var(--face);font-size:19px">Content</text>
    </svg>)svg");
  const auto text = document.querySelector("#text");
  ASSERT_THAT(text, testing::Optional(testing::_));
  const auto& style = text->getComputedStyle();
  EXPECT_THAT(style.fontSize.get(), testing::Optional(Eq(Lengthd(19, Lengthd::Unit::Px))));
  EXPECT_THAT(style.fontStyle.get(), testing::Optional(Eq(FontStyle::Italic)));
}

}  // namespace donner::svg::components
