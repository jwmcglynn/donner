#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/tests/base_test_utils.h"
#include "src/svg/components/viewbox_component.h"

using testing::DoubleNear;
using testing::Pointwise;

namespace donner::svg {

TEST(ViewboxComponent, OptionalNone) {
  Boxd viewbox({0, 0}, {100, 100});
  ViewboxComponent component{std::nullopt};
  EXPECT_TRUE(
      component.computeTransform(Boxd({0, 0}, {100, 100}), PreserveAspectRatio()).isIdentity());
}

TEST(ViewboxComponent, Defaults) {
  const Boxd viewbox({0, 0}, {100, 100});
  const PreserveAspectRatio preserveAspectRatio;
  ViewboxComponent component{std::make_optional(viewbox)};

  EXPECT_TRUE(
      component.computeTransform(Boxd({0, 0}, {100, 100}), preserveAspectRatio).isIdentity());

  // Element half size: Scale down content.
  EXPECT_THAT(component.computeTransform(Boxd({0, 0}, {50, 50}), preserveAspectRatio),
              TransformEq(Transformd::Scale({0.5, 0.5})));

  // Larger: scale up.
  EXPECT_THAT(component.computeTransform(Boxd({0, 0}, {200, 200}), preserveAspectRatio),
              TransformEq(Transformd::Scale({2, 2})));

  // Aspect ratio is preserved, and the default is "meet" so the use the smaller dimension.
  {
    auto transform = component.computeTransform(Boxd({0, 0}, {50, 100}), preserveAspectRatio);
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({0.5, 0.5}) * Transformd::Translate({0, 25})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(0, 25));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(50, 75));
  }

  {
    auto transform = component.computeTransform(Boxd({0, 0}, {400, 200}), preserveAspectRatio);
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({2, 2}) * Transformd::Translate({100, 0})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(100, 0));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(300, 200));
  }

  // With the position x/y other than 0,0 it translates to the new origin too.
  {
    auto transform = component.computeTransform(Boxd({50, 50}, {250, 450}), preserveAspectRatio);
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({2, 2}) * Transformd::Translate({50, 150})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(50, 150));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(250, 350));
  }
}

TEST(ViewboxComponent, None) {
  const Boxd viewbox({0, 0}, {100, 100});
  const PreserveAspectRatio preserveAspectRatio = PreserveAspectRatio::None();
  ViewboxComponent component{std::make_optional(viewbox)};

  EXPECT_TRUE(
      component.computeTransform(Boxd({0, 0}, {100, 100}), preserveAspectRatio).isIdentity());

  {
    auto transform = component.computeTransform(Boxd({0, 0}, {50, 100}), preserveAspectRatio);
    EXPECT_THAT(transform, TransformEq(Transformd::Scale({0.5, 1})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(50, 100));
  }

  {
    auto transform = component.computeTransform(Boxd({0, 0}, {400, 200}), preserveAspectRatio);
    EXPECT_THAT(transform, TransformEq(Transformd::Scale({4, 2})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(400, 200));
  }

  // With the position x/y other than 0,0 it translates to the new origin.
  {
    auto transform = component.computeTransform(Boxd({50, 50}, {250, 450}), preserveAspectRatio);
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({2, 4}) * Transformd::Translate({50, 50})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(50, 50));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(250, 450));
  }
}

TEST(ViewboxComponent, Slice) {
  const Boxd viewbox({0, 0}, {100, 100});
  const PreserveAspectRatio preserveAspectRatio{PreserveAspectRatio::Align::XMidYMid,
                                                PreserveAspectRatio::MeetOrSlice::Slice};
  ViewboxComponent component{std::make_optional(viewbox)};

  EXPECT_TRUE(
      component.computeTransform(Boxd({0, 0}, {100, 100}), preserveAspectRatio).isIdentity());

  // No slicing if the box fits.
  EXPECT_THAT(component.computeTransform(Boxd({0, 0}, {50, 50}), preserveAspectRatio),
              TransformEq(Transformd::Scale({0.5, 0.5})));
  EXPECT_THAT(component.computeTransform(Boxd({0, 0}, {200, 200}), preserveAspectRatio),
              TransformEq(Transformd::Scale({2, 2})));

  // Slice, effectively scaling to the larger dimension.
  {
    auto transform = component.computeTransform(Boxd({0, 0}, {50, 200}), preserveAspectRatio);
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({2, 2}) * Transformd::Translate({-75, 0})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(-75, 0));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(125, 200));
  }

  {
    auto transform = component.computeTransform(Boxd({0, 0}, {50, 25}), preserveAspectRatio);
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({0.5, 0.5}) * Transformd::Translate({0, -12.5})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(0, -12.5));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(50, 37.5));
  }

  // With the position x/y other than 0,0 it translates to the new origin too.
  {
    auto transform = component.computeTransform(Boxd({50, 50}, {250, 450}), preserveAspectRatio);
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({4, 4}) * Transformd::Translate({-50, 50})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(-50, 50));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(350, 450));
  }
}

