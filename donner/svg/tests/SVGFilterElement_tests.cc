#include "donner/svg/SVGFilterElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/SVGFEComponentTransferElement.h"
#include "donner/svg/SVGFEFuncBElement.h"
#include "donner/svg/SVGFEGaussianBlurElement.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/filter/FilterSystem.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::AllOf;

namespace donner::svg {

namespace {

auto XEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("x", &SVGFilterElement::x, LengthIs(valueMatcher, unitMatcher));
}

auto YEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("y", &SVGFilterElement::y, LengthIs(valueMatcher, unitMatcher));
}

auto WidthEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("width", &SVGFilterElement::width, LengthIs(valueMatcher, unitMatcher));
}

auto HeightEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("height", &SVGFilterElement::height,
                           LengthIs(valueMatcher, unitMatcher));
}

MATCHER_P(FilterHas, matchers, "") {
  return testing::ExplainMatchResult(matchers, arg.element, result_listener);
}

}  // namespace

TEST(SVGFilterElementTests, FeatureDisabled) {
  auto element = instantiateSubtreeElement("<filter />");
  EXPECT_EQ(element->type(), ElementType::Unknown);
}

TEST(SVGFilterElementTests, Defaults) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  auto filter = instantiateSubtreeElementAs<SVGFilterElement>("<filter />", options);
  EXPECT_THAT(filter, FilterHas(AllOf(XEq(-10.0, Lengthd::Unit::Percent),      //
                                      YEq(-10.0, Lengthd::Unit::Percent),      //
                                      WidthEq(120.0, Lengthd::Unit::Percent),  //
                                      HeightEq(120.0, Lengthd::Unit::Percent))));

  EXPECT_THAT(filter->filterUnits(), testing::Eq(FilterUnits::ObjectBoundingBox));
  EXPECT_THAT(filter->primitiveUnits(), testing::Eq(PrimitiveUnits::UserSpaceOnUse));
}

TEST(SVGFilterElementTests, SetRect) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  auto filter = instantiateSubtreeElementAs<SVGFilterElement>(
      R"(<filter x="10" y="20" width="30" height="40" />)", options);
  EXPECT_THAT(filter, FilterHas(AllOf(XEq(10.0, Lengthd::Unit::None),      //
                                      YEq(20.0, Lengthd::Unit::None),      //
                                      WidthEq(30.0, Lengthd::Unit::None),  //
                                      HeightEq(40.0, Lengthd::Unit::None))));
}

TEST(SVGFilterElementTests, FilterUnits) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;
  {
    auto filter = instantiateSubtreeElementAs<SVGFilterElement>(
        R"(<filter filterUnits="userSpaceOnUse" />)", options);
    EXPECT_THAT(filter->filterUnits(), testing::Eq(FilterUnits::UserSpaceOnUse));
  }

  {
    auto filter = instantiateSubtreeElementAs<SVGFilterElement>(
        R"(<filter filterUnits="objectBoundingBox" />)", options);
    EXPECT_THAT(filter->filterUnits(), testing::Eq(FilterUnits::ObjectBoundingBox));
  }

  // An invalid option will go back to the default.
  {
    auto filter = instantiateSubtreeElementAs<SVGFilterElement>(
        R"(<filter filterUnits="invalid" />)", options);
    EXPECT_THAT(filter->filterUnits(), testing::Eq(FilterUnits::Default));
  }
}

TEST(SVGFilterElementTests, PrimitiveUnits) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  {
    auto filter = instantiateSubtreeElementAs<SVGFilterElement>(
        R"(<filter primitiveUnits="userSpaceOnUse" />)", options);
    EXPECT_THAT(filter->primitiveUnits(), testing::Eq(PrimitiveUnits::UserSpaceOnUse));
  }

  {
    auto filter = instantiateSubtreeElementAs<SVGFilterElement>(
        R"(<filter primitiveUnits="objectBoundingBox" />)", options);
    EXPECT_THAT(filter->primitiveUnits(), testing::Eq(PrimitiveUnits::ObjectBoundingBox));
  }

  // An invalid option will go back to the default.
  {
    auto filter = instantiateSubtreeElementAs<SVGFilterElement>(
        R"(<filter primitiveUnits="invalid" />)", options);
    EXPECT_THAT(filter->primitiveUnits(), testing::Eq(PrimitiveUnits::Default));
  }
}

TEST(SVGFilterElementTests, Href) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  {
    auto filter =
        instantiateSubtreeElementAs<SVGFilterElement>(R"(<filter href="#baseFilter" />)", options);
    EXPECT_THAT(filter->href(), testing::Optional(testing::Eq("#baseFilter")));
  }

  {
    auto filter = instantiateSubtreeElementAs<SVGFilterElement>(
        R"(<filter xmlns:xlink="http://www.w3.org/1999/xlink" xlink:href="#baseFilter" />)",
        options);
    EXPECT_THAT(filter->href(), testing::Optional(testing::Eq("#baseFilter")));
  }
}

