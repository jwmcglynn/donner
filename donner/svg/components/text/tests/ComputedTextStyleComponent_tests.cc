#include "donner/svg/components/text/ComputedTextStyleComponent.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/components/text/TextSystem.h"
#include "donner/svg/parser/SVGParser.h"

using testing::ElementsAre;
using testing::ElementsAreArray;

namespace donner::svg::components {

namespace {

parser::SVGParser::Options ExperimentalTextOptions() {
  parser::SVGParser::Options options;
  options.enableExperimental = true;
  return options;
}

SVGDocument ParseWithExperimentalText(std::string_view input) {
  auto maybeDoc = parser::SVGParser::ParseSVG(input, nullptr, ExperimentalTextOptions());
  EXPECT_THAT(maybeDoc, NoParseError());
  return std::move(maybeDoc).result();
}

}  // namespace

TEST(ComputedTextStyleComponentTests, PopulatesTypographyFromComputedStyle) {
  auto document = ParseWithExperimentalText(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <text id="root"
            font-family="Inter, serif"
            font-style="italic"
            font-weight="700"
            font-stretch="condensed"
            font-variant="small-caps"
            font-size="14px"
            letter-spacing="0.5px"
            word-spacing="2%"
            text-anchor="middle"
            white-space="pre-wrap"
            direction="rtl">
        Hello
      </text>
    </svg>
  )");

  StyleSystem().computeAllStyles(document.registry(), nullptr);
  TextSystem().instantiateAllComputedComponents(document.registry(), nullptr);

  const EntityHandle handle = document.querySelector("#root")->entityHandle();
  const auto* textStyle = document.registry().try_get<ComputedTextStyleComponent>(handle.entity());
  ASSERT_NE(textStyle, nullptr);

  EXPECT_THAT(textStyle->fontFamily, ElementsAre(RcString("Inter"), RcString("serif")));
  EXPECT_EQ(textStyle->fontStyle, FontStyle::Italic);
  EXPECT_EQ(textStyle->fontWeight, FontWeight::Number(700));
  EXPECT_EQ(textStyle->fontStretch, FontStretch::Condensed);
  EXPECT_EQ(textStyle->fontVariant, FontVariant::SmallCaps);
  EXPECT_THAT(textStyle->fontSize, LengthIs(14.0, Lengthd::Unit::Px));
  EXPECT_EQ(textStyle->letterSpacing, TextSpacing::Length(Lengthd(0.5, Lengthd::Unit::Px)));
  EXPECT_EQ(textStyle->wordSpacing, TextSpacing::Length(Lengthd(2.0, Lengthd::Unit::Percent)));
  EXPECT_EQ(textStyle->textAnchor, TextAnchor::Middle);
  EXPECT_EQ(textStyle->whiteSpace, WhiteSpace::PreWrap);
  EXPECT_EQ(textStyle->direction, Direction::Rtl);
}

TEST(ComputedTextStyleComponentTests, InheritsTypographyIntoChildSpans) {
  auto document = ParseWithExperimentalText(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <text id="parent" font-family="Example" font-size="20px" font-style="oblique">
        <tspan id="child" font-weight="bold" font-size="150%">Child</tspan>
      </text>
    </svg>
  )");

  StyleSystem().computeAllStyles(document.registry(), nullptr);
  TextSystem().instantiateAllComputedComponents(document.registry(), nullptr);

  const EntityHandle childHandle = document.querySelector("#child")->entityHandle();
  const auto* textStyle =
      document.registry().try_get<ComputedTextStyleComponent>(childHandle.entity());
  ASSERT_NE(textStyle, nullptr);

  EXPECT_THAT(textStyle->fontFamily, ElementsAre(RcString("Example")));
  EXPECT_EQ(textStyle->fontStyle, FontStyle::Oblique);
  EXPECT_EQ(textStyle->fontWeight, FontWeight::Bold());
  EXPECT_THAT(textStyle->fontSize, LengthIs(150.0, Lengthd::Unit::Percent));
}

TEST(ComputedTextStyleComponentTests, CopiesTypographyIntoComputedSpans) {
  auto document = ParseWithExperimentalText(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <text id="parent" font-family="Serif" font-size="10px" font-style="italic">
        Parent
        <tspan id="child" font-family="Example" font-weight="700" font-size="25px">Child</tspan>
      </text>
    </svg>
  )");

  StyleSystem().computeAllStyles(document.registry(), nullptr);
  TextSystem().instantiateAllComputedComponents(document.registry(), nullptr);

  const EntityHandle rootHandle = document.querySelector("#parent")->entityHandle();
  const auto* computedText =
      document.registry().try_get<ComputedTextComponent>(rootHandle.entity());
  ASSERT_NE(computedText, nullptr);
  ASSERT_EQ(computedText->spans.size(), 2u);

  EXPECT_THAT(computedText->spans[0].style.fontFamily, ElementsAre(RcString("Serif")));
  EXPECT_THAT(computedText->spans[1].style.fontFamily, ElementsAre(RcString("Example")));
  EXPECT_EQ(computedText->spans[0].style.fontStyle, FontStyle::Italic);
  EXPECT_THAT(computedText->spans[0].style.fontSize, LengthIs(10.0, Lengthd::Unit::Px));

  EXPECT_EQ(computedText->spans[1].style.fontWeight, FontWeight::Number(700));
  EXPECT_THAT(computedText->spans[1].style.fontSize, LengthIs(25.0, Lengthd::Unit::Px));
}

TEST(ComputedTextStyleComponentTests, InheritsFontAndPositionUnitsFromSvgRoot) {
  auto document = ParseWithExperimentalText(R"(
    <svg xmlns="http://www.w3.org/2000/svg" font-family="Noto Sans" font-size="64">
      <text id="target" x="0.5em" y="3.1ex">Text</text>
    </svg>
  )");

  const EntityHandle root = document.rootEntityHandle();
  const auto* attributes =
      document.registry().try_get<donner::components::AttributesComponent>(root.entity());
  ASSERT_NE(attributes, nullptr);
  EXPECT_TRUE(attributes->hasAttribute(xml::XMLQualifiedNameRef("font-family")));
  const auto* styleComponent = document.registry().try_get<StyleComponent>(root.entity());
  ASSERT_NE(styleComponent, nullptr);
  ASSERT_TRUE(styleComponent->properties.fontFamily.hasValue());
  EXPECT_THAT(styleComponent->properties.fontFamily.getRequired(),
              ElementsAre(RcString("Noto Sans")));

  StyleSystem().computeAllStyles(document.registry(), nullptr);
  TextSystem().instantiateAllComputedComponents(document.registry(), nullptr);

  const auto* rootStyle = document.registry().try_get<ComputedStyleComponent>(root.entity());
  ASSERT_NE(rootStyle, nullptr);
  ASSERT_TRUE(rootStyle->properties);
  EXPECT_THAT(rootStyle->properties->fontFamily.getRequired(), ElementsAre(RcString("Noto Sans")));
  EXPECT_THAT(rootStyle->properties->fontSize.getRequired(), LengthIs(64.0, Lengthd::Unit::None));

  const EntityHandle handle = document.querySelector("#target")->entityHandle();
  const auto* textStyle = document.registry().try_get<ComputedTextStyleComponent>(handle.entity());
  ASSERT_NE(textStyle, nullptr);

  EXPECT_THAT(textStyle->fontFamily, ElementsAre(RcString("Noto Sans")));
  EXPECT_THAT(textStyle->fontSize, LengthIs(64.0, Lengthd::Unit::None));

  const auto* computedText = document.registry().try_get<ComputedTextComponent>(handle.entity());
  ASSERT_NE(computedText, nullptr);
  ASSERT_EQ(computedText->spans.size(), 1u);

  const auto& span = computedText->spans[0];
  ASSERT_EQ(span.x.size(), 1u);
  ASSERT_EQ(span.y.size(), 1u);
  EXPECT_THAT(span.x[0], LengthIs(0.5, Lengthd::Unit::Em));
  EXPECT_THAT(span.y[0], LengthIs(3.1, Lengthd::Unit::Ex));
}

}  // namespace donner::svg::components
