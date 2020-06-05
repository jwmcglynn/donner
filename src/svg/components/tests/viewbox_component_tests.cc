#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/svg/components/viewbox_component.h"
#include "src/svg/core/tests/path_spline_test_utils.h"

using testing::DoubleNear;
using testing::Pointwise;

namespace donner {

MATCHER_P(TransformEq, other, "") {
  return testing::ExplainMatchResult(Pointwise(DoubleNear(0.01), other.data), arg.data,
                                     result_listener);
}

TEST(ViewboxComponent, Defaults) {
  Boxd viewbox({0, 0}, {100, 100});
  ViewboxComponent component{viewbox, PreserveAspectRatio()};

  EXPECT_TRUE(component.computeTransform(Boxd({0, 0}, {100, 100})).isIdentity());

  // Element half size: Scale down content.
  EXPECT_THAT(component.computeTransform(Boxd({0, 0}, {50, 50})),
              TransformEq(Transformd::Scale({0.5, 0.5})));

  // Larger: scale up.
  EXPECT_THAT(component.computeTransform(Boxd({0, 0}, {200, 200})),
              TransformEq(Transformd::Scale({2, 2})));

  // Aspect ratio is preserved, and the default is "meet" so the use the smaller dimension.
  {
    auto transform = component.computeTransform(Boxd({0, 0}, {50, 100}));
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({0.5, 0.5}) * Transformd::Translate({0, 25})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(0, 25));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(50, 75));
  }

  {
    auto transform = component.computeTransform(Boxd({0, 0}, {400, 200}));
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({2, 2}) * Transformd::Translate({100, 0})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(100, 0));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(300, 200));
  }

  // With the position x/y other than 0,0 it translates to the new origin too.
  {
    auto transform = component.computeTransform(Boxd({50, 50}, {250, 450}));
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({2, 2}) * Transformd::Translate({50, 150})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(50, 150));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(250, 350));
  }
}

TEST(ViewboxComponent, None) {
  Boxd viewbox({0, 0}, {100, 100});
  ViewboxComponent component{viewbox, PreserveAspectRatio::None()};

  EXPECT_TRUE(component.computeTransform(Boxd({0, 0}, {100, 100})).isIdentity());

  {
    auto transform = component.computeTransform(Boxd({0, 0}, {50, 100}));
    EXPECT_THAT(transform, TransformEq(Transformd::Scale({0.5, 1})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(50, 100));
  }

  {
    auto transform = component.computeTransform(Boxd({0, 0}, {400, 200}));
    EXPECT_THAT(transform, TransformEq(Transformd::Scale({4, 2})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(400, 200));
  }

  // With the position x/y other than 0,0 it translates to the new origin.
  {
    auto transform = component.computeTransform(Boxd({50, 50}, {250, 450}));
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({2, 4}) * Transformd::Translate({50, 50})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(50, 50));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(250, 450));
  }
}

TEST(ViewboxComponent, Slice) {
  Boxd viewbox({0, 0}, {100, 100});
  ViewboxComponent component{viewbox, PreserveAspectRatio{PreserveAspectRatio::Align::XMidYMid,
                                                          PreserveAspectRatio::MeetOrSlice::Slice}};

  EXPECT_TRUE(component.computeTransform(Boxd({0, 0}, {100, 100})).isIdentity());

  // No slicing if the box fits.
  EXPECT_THAT(component.computeTransform(Boxd({0, 0}, {50, 50})),
              TransformEq(Transformd::Scale({0.5, 0.5})));
  EXPECT_THAT(component.computeTransform(Boxd({0, 0}, {200, 200})),
              TransformEq(Transformd::Scale({2, 2})));

  // Slice, effectively scaling to the larger dimension.
  {
    auto transform = component.computeTransform(Boxd({0, 0}, {50, 200}));
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({2, 2}) * Transformd::Translate({-75, 0})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(-75, 0));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(125, 200));
  }

  {
    auto transform = component.computeTransform(Boxd({0, 0}, {50, 25}));
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({0.5, 0.5}) * Transformd::Translate({0, -12.5})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(0, -12.5));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(50, 37.5));
  }

  // With the position x/y other than 0,0 it translates to the new origin too.
  {
    auto transform = component.computeTransform(Boxd({50, 50}, {250, 450}));
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({4, 4}) * Transformd::Translate({-50, 50})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(-50, 50));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(350, 450));
  }
}