TEST(SVGFilterElementTests, HrefInheritsPrimitivesAndRegion) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  SVGDocument document = instantiateSubtree(R"(
    <defs>
      <filter id="filter0" x="0.1" y="0.2" width="0.8" height="1.0">
        <feGaussianBlur stdDeviation="4"/>
      </filter>
      <filter id="filter1" href="#filter0"/>
    </defs>
  )",
                                            options, Vector2i(200, 200));

  std::vector<ParseError> warnings;
  components::FilterSystem().instantiateAllComputedComponents(document.registry(), &warnings);

  const entt::entity filter1Entity =
      document.registry().ctx().get<const components::SVGDocumentContext>().getEntityById(
          "filter1");
  ASSERT_TRUE(filter1Entity != entt::null);

  const auto* computed =
      document.registry().try_get<components::ComputedFilterComponent>(filter1Entity);
  ASSERT_NE(computed, nullptr);
  ASSERT_EQ(computed->filterGraph.nodes.size(), 1u);

  const auto* blur = std::get_if<components::filter_primitive::GaussianBlur>(
      &computed->filterGraph.nodes.front().primitive);
  ASSERT_NE(blur, nullptr);
  EXPECT_DOUBLE_EQ(blur->stdDeviationX, 4.0);
  EXPECT_DOUBLE_EQ(blur->stdDeviationY, 4.0);
  EXPECT_THAT(computed->x.value, testing::DoubleEq(0.1));
  EXPECT_THAT(computed->y.value, testing::DoubleEq(0.2));
  EXPECT_THAT(computed->width.value, testing::DoubleEq(0.8));
  EXPECT_THAT(computed->height.value, testing::DoubleEq(1.0));
}

TEST(SVGFilterElementTests, HrefInheritsAttributesButKeepsLocalPrimitives) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  SVGDocument document = instantiateSubtree(R"(
    <defs>
      <filter id="filter0" x="0.1" height="1.0">
        <feGaussianBlur stdDeviation="2"/>
      </filter>
      <filter id="filter1" y="0.2" width="0.8" href="#filter0">
        <feGaussianBlur stdDeviation="4"/>
      </filter>
    </defs>
  )",
                                            options, Vector2i(200, 200));

  std::vector<ParseError> warnings;
  components::FilterSystem().instantiateAllComputedComponents(document.registry(), &warnings);

  const entt::entity filter1Entity =
      document.registry().ctx().get<const components::SVGDocumentContext>().getEntityById(
          "filter1");
  ASSERT_TRUE(filter1Entity != entt::null);

  const auto* computed =
      document.registry().try_get<components::ComputedFilterComponent>(filter1Entity);
  ASSERT_NE(computed, nullptr);
  ASSERT_EQ(computed->filterGraph.nodes.size(), 1u);

  const auto* blur = std::get_if<components::filter_primitive::GaussianBlur>(
      &computed->filterGraph.nodes.front().primitive);
  ASSERT_NE(blur, nullptr);
  EXPECT_DOUBLE_EQ(blur->stdDeviationX, 4.0);
  EXPECT_DOUBLE_EQ(blur->stdDeviationY, 4.0);
  EXPECT_THAT(computed->x.value, testing::DoubleEq(0.1));
  EXPECT_THAT(computed->y.value, testing::DoubleEq(0.2));
  EXPECT_THAT(computed->width.value, testing::DoubleEq(0.8));
  EXPECT_THAT(computed->height.value, testing::DoubleEq(1.0));
}

TEST(SVGFilterElementTests, RejectsInvalidFeFuncTableValuesWithUnitSuffix) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  auto transfer = instantiateSubtreeElementAs<SVGFEComponentTransferElement>(
      R"(
        <feComponentTransfer>
          <feFuncB type="table" tableValues="1px"/>
        </feComponentTransfer>
      )",
      options);

  auto maybeChild = transfer->firstChild();
  ASSERT_TRUE(maybeChild.has_value());

  auto funcB = maybeChild->cast<SVGFEFuncBElement>();
  const auto* component = funcB.entityHandle().try_get<components::FEFuncComponent>();
  ASSERT_NE(component, nullptr);
  EXPECT_TRUE(component->tableValues.empty());
}

// TODO: Move to another file
TEST(SVGFEGaussianBlurElement, FeatureDisabled) {
  auto element = instantiateSubtreeElement("<feGaussianBlur />");
  EXPECT_EQ(element->type(), ElementType::Unknown);
}

// TODO: Move to another file
TEST(SVGFEGaussianBlurElement, SetStdDeviation) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  auto blur = instantiateSubtreeElementAs<SVGFEGaussianBlurElement>(
      "<feGaussianBlur stdDeviation=\"3\" />", options);
  EXPECT_EQ(blur->stdDeviationX(), 3.0);
  EXPECT_EQ(blur->stdDeviationY(), 3.0);
}

}  // namespace donner::svg
