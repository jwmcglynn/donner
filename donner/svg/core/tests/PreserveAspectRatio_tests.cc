#include "donner/svg/core/PreserveAspectRatio.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

using testing::DoubleNear;
using testing::Pointwise;

namespace donner::svg {

// Validates operator<< and testing::PrintToString.
TEST(PreserveAspectRatio, AlignToString) {
  EXPECT_EQ("Align::None", testing::PrintToString(PreserveAspectRatio::Align::None));
  EXPECT_EQ("Align::XMinYMin", testing::PrintToString(PreserveAspectRatio::Align::XMinYMin));
  EXPECT_EQ("Align::XMidYMin", testing::PrintToString(PreserveAspectRatio::Align::XMidYMin));
  EXPECT_EQ("Align::XMaxYMin", testing::PrintToString(PreserveAspectRatio::Align::XMaxYMin));
  EXPECT_EQ("Align::XMinYMid", testing::PrintToString(PreserveAspectRatio::Align::XMinYMid));
  EXPECT_EQ("Align::XMidYMid", testing::PrintToString(PreserveAspectRatio::Align::XMidYMid));
  EXPECT_EQ("Align::XMaxYMid", testing::PrintToString(PreserveAspectRatio::Align::XMaxYMid));
  EXPECT_EQ("Align::XMinYMax", testing::PrintToString(PreserveAspectRatio::Align::XMinYMax));
  EXPECT_EQ("Align::XMidYMax", testing::PrintToString(PreserveAspectRatio::Align::XMidYMax));
  EXPECT_EQ("Align::XMaxYMax", testing::PrintToString(PreserveAspectRatio::Align::XMaxYMax));
}
TEST(PreserveAspectRatio, MeetOrSliceToString) {
  EXPECT_EQ(testing::PrintToString(PreserveAspectRatio::MeetOrSlice::Meet), "MeetOrSlice::Meet");
  EXPECT_EQ(testing::PrintToString(PreserveAspectRatio::MeetOrSlice::Slice), "MeetOrSlice::Slice");
}

TEST(PreserveAspectRatio, FullStructToString) {
  PreserveAspectRatio par1{PreserveAspectRatio::Align::None,
                           PreserveAspectRatio::MeetOrSlice::Meet};
  EXPECT_EQ(testing::PrintToString(par1), "PreserveAspectRatio {Align::None, MeetOrSlice::Meet}");

  PreserveAspectRatio par2{PreserveAspectRatio::Align::XMidYMid,
                           PreserveAspectRatio::MeetOrSlice::Slice};
  EXPECT_EQ(testing::PrintToString(par2),
            "PreserveAspectRatio {Align::XMidYMid, MeetOrSlice::Slice}");

  PreserveAspectRatio par3{PreserveAspectRatio::Align::XMaxYMax,
                           PreserveAspectRatio::MeetOrSlice::Meet};
  EXPECT_EQ(testing::PrintToString(par3),
            "PreserveAspectRatio {Align::XMaxYMax, MeetOrSlice::Meet}");
}

TEST(PreserveAspectRatio, TransformEmptyViewbox) {
  EXPECT_TRUE(PreserveAspectRatio()
                  .elementContentFromViewBoxTransform(Boxd({0, 0}, {100, 100}), std::nullopt)
                  .isIdentity());
}

TEST(PreserveAspectRatio, Defaults) {
  const Boxd viewbox({0, 0}, {100, 100});
  const PreserveAspectRatio preserveAspectRatio;

  EXPECT_TRUE(
      preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {100, 100}), viewbox)
          .isIdentity());

  // Element half size: Scale down content.
  EXPECT_THAT(
      preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 50}), viewbox),
      TransformEq(Transformd::Scale({0.5, 0.5})));

  // Larger: scale up.
  EXPECT_THAT(
      preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {200, 200}), viewbox),
      TransformEq(Transformd::Scale({2, 2})));

  // Aspect ratio is preserved, and the default is "meet" so the use the smaller dimension.
  {
    auto transform =
        preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 100}), viewbox);
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({0.5, 0.5}) * Transformd::Translate({0, 25})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(0, 25));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(50, 75));
  }

  {
    auto transform =
        preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {400, 200}), viewbox);
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({2, 2}) * Transformd::Translate({100, 0})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(100, 0));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(300, 200));
  }

  // With the position x/y other than 0,0 it translates to the new origin too.
  {
    auto transform =
        preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({50, 50}, {250, 450}), viewbox);
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({2, 2}) * Transformd::Translate({50, 150})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(50, 150));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(250, 350));
  }
}

TEST(PreserveAspectRatio, None) {
  const Boxd viewbox({0, 0}, {100, 100});
  const PreserveAspectRatio preserveAspectRatio = PreserveAspectRatio::None();

  EXPECT_TRUE(
      preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {100, 100}), viewbox)
          .isIdentity());

  {
    auto transform =
        preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 100}), viewbox);
    EXPECT_THAT(transform, TransformEq(Transformd::Scale({0.5, 1})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(50, 100));
  }

  {
    auto transform =
        preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {400, 200}), viewbox);
    EXPECT_THAT(transform, TransformEq(Transformd::Scale({4, 2})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(400, 200));
  }

  // With the position x/y other than 0,0 it translates to the new origin.
  {
    auto transform =
        preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({50, 50}, {250, 450}), viewbox);
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({2, 4}) * Transformd::Translate({50, 50})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(50, 50));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(250, 450));
  }
}

