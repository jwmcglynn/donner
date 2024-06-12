#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/tests/base_test_utils.h"
#include "src/svg/core/gradient.h"
#include "src/svg/core/preserve_aspect_ratio.h"
#include "src/svg/renderer/tests/renderer_test_utils.h"
#include "src/svg/svg_pattern_element.h"
#include "src/svg/tests/xml_test_utils.h"

using testing::AllOf;

namespace donner::svg {

TEST(SVGPatternElementTests, Defaults) {
  auto pattern = instantiateSubtreeElementAs<SVGPatternElement>("<pattern />");

  EXPECT_THAT(pattern->viewbox(), testing::Eq(std::nullopt));
  EXPECT_THAT(pattern->preserveAspectRatio(),
              testing::Eq(PreserveAspectRatio{PreserveAspectRatio::Align::XMidYMid,
                                              PreserveAspectRatio::MeetOrSlice::Meet}));

  EXPECT_THAT(pattern->x(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(pattern->y(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(pattern->width(), testing::Eq(std::nullopt));
  EXPECT_THAT(pattern->height(), testing::Eq(std::nullopt));

  EXPECT_THAT(pattern->patternUnits(), testing::Eq(PatternUnits::ObjectBoundingBox));
  EXPECT_THAT(pattern->patternContentUnits(), testing::Eq(PatternContentUnits::UserSpaceOnUse));
  EXPECT_THAT(pattern->patternTransform(), TransformEq(Transformd()));
  EXPECT_THAT(pattern->href(), testing::Eq(std::nullopt));
}

// TODO: Additional tests and rendering tests

}  // namespace donner::svg
