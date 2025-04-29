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

TEST(PreserveAspectRatio, TransformEmptyViewBox) {
  EXPECT_TRUE(PreserveAspectRatio()
                  .elementContentFromViewBoxTransform(Boxd({0, 0}, {100, 100}), std::nullopt)
                  .isIdentity());
}

TEST(PreserveAspectRatio, Defaults) {
  const Boxd viewBox({0, 0}, {100, 100});
  const PreserveAspectRatio preserveAspectRatio;

  EXPECT_TRUE(
      preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {100, 100}), viewBox)
          .isIdentity());

  // Element half size: Scale down content.
  EXPECT_THAT(
      preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 50}), viewBox),
      TransformEq(Transformd::Scale({0.5, 0.5})));

  // Larger: scale up.
  EXPECT_THAT(
      preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {200, 200}), viewBox),
      TransformEq(Transformd::Scale({2, 2})));

  // Aspect ratio is preserved, and the default is "meet" so the use the smaller dimension.
  {
    auto transform =
        preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 100}), viewBox);
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({0.5, 0.5}) * Transformd::Translate({0, 25})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(0, 25));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(50, 75));
  }

  {
    auto transform =
        preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {400, 200}), viewBox);
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({2, 2}) * Transformd::Translate({100, 0})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(100, 0));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(300, 200));
  }

  // With the position x/y other than 0,0 it translates to the new origin too.
  {
    auto transform =
        preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({50, 50}, {250, 450}), viewBox);
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({2, 2}) * Transformd::Translate({50, 150})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(50, 150));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(250, 350));
  }
}

TEST(PreserveAspectRatio, None) {
  const Boxd viewBox({0, 0}, {100, 100});
  const PreserveAspectRatio preserveAspectRatio = PreserveAspectRatio::None();

  EXPECT_TRUE(
      preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {100, 100}), viewBox)
          .isIdentity());

  {
    auto transform =
        preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 100}), viewBox);
    EXPECT_THAT(transform, TransformEq(Transformd::Scale({0.5, 1})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(50, 100));
  }

  {
    auto transform =
        preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {400, 200}), viewBox);
    EXPECT_THAT(transform, TransformEq(Transformd::Scale({4, 2})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(400, 200));
  }

  // With the position x/y other than 0,0 it translates to the new origin.
  {
    auto transform =
        preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({50, 50}, {250, 450}), viewBox);
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({2, 4}) * Transformd::Translate({50, 50})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(50, 50));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(250, 450));
  }
}

TEST(PreserveAspectRatio, Slice) {
  const Boxd viewBox({0, 0}, {100, 100});
  const PreserveAspectRatio preserveAspectRatio{PreserveAspectRatio::Align::XMidYMid,
                                                PreserveAspectRatio::MeetOrSlice::Slice};

  EXPECT_TRUE(
      preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {100, 100}), viewBox)
          .isIdentity());

  // No slicing if the box fits.
  EXPECT_THAT(
      preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 50}), viewBox),
      TransformEq(Transformd::Scale({0.5, 0.5})));
  EXPECT_THAT(
      preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {200, 200}), viewBox),
      TransformEq(Transformd::Scale({2, 2})));

  // Slice, effectively scaling to the larger dimension.
  {
    auto transform =
        preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 200}), viewBox);
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({2, 2}) * Transformd::Translate({-75, 0})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(-75, 0));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(125, 200));
  }

  {
    auto transform =
        preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 25}), viewBox);
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({0.5, 0.5}) * Transformd::Translate({0, -12.5})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(0, -12.5));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(50, 37.5));
  }

  // With the position x/y other than 0,0 it translates to the new origin too.
  {
    auto transform =
        preserveAspectRatio.elementContentFromViewBoxTransform(Boxd({50, 50}, {250, 450}), viewBox);
    EXPECT_THAT(transform,
                TransformEq(Transformd::Scale({4, 4}) * Transformd::Translate({-50, 50})));

    EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(-50, 50));
    EXPECT_THAT(transform.transformPosition({100, 100}), Vector2Near(350, 450));
  }
}

TEST(PreserveAspectRatio, MinMaxMeet) {
  const Boxd viewBox({0, 0}, {100, 100});
  const PreserveAspectRatio minMeet{PreserveAspectRatio::Align::XMinYMin,
                                    PreserveAspectRatio::MeetOrSlice::Meet};
  const PreserveAspectRatio maxMeet{PreserveAspectRatio::Align::XMaxYMax,
                                    PreserveAspectRatio::MeetOrSlice::Meet};

  // No effect if the box fits.
  EXPECT_TRUE(
      minMeet.elementContentFromViewBoxTransform(Boxd({0, 0}, {100, 100}), viewBox).isIdentity());
  EXPECT_THAT(minMeet.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 50}), viewBox),
              TransformEq(Transformd::Scale({0.5, 0.5})));
  EXPECT_THAT(minMeet.elementContentFromViewBoxTransform(Boxd({0, 0}, {200, 200}), viewBox),
              TransformEq(Transformd::Scale({2, 2})));
  EXPECT_TRUE(
      maxMeet.elementContentFromViewBoxTransform(Boxd({0, 0}, {100, 100}), viewBox).isIdentity());
  EXPECT_THAT(maxMeet.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 50}), viewBox),
              TransformEq(Transformd::Scale({0.5, 0.5})));
  EXPECT_THAT(maxMeet.elementContentFromViewBoxTransform(Boxd({0, 0}, {200, 200}), viewBox),
              TransformEq(Transformd::Scale({2, 2})));

  {
    auto transformMin =
        minMeet.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 100}), viewBox);
    auto transformMax =
        maxMeet.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 100}), viewBox);
    EXPECT_THAT(transformMin, TransformEq(Transformd::Scale({0.5, 0.5})));
    EXPECT_THAT(transformMax,
                TransformEq(Transformd::Scale({0.5, 0.5}) * Transformd::Translate({0, 50})));
  }

  {
    auto transformMin =
        minMeet.elementContentFromViewBoxTransform(Boxd({0, 0}, {400, 200}), viewBox);
    auto transformMax =
        maxMeet.elementContentFromViewBoxTransform(Boxd({0, 0}, {400, 200}), viewBox);

    EXPECT_THAT(transformMin, TransformEq(Transformd::Scale({2, 2})));
    EXPECT_THAT(transformMax,
                TransformEq(Transformd::Scale({2, 2}) * Transformd::Translate({200, 0})));
  }
}