TEST(ViewboxComponent, MinMaxMeet) {
  Boxd viewbox({0, 0}, {100, 100});
  ViewboxComponent componentMin{viewbox,
                                PreserveAspectRatio{PreserveAspectRatio::Align::XMinYMin,
                                                    PreserveAspectRatio::MeetOrSlice::Meet}};
  ViewboxComponent componentMax{viewbox,
                                PreserveAspectRatio{PreserveAspectRatio::Align::XMaxYMax,
                                                    PreserveAspectRatio::MeetOrSlice::Meet}};

  // No effect if the box fits.
  EXPECT_TRUE(componentMin.computeTransform(Boxd({0, 0}, {100, 100})).isIdentity());
  EXPECT_THAT(componentMin.computeTransform(Boxd({0, 0}, {50, 50})),
              TransformEq(Transformd::Scale({0.5, 0.5})));
  EXPECT_THAT(componentMin.computeTransform(Boxd({0, 0}, {200, 200})),
              TransformEq(Transformd::Scale({2, 2})));
  EXPECT_TRUE(componentMax.computeTransform(Boxd({0, 0}, {100, 100})).isIdentity());
  EXPECT_THAT(componentMax.computeTransform(Boxd({0, 0}, {50, 50})),
              TransformEq(Transformd::Scale({0.5, 0.5})));
  EXPECT_THAT(componentMax.computeTransform(Boxd({0, 0}, {200, 200})),
              TransformEq(Transformd::Scale({2, 2})));

  {
    auto transformMin = componentMin.computeTransform(Boxd({0, 0}, {50, 100}));
    auto transformMax = componentMax.computeTransform(Boxd({0, 0}, {50, 100}));
    EXPECT_THAT(transformMin, TransformEq(Transformd::Scale({0.5, 0.5})));
    EXPECT_THAT(transformMax,
                TransformEq(Transformd::Scale({0.5, 0.5}) * Transformd::Translate({0, 50})));
  }

  {
    auto transformMin = componentMin.computeTransform(Boxd({0, 0}, {400, 200}));
    auto transformMax = componentMax.computeTransform(Boxd({0, 0}, {400, 200}));

    EXPECT_THAT(transformMin, TransformEq(Transformd::Scale({2, 2})));
    EXPECT_THAT(transformMax,
                TransformEq(Transformd::Scale({2, 2}) * Transformd::Translate({200, 0})));
  }
}

TEST(ViewboxComponent, MinMaxSlice) {
  Boxd viewbox({0, 0}, {100, 100});
  ViewboxComponent componentMin{viewbox,
                                PreserveAspectRatio{PreserveAspectRatio::Align::XMinYMin,
                                                    PreserveAspectRatio::MeetOrSlice::Slice}};
  ViewboxComponent componentMax{viewbox,
                                PreserveAspectRatio{PreserveAspectRatio::Align::XMaxYMax,
                                                    PreserveAspectRatio::MeetOrSlice::Slice}};

  // No effect if the box fits.
  EXPECT_TRUE(componentMin.computeTransform(Boxd({0, 0}, {100, 100})).isIdentity());
  EXPECT_THAT(componentMin.computeTransform(Boxd({0, 0}, {50, 50})),
              TransformEq(Transformd::Scale({0.5, 0.5})));
  EXPECT_THAT(componentMin.computeTransform(Boxd({0, 0}, {200, 200})),
              TransformEq(Transformd::Scale({2, 2})));
  EXPECT_TRUE(componentMax.computeTransform(Boxd({0, 0}, {100, 100})).isIdentity());
  EXPECT_THAT(componentMax.computeTransform(Boxd({0, 0}, {50, 50})),
              TransformEq(Transformd::Scale({0.5, 0.5})));
  EXPECT_THAT(componentMax.computeTransform(Boxd({0, 0}, {200, 200})),
              TransformEq(Transformd::Scale({2, 2})));

  {
    auto transformMin = componentMin.computeTransform(Boxd({0, 0}, {50, 200}));
    auto transformMax = componentMax.computeTransform(Boxd({0, 0}, {50, 200}));
    EXPECT_THAT(transformMin, TransformEq(Transformd::Scale({2, 2})));
    EXPECT_THAT(transformMax,
                TransformEq(Transformd::Scale({2, 2}) * Transformd::Translate({-150, 0})));
  }

  {
    auto transformMin = componentMin.computeTransform(Boxd({0, 0}, {50, 25}));
    auto transformMax = componentMax.computeTransform(Boxd({0, 0}, {50, 25}));
    EXPECT_THAT(transformMin, TransformEq(Transformd::Scale({0.5, 0.5})));
    EXPECT_THAT(transformMax,
                TransformEq(Transformd::Scale({0.5, 0.5}) * Transformd::Translate({0, -25})));
  }
}

}  // namespace donner
