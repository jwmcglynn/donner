#include "donner/base/Transform.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner {

TEST(Transform, Construct) {
  Transformf transform_float;
  EXPECT_TRUE(transform_float.isIdentity());

  Transformd transform_double;
  EXPECT_TRUE(transform_double.isIdentity());
}

TEST(Transform, Inverse) {
  {
    Transformd t = Transformd::Rotation(MathConstants<double>::kHalfPi * 0.5);
    EXPECT_THAT(t.inverse(),
                TransformEq(Transformd::Rotation(MathConstants<double>::kHalfPi * -0.5)));
  }

  {
    Transformd t = Transformd::Scale({2, 2});
    EXPECT_THAT(t.inverse(), TransformEq(Transformd::Scale({0.5, 0.5})));
  }

  {
    Transformd t = Transformd::Translate({50, -100});
    EXPECT_THAT(t.inverse(), TransformEq(Transformd::Translate({-50, 100})));
  }

  {
    Transformd t = Transformd::SkewX(0.5);
    EXPECT_THAT(t.inverse(), TransformEq(Transformd::SkewX(-0.5)));
  }

  {
    Transformd t = Transformd::SkewY(0.2);
    EXPECT_THAT(t.inverse(), TransformEq(Transformd::SkewY(-0.2)));
  }

  {
    Transformd t = Transformd::Rotation(MathConstants<double>::kHalfPi * 0.5) *
                   Transformd::Scale({2, 2}) * Transformd::Translate({-50, 100});

    // The inverse should apply the inverse transformations in reverse order
    EXPECT_THAT(t.inverse(),
                TransformEq(Transformd::Translate({50, -100}) * Transformd::Scale({0.5, 0.5}) *
                            Transformd::Rotation(MathConstants<double>::kHalfPi * -0.5)));
  }
}

TEST(Transform, MultiplicationOrder) {
  const double angle = MathConstants<double>::kHalfPi * 0.5;  // 45 degrees
  const double cos45 = std::cos(angle);                       // cos(45 degrees)
  const double sin45 = std::sin(angle);                       // sin(45 degrees)
  const double scaleFactor = 2.0;

  const Transformd t = Transformd::Rotation(angle) * Transformd::Scale({scaleFactor, scaleFactor}) *
                       Transformd::Translate({-50, 100});

  EXPECT_THAT(t, TransformIs(cos45 * scaleFactor,   // a
                             sin45 * scaleFactor,   // b
                             -sin45 * scaleFactor,  // c
                             cos45 * scaleFactor,   // d
                             -50.0,                 // e
                             100.0                  // f
                             ));
}

TEST(Transform, TransformVectorOrPosition) {
  {
    Transformd t = Transformd::Rotation(MathConstants<double>::kHalfPi * 0.5);
    EXPECT_THAT(t.transformVector({100, 100}), Vector2Near(0, 100 * sqrt(2)));
    EXPECT_THAT(t.transformVector({-100, 0}), Vector2Near(-100 / sqrt(2), -100 / sqrt(2)));

    EXPECT_THAT(t.transformPosition({100, 100}), Vector2Near(0, 100 * sqrt(2)));
    EXPECT_THAT(t.transformPosition({-100, 0}), Vector2Near(-100 / sqrt(2), -100 / sqrt(2)));
  }

  {
    Transformd t = Transformd::Scale({-0.5, 2});
    EXPECT_THAT(t.transformVector({100, 100}), Vector2Near(-50, 200));
    EXPECT_THAT(t.transformVector({50, -200}), Vector2Near(-25, -400));

    EXPECT_THAT(t.transformPosition({100, 100}), Vector2Near(-50, 200));
    EXPECT_THAT(t.transformPosition({50, -200}), Vector2Near(-25, -400));
  }

  {
    Transformd t = Transformd::Translate({50, -100});
    EXPECT_THAT(t.transformVector({100, 100}), Vector2Near(100, 100));
    EXPECT_THAT(t.transformVector({50, -200}), Vector2Near(50, -200));

    EXPECT_THAT(t.transformPosition({100, 100}), Vector2Near(150, 0));
    EXPECT_THAT(t.transformPosition({50, -200}), Vector2Near(100, -300));
  }

  {
    Transformd t = Transformd::SkewX(MathConstants<double>::kHalfPi * 0.5);
    EXPECT_THAT(t.transformVector({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(t.transformVector({50, 50}), Vector2Near(100, 50));
    EXPECT_THAT(t.transformVector({50, 100}), Vector2Near(150, 100));

    EXPECT_THAT(t.transformPosition({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(t.transformPosition({50, 50}), Vector2Near(100, 50));
    EXPECT_THAT(t.transformPosition({50, 100}), Vector2Near(150, 100));
  }

  {
    Transformd t = Transformd::SkewY(MathConstants<double>::kHalfPi * -0.5);
    EXPECT_THAT(t.transformVector({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(t.transformVector({50, 50}), Vector2Near(50, 0));
    EXPECT_THAT(t.transformVector({100, 50}), Vector2Near(100, -50));

    EXPECT_THAT(t.transformPosition({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(t.transformPosition({50, 50}), Vector2Near(50, 0));
    EXPECT_THAT(t.transformPosition({100, 50}), Vector2Near(100, -50));
  }

  {
    Transformd t = Transformd::Rotation(MathConstants<double>::kHalfPi) *
                   Transformd::Scale({2, 2}) * Transformd::Translate({-50, 100});

    EXPECT_THAT(t.transformVector({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(t.transformVector({50, 50}), Vector2Near(-100, 100));
    EXPECT_THAT(t.transformVector({100, 50}), Vector2Near(-100, 200));

    EXPECT_THAT(t.transformPosition({0, 0}), Vector2Near(-50, 100));
    EXPECT_THAT(t.transformPosition({50, 50}), Vector2Near(-150, 200));
    EXPECT_THAT(t.transformPosition({100, 50}), Vector2Near(-150, 300));
  }
}

TEST(Transform, TransformBox) {
  {
    Transformd t = Transformd::Rotation(MathConstants<double>::kHalfPi * 0.5);
    EXPECT_THAT(t.transformBox(Boxd({-100, -100}, {100, 100})),
                BoxEq(Vector2Near(-100 * sqrt(2), -100 * sqrt(2)),
                      Vector2Near(100 * sqrt(2), 100 * sqrt(2))));
  }

  {
    Transformd t = Transformd::Scale({-0.5, 2});
    EXPECT_THAT(t.transformBox(Boxd({-200, -50}, {100, 150})),
                BoxEq(Vector2Near(-50, -100), Vector2Near(100, 300)));
  }

  {
    Transformd t = Transformd::Translate({50, -100});
    EXPECT_THAT(t.transformBox(Boxd({-200, -50}, {100, 150})),
                BoxEq(Vector2Near(-150, -150), Vector2Near(150, 50)));
  }
}

TEST(Transform, Output) {
  Transformd t(Transformd::uninitialized);
  t.data[0] = 1.0;
  t.data[1] = -2.0;
  t.data[2] = 3.0;
  t.data[3] = -4.0;
  t.data[4] = 5.0;
  t.data[5] = -6.0;

  EXPECT_EQ((std::ostringstream() << t).str(),
            "matrix(1 -2 3 -4 5 -6) =>\n"
            "[ 1\t3\t5\n"
            "  -2\t-4\t-6\n"
            "  0\t0\t1 ]\n");
}

}  // namespace donner