TEST(PreserveAspectRatio, MinMaxSlice) {
  const Boxd viewBox({0, 0}, {100, 100});
  const PreserveAspectRatio minSlice{PreserveAspectRatio::Align::XMinYMin,
                                     PreserveAspectRatio::MeetOrSlice::Slice};
  const PreserveAspectRatio maxSlice{PreserveAspectRatio::Align::XMaxYMax,
                                     PreserveAspectRatio::MeetOrSlice::Slice};

  // No effect if the box fits.
  EXPECT_TRUE(
      minSlice.elementContentFromViewBoxTransform(Boxd({0, 0}, {100, 100}), viewBox).isIdentity());
  EXPECT_THAT(minSlice.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 50}), viewBox),
              TransformEq(Transformd::Scale({0.5, 0.5})));
  EXPECT_THAT(minSlice.elementContentFromViewBoxTransform(Boxd({0, 0}, {200, 200}), viewBox),
              TransformEq(Transformd::Scale({2, 2})));
  EXPECT_TRUE(
      maxSlice.elementContentFromViewBoxTransform(Boxd({0, 0}, {100, 100}), viewBox).isIdentity());
  EXPECT_THAT(maxSlice.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 50}), viewBox),
              TransformEq(Transformd::Scale({0.5, 0.5})));
  EXPECT_THAT(maxSlice.elementContentFromViewBoxTransform(Boxd({0, 0}, {200, 200}), viewBox),
              TransformEq(Transformd::Scale({2, 2})));

  {
    auto transformMin =
        minSlice.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 200}), viewBox);
    auto transformMax =
        maxSlice.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 200}), viewBox);
    EXPECT_THAT(transformMin, TransformEq(Transformd::Scale({2, 2})));
    EXPECT_THAT(transformMax,
                TransformEq(Transformd::Scale({2, 2}) * Transformd::Translate({-150, 0})));
  }

  {
    auto transformMin =
        minSlice.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 25}), viewBox);
    auto transformMax =
        maxSlice.elementContentFromViewBoxTransform(Boxd({0, 0}, {50, 25}), viewBox);
    EXPECT_THAT(transformMin, TransformEq(Transformd::Scale({0.5, 0.5})));
    EXPECT_THAT(transformMax,
                TransformEq(Transformd::Scale({0.5, 0.5}) * Transformd::Translate({0, -25})));
  }
}

TEST(PreserveAspectRatio, EqualityOperator) {
  // Same align and meetOrSlice => should be equal.
  const PreserveAspectRatio par1{PreserveAspectRatio::Align::XMidYMid,
                                 PreserveAspectRatio::MeetOrSlice::Meet};
  const PreserveAspectRatio par2{PreserveAspectRatio::Align::XMidYMid,
                                 PreserveAspectRatio::MeetOrSlice::Meet};
  EXPECT_EQ(par1, par2);

  // Different align => not equal.
  const PreserveAspectRatio par3{PreserveAspectRatio::Align::XMaxYMid,
                                 PreserveAspectRatio::MeetOrSlice::Meet};
  EXPECT_NE(par1, par3);

  // Different meetOrSlice => not equal.
  const PreserveAspectRatio par4{PreserveAspectRatio::Align::XMidYMid,
                                 PreserveAspectRatio::MeetOrSlice::Slice};
  EXPECT_NE(par1, par4);

  // “None()” vs. explicit “none” => should be equal.
  auto none1 = PreserveAspectRatio::None();
  PreserveAspectRatio none2{PreserveAspectRatio::Align::None,
                            PreserveAspectRatio::MeetOrSlice::Meet};
  EXPECT_EQ(none1, none2);
}

TEST(PreserveAspectRatio, NoViewBoxNonEmptySize) {
  // If no viewBox is given, the doc says we apply a simple translation to the top-left of `size`.
  // Let's pick a non-zero origin and a non-empty size to confirm that code path.
  const Boxd size({10, 20}, {200, 100});
  const PreserveAspectRatio par;  // default: XMidYMid + Meet

  const auto transform = par.elementContentFromViewBoxTransform(size, std::nullopt);
  // We expect the transform to be a pure translation to the new origin.
  // That is, a matrix [1 0 Tx; 0 1 Ty].
  EXPECT_THAT(transform, TransformEq(Transformd::Translate({10, 20})));

  // Points should map exactly by adding the translation.
  EXPECT_THAT(transform.transformPosition({0, 0}), Vector2Near(10, 20));
  EXPECT_THAT(transform.transformPosition({100, 50}), Vector2Near(110, 70));
}

}  // namespace donner::svg
