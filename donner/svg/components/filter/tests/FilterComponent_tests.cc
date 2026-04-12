#include "donner/svg/components/filter/FilterEffect.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg::components {
namespace {

using css::Color;
using css::RGBA;
using testing::Optional;

TEST(FilterEffectTest, EqualityAndOstreamOutput) {
  const FilterEffect blur(FilterEffect::Blur{Lengthd(1, Lengthd::Unit::Px),
                                             Lengthd(2, Lengthd::Unit::Px)});
  const FilterEffect reference(FilterEffect::ElementReference(Reference("#f")));
  const FilterEffect hueRotate(FilterEffect::HueRotate{45.0});
  const FilterEffect brightness(FilterEffect::Brightness{0.5});
  const FilterEffect contrast(FilterEffect::Contrast{2.0});
  const FilterEffect grayscale(FilterEffect::Grayscale{0.25});
  const FilterEffect invert(FilterEffect::Invert{0.75});
  const FilterEffect opacity(FilterEffect::FilterOpacity{0.4});
  const FilterEffect saturate(FilterEffect::Saturate{3.0});
  const FilterEffect sepia(FilterEffect::Sepia{1.0});
  const FilterEffect shadow(FilterEffect::DropShadow{
      Lengthd(1, Lengthd::Unit::Px), Lengthd(2, Lengthd::Unit::Px),
      Lengthd(3, Lengthd::Unit::Px), Color(RGBA(0xFF, 0, 0, 0xFF))});

  EXPECT_EQ(blur, FilterEffect(FilterEffect::Blur{Lengthd(1, Lengthd::Unit::Px),
                                                  Lengthd(2, Lengthd::Unit::Px)}));
  EXPECT_NE(blur, hueRotate);

  EXPECT_THAT(FilterEffect(FilterEffect::None{}), ToStringIs("FilterEffect(none)"));
  EXPECT_THAT(blur, ToStringIs("FilterEffect(blur(1px 2px))"));
  EXPECT_THAT(reference, ToStringIs("FilterEffect(url(#f))"));
  EXPECT_THAT(hueRotate, ToStringIs("FilterEffect(hue-rotate(45deg))"));
  EXPECT_THAT(brightness, ToStringIs("FilterEffect(brightness(0.5))"));
  EXPECT_THAT(contrast, ToStringIs("FilterEffect(contrast(2))"));
  EXPECT_THAT(grayscale, ToStringIs("FilterEffect(grayscale(0.25))"));
  EXPECT_THAT(invert, ToStringIs("FilterEffect(invert(0.75))"));
  EXPECT_THAT(opacity, ToStringIs("FilterEffect(opacity(0.4))"));
  EXPECT_THAT(saturate, ToStringIs("FilterEffect(saturate(3))"));
  EXPECT_THAT(sepia, ToStringIs("FilterEffect(sepia(1))"));
  EXPECT_THAT(shadow, ToStringIs("FilterEffect(drop-shadow(1 2 3))"));
  const std::vector<FilterEffect> effects{blur, reference};
  EXPECT_THAT(effects, ToStringIs("[FilterEffect(blur(1px 2px)), FilterEffect(url(#f))]"));
}

TEST(FilterPrimitiveComponentTest, ParseFeFloodPresentationAttribute) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);

  EXPECT_THAT(ParseFeFloodPresentationAttribute(
                  rect.entityHandle(), "flood-color",
                  parser::PropertyParseFnParams::CreateForAttribute("red")),
              ParseResultIs(true));
  EXPECT_THAT(ParseFeFloodPresentationAttribute(
                  rect.entityHandle(), "flood-opacity",
                  parser::PropertyParseFnParams::CreateForAttribute("50%")),
              ParseResultIs(true));
  EXPECT_THAT(ParseFeFloodPresentationAttribute(
                  rect.entityHandle(), "unknown",
                  parser::PropertyParseFnParams::CreateForAttribute("red")),
              ParseResultIs(false));

  const auto& component = rect.entityHandle().get<FEFloodComponent>();
  EXPECT_THAT(component.floodColor.get(), Optional(Color(RGBA(0xFF, 0, 0, 0xFF))));
  EXPECT_THAT(component.floodOpacity.get(), Optional(0.5));
}

