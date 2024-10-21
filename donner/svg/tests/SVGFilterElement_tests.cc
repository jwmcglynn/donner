#include "donner/svg/SVGFilterElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/SVGFEGaussianBlurElement.h"
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

TEST(SVGFilterElementTests, Defaults) {
  auto filter = instantiateSubtreeElementAs<SVGFilterElement>("<filter />");
  EXPECT_THAT(filter, FilterHas(AllOf(XEq(-10.0, Lengthd::Unit::Percent),      //
                                      YEq(-10.0, Lengthd::Unit::Percent),      //
                                      WidthEq(120.0, Lengthd::Unit::Percent),  //
                                      HeightEq(120.0, Lengthd::Unit::Percent))));

  EXPECT_THAT(filter->filterUnits(), testing::Eq(FilterUnits::ObjectBoundingBox));
  EXPECT_THAT(filter->primitiveUnits(), testing::Eq(PrimitiveUnits::UserSpaceOnUse));
}

TEST(SVGFilterElementTests, SetRect) {
  auto filter = instantiateSubtreeElementAs<SVGFilterElement>(
      R"(<filter x="10" y="20" width="30" height="40" />)");
  EXPECT_THAT(filter, FilterHas(AllOf(XEq(10.0, Lengthd::Unit::None),      //
                                      YEq(20.0, Lengthd::Unit::None),      //
                                      WidthEq(30.0, Lengthd::Unit::None),  //
                                      HeightEq(40.0, Lengthd::Unit::None))));
}

TEST(SVGFilterElementTests, FilterUnits) {
  {
    auto filter =
        instantiateSubtreeElementAs<SVGFilterElement>(R"(<filter filterUnits="userSpaceOnUse" />)");
    EXPECT_THAT(filter->filterUnits(), testing::Eq(FilterUnits::UserSpaceOnUse));
  }

  {
    auto filter = instantiateSubtreeElementAs<SVGFilterElement>(
        R"(<filter filterUnits="objectBoundingBox" />)");
    EXPECT_THAT(filter->filterUnits(), testing::Eq(FilterUnits::ObjectBoundingBox));
  }

  // An invalid option will go back to the default.
  {
    auto filter =
        instantiateSubtreeElementAs<SVGFilterElement>(R"(<filter filterUnits="invalid" />)");
    EXPECT_THAT(filter->filterUnits(), testing::Eq(FilterUnits::Default));
  }
}

TEST(SVGFilterElementTests, PrimitiveUnits) {
  {
    auto filter = instantiateSubtreeElementAs<SVGFilterElement>(
        R"(<filter primitiveUnits="userSpaceOnUse" />)");
    EXPECT_THAT(filter->primitiveUnits(), testing::Eq(PrimitiveUnits::UserSpaceOnUse));
  }

  {
    auto filter = instantiateSubtreeElementAs<SVGFilterElement>(
        R"(<filter primitiveUnits="objectBoundingBox" />)");
    EXPECT_THAT(filter->primitiveUnits(), testing::Eq(PrimitiveUnits::ObjectBoundingBox));
  }

  // An invalid option will go back to the default.
  {
    auto filter =
        instantiateSubtreeElementAs<SVGFilterElement>(R"(<filter primitiveUnits="invalid" />)");
    EXPECT_THAT(filter->primitiveUnits(), testing::Eq(PrimitiveUnits::Default));
  }
}

// TODO: Move to another file
TEST(SVGFEGaussianBlurElement, SetStdDeviation) {
  auto blur = instantiateSubtreeElementAs<SVGFEGaussianBlurElement>(
      "<feGaussianBlur stdDeviation=\"3\" />");
  EXPECT_EQ(blur->stdDeviationX(), 3.0);
  EXPECT_EQ(blur->stdDeviationY(), 3.0);
}

}  // namespace donner::svg