TEST(PreserveAspectRatio, Slice) {
  const Boxd viewbox({0, 0}, {100, 100});
  const PreserveAspectRatio preserveAspectRatio{PreserveAspectRatio::Align::XMidYMid,
                                                PreserveAspectRatio::MeetOrSlice::Slice};

  EXPECT_TRUE(
      preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {100, 100}), viewbox)
          .isIdentity());

  // No slicing if the box fits.
  EXPECT_THAT(
      preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 50}), viewbox),
      TransformEq(Transformd::Scale({0.5, 0.5})));
  EXPECT_THAT(
      preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {200, 200}), viewbox),
      TransformEq(Transformd::Scale({2, 2})));

  // Slice, effectively scaling to the larger dimension.
  {
    auto transform =
        preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 200}), viewbox);
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({2, 2}) * Transformd::Translate({-75, 0})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(-75, 0));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(125, 200));
  }

  {
    auto transform =
        preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 25}), viewbox);
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({0.5, 0.5}) * Transformd::Translate({0, -12.5})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(0, -12.5));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(50, 37.5));
  }

  // With the position x/y other than 0,0 it translates to the new origin too.
  {
    auto transform =
        preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({50, 50}, {250, 450}), viewbox);
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({4, 4}) * Transformd::Translate({-50, 50})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(-50, 50));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(350, 450));
  }
}

TEST(PreserveAspectRatio, MinMaxMeet) {
  const Boxd viewbox({0, 0}, {100, 100});
  const PreserveAspectRatio minMeet{PreserveAspectRatio::Align::XMinYMin,
                                    PreserveAspectRatio::MeetOrSlice::Meet};
  const PreserveAspectRatio maxMeet{PreserveAspectRatio::Align::XMaxYMax,
                                    PreserveAspectRatio::MeetOrSlice::Meet};

  // No effect if the box fits.
  EXPECT_TRUE(
      minMeet.elementContentFromViewBoxTransform(Boxd({0, 0}, {100, 100}), viewbox).isIdentity());
  EXPECT_THAT(minMeet.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 50}), viewbox),
              TransformEq(Transformd::Scale({0.5, 0.5})));
  EXPECT_THAT(minMeet.elementContentFromViewBoxTransform(Boxd({0, 0}, {200, 200}), viewbox),
              TransformEq(Transformd::Scale({2, 2})));
  EXPECT_TRUE(
      maxMeet.elementContentFromViewBoxTransform(Boxd({0, 0}, {100, 100}), viewbox).isIdentity());
  EXPECT_THAT(maxMeet.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 50}), viewbox),
              TransformEq(Transformd::Scale({0.5, 0.5})));
  EXPECT_THAT(maxMeet.elementContentFromViewBoxTransform(Boxd({0, 0}, {200, 200}), viewbox),
              TransformEq(Transformd::Scale({2, 2})));

  {
    auto transformMin =
        minMeet.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 100}), viewbox);
    auto transformMax =
        maxMeet.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 100}), viewbox);
    EXPECT_THAT(transformMin, TransformEq(Transformd::Scale({0.5, 0.5})));
    EXPECT_THAT(transformMax,
                TransformEq(Transformd::Scale({0.5, 0.5}) * Transformd::Translate({0, 50})));
  }

  {
    auto transformMin =
        minMeet.elementContentFromViewBoxTransform(Boxd({0, 0}, {400, 200}), viewbox);
    auto transformMax =
        maxMeet.elementContentFromViewBoxTransform(Boxd({0, 0}, {400, 200}), viewbox);

    EXPECT_THAT(transformMin, TransformEq(Transformd::Scale({2, 2})));
    EXPECT_THAT(transformMax,
                TransformEq(Transformd::Scale({2, 2}) * Transformd::Translate({200, 0})));
  }
}

TEST(PreserveAspectRatio, MinMaxSlice) {
  const Boxd viewbox({0, 0}, {100, 100});
  const PreserveAspectRatio minSlice{PreserveAspectRatio::Align::XMinYMin,
                                     PreserveAspectRatio::MeetOrSlice::Slice};
  const PreserveAspectRatio maxSlice{PreserveAspectRatio::Align::XMaxYMax,
                                     PreserveAspectRatio::MeetOrSlice::Slice};

  // No effect if the box fits.
  EXPECT_TRUE(
      minSlice.elementContentFromViewBoxTransform(Boxd({0, 0}, {100, 100}), viewbox).isIdentity());
  EXPECT_THAT(minSlice.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 50}), viewbox),
              TransformEq(Transformd::Scale({0.5, 0.5})));
  EXPECT_THAT(minSlice.elementContentFromViewBoxTransform(Boxd({0, 0}, {200, 200}), viewbox),
              TransformEq(Transformd::Scale({2, 2})));
  EXPECT_TRUE(
      maxSlice.elementContentFromViewBoxTransform(Boxd({0, 0}, {100, 100}), viewbox).isIdentity());
  EXPECT_THAT(maxSlice.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 50}), viewbox),
              TransformEq(Transformd::Scale({0.5, 0.5})));
  EXPECT_THAT(maxSlice.elementContentFromViewBoxTransform(Boxd({0, 0}, {200, 200}), viewbox),
              TransformEq(Transformd::Scale({2, 2})));

  {
    auto transformMin =
        minSlice.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 200}), viewbox);
    auto transformMax =
        maxSlice.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 200}), viewbox);
    EXPECT_THAT(transformMin, TransformEq(Transformd::Scale({2, 2})));
    EXPECT_THAT(transformMax,
                TransformEq(Transformd::Scale({2, 2}) * Transformd::Translate({-150, 0})));
  }

  {
    auto transformMin =
        minSlice.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 25}), viewbox);
    auto transformMax =
        maxSlice.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 25}), viewbox);
    EXPECT_THAT(transformMin, TransformEq(Transformd::Scale({0.5, 0.5})));
    EXPECT_THAT(transformMax,
                TransformEq(Transformd::Scale({0.5, 0.5}) * Transformd::Translate({0, -25})));
  }
}

}  // namespace donner::svg