TEST(ViewboxComponent, MinMaxMeet) {
  const Boxd viewbox({0, 0}, {100, 100});
  const PreserveAspectRatio minMeet{PreserveAspectRatio::Align::XMinYMin,
                                    PreserveAspectRatio::MeetOrSlice::Meet};
  const PreserveAspectRatio maxMeet{PreserveAspectRatio::Align::XMaxYMax,
                                    PreserveAspectRatio::MeetOrSlice::Meet};
  ViewboxComponent component{std::make_optional(viewbox)};

  // No effect if the box fits.
  EXPECT_TRUE(component.computeTransform(Boxd({0, 0}, {100, 100}), minMeet).isIdentity());
  EXPECT_THAT(component.computeTransform(Boxd({0, 0}, {50, 50}), minMeet),
              TransformEq(Transformd::Scale({0.5, 0.5})));
  EXPECT_THAT(component.computeTransform(Boxd({0, 0}, {200, 200}), minMeet),
              TransformEq(Transformd::Scale({2, 2})));
  EXPECT_TRUE(component.computeTransform(Boxd({0, 0}, {100, 100}), maxMeet).isIdentity());
  EXPECT_THAT(component.computeTransform(Boxd({0, 0}, {50, 50}), maxMeet),
              TransformEq(Transformd::Scale({0.5, 0.5})));
  EXPECT_THAT(component.computeTransform(Boxd({0, 0}, {200, 200}), maxMeet),
              TransformEq(Transformd::Scale({2, 2})));

  {
    auto transformMin = component.computeTransform(Boxd({0, 0}, {50, 100}), minMeet);
    auto transformMax = component.computeTransform(Boxd({0, 0}, {50, 100}), maxMeet);
    EXPECT_THAT(transformMin, TransformEq(Transformd::Scale({0.5, 0.5})));
    EXPECT_THAT(transformMax,
                TransformEq(Transformd::Scale({0.5, 0.5}) * Transformd::Translate({0, 50})));
  }

  {
    auto transformMin = component.computeTransform(Boxd({0, 0}, {400, 200}), minMeet);
    auto transformMax = component.computeTransform(Boxd({0, 0}, {400, 200}), maxMeet);

    EXPECT_THAT(transformMin, TransformEq(Transformd::Scale({2, 2})));
    EXPECT_THAT(transformMax,
                TransformEq(Transformd::Scale({2, 2}) * Transformd::Translate({200, 0})));
  }
}

TEST(ViewboxComponent, MinMaxSlice) {
  const Boxd viewbox({0, 0}, {100, 100});
  const PreserveAspectRatio minSlice{PreserveAspectRatio::Align::XMinYMin,
                                     PreserveAspectRatio::MeetOrSlice::Slice};
  const PreserveAspectRatio maxSlice{PreserveAspectRatio::Align::XMaxYMax,
                                     PreserveAspectRatio::MeetOrSlice::Slice};
  ViewboxComponent component{std::make_optional(viewbox)};

  // No effect if the box fits.
  EXPECT_TRUE(component.computeTransform(Boxd({0, 0}, {100, 100}), minSlice).isIdentity());
  EXPECT_THAT(component.computeTransform(Boxd({0, 0}, {50, 50}), minSlice),
              TransformEq(Transformd::Scale({0.5, 0.5})));
  EXPECT_THAT(component.computeTransform(Boxd({0, 0}, {200, 200}), minSlice),
              TransformEq(Transformd::Scale({2, 2})));
  EXPECT_TRUE(component.computeTransform(Boxd({0, 0}, {100, 100}), maxSlice).isIdentity());
  EXPECT_THAT(component.computeTransform(Boxd({0, 0}, {50, 50}), maxSlice),
              TransformEq(Transformd::Scale({0.5, 0.5})));
  EXPECT_THAT(component.computeTransform(Boxd({0, 0}, {200, 200}), maxSlice),
              TransformEq(Transformd::Scale({2, 2})));

  {
    auto transformMin = component.computeTransform(Boxd({0, 0}, {50, 200}), minSlice);
    auto transformMax = component.computeTransform(Boxd({0, 0}, {50, 200}), maxSlice);
    EXPECT_THAT(transformMin, TransformEq(Transformd::Scale({2, 2})));
    EXPECT_THAT(transformMax,
                TransformEq(Transformd::Scale({2, 2}) * Transformd::Translate({-150, 0})));
  }

  {
    auto transformMin = component.computeTransform(Boxd({0, 0}, {50, 25}), minSlice);
    auto transformMax = component.computeTransform(Boxd({0, 0}, {50, 25}), maxSlice);
    EXPECT_THAT(transformMin, TransformEq(Transformd::Scale({0.5, 0.5})));
    EXPECT_THAT(transformMax,
                TransformEq(Transformd::Scale({0.5, 0.5}) * Transformd::Translate({0, -25})));
  }
}

}  // namespace donner::svg