TEST(FilterPrimitiveComponentTest, ParseFeFloodPresentationAttributeErrors) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);

  EXPECT_THAT(ParseFeFloodPresentationAttribute(
                  rect.entityHandle(), "flood-color",
                  parser::PropertyParseFnParams::CreateForAttribute("bogus")),
              ParseErrorIs("Invalid color 'bogus'"));
  EXPECT_THAT(ParseFeFloodPresentationAttribute(
                  rect.entityHandle(), "flood-opacity",
                  parser::PropertyParseFnParams::CreateForAttribute("bogus")),
              ParseErrorIs("Invalid alpha value"));
}

TEST(FilterPrimitiveComponentTest, ParseFeDropShadowPresentationAttribute) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);

  EXPECT_THAT(ParseFeDropShadowPresentationAttribute(
                  rect.entityHandle(), "flood-color",
                  parser::PropertyParseFnParams::CreateForAttribute("lime")),
              ParseResultIs(true));
  EXPECT_THAT(ParseFeDropShadowPresentationAttribute(
                  rect.entityHandle(), "flood-opacity",
                  parser::PropertyParseFnParams::CreateForAttribute("0.25")),
              ParseResultIs(true));

  const auto& component = rect.entityHandle().get<FEDropShadowComponent>();
  EXPECT_THAT(component.floodColor.get(), Optional(Color(RGBA(0, 0xFF, 0, 0xFF))));
  EXPECT_THAT(component.floodOpacity.get(), Optional(0.25));
}

TEST(FilterPrimitiveComponentTest, ParseFeDropShadowPresentationAttributeErrors) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);

  EXPECT_THAT(ParseFeDropShadowPresentationAttribute(
                  rect.entityHandle(), "flood-color",
                  parser::PropertyParseFnParams::CreateForAttribute("bogus")),
              ParseErrorIs("Invalid color 'bogus'"));
  EXPECT_THAT(ParseFeDropShadowPresentationAttribute(
                  rect.entityHandle(), "flood-opacity",
                  parser::PropertyParseFnParams::CreateForAttribute("bogus")),
              ParseErrorIs("Invalid alpha value"));
  EXPECT_THAT(ParseFeDropShadowPresentationAttribute(
                  rect.entityHandle(), "unknown",
                  parser::PropertyParseFnParams::CreateForAttribute("red")),
              ParseResultIs(false));
}

TEST(FilterPrimitiveComponentTest, ParseLightingPresentationAttribute) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);

  EXPECT_THAT(ParseFeDiffuseLightingPresentationAttribute(
                  rect.entityHandle(), "lighting-color",
                  parser::PropertyParseFnParams::CreateForAttribute("blue")),
              ParseResultIs(true));
  EXPECT_THAT(ParseFeSpecularLightingPresentationAttribute(
                  rect.entityHandle(), "lighting-color",
                  parser::PropertyParseFnParams::CreateForAttribute("green")),
              ParseResultIs(true));

  const auto& diffuse = rect.entityHandle().get<FEDiffuseLightingComponent>();
  const auto& specular = rect.entityHandle().get<FESpecularLightingComponent>();
  EXPECT_THAT(diffuse.lightingColor.get(), Optional(Color(RGBA(0, 0, 0xFF, 0xFF))));
  EXPECT_THAT(specular.lightingColor.get(), Optional(Color(RGBA(0, 128, 0, 0xFF))));
}

TEST(FilterPrimitiveComponentTest, ParseLightingPresentationAttributeErrors) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);

  EXPECT_THAT(ParseFeDiffuseLightingPresentationAttribute(
                  rect.entityHandle(), "lighting-color",
                  parser::PropertyParseFnParams::CreateForAttribute("bogus")),
              ParseErrorIs("Invalid color 'bogus'"));
  EXPECT_THAT(ParseFeDiffuseLightingPresentationAttribute(
                  rect.entityHandle(), "unknown",
                  parser::PropertyParseFnParams::CreateForAttribute("red")),
              ParseResultIs(false));
  EXPECT_THAT(ParseFeSpecularLightingPresentationAttribute(
                  rect.entityHandle(), "lighting-color",
                  parser::PropertyParseFnParams::CreateForAttribute("bogus")),
              ParseErrorIs("Invalid color 'bogus'"));
  EXPECT_THAT(ParseFeSpecularLightingPresentationAttribute(
                  rect.entityHandle(), "unknown",
                  parser::PropertyParseFnParams::CreateForAttribute("red")),
              ParseResultIs(false));
}

}  // namespace
}  // namespace donner::svg::components
