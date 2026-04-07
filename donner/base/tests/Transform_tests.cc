#include "donner/base/Transform.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner {

TEST(Transform, Construct) {
  Transform2f transform_float;
  EXPECT_TRUE(transform_float.isIdentity());

  Transform2d transform_double;
  EXPECT_TRUE(transform_double.isIdentity());
}

TEST(Transform, Rotate) {
  {
    Transform2d t = Transform2d::Rotate(MathConstants<double>::kHalfPi);
    EXPECT_THAT(t, TransformIs(0, 1, -1, 0, 0, 0));
  }

  {
    Transform2d t = Transform2d::Rotate(-MathConstants<double>::kHalfPi);
    EXPECT_THAT(t, TransformIs(0, -1, 1, 0, 0, 0));
  }
}

TEST(Transform, Scale) {
  {
    Transform2d t = Transform2d::Scale(2);
    EXPECT_THAT(t, TransformIs(2, 0, 0, 2, 0, 0));
  }

  {
    Transform2d t = Transform2d::Scale(0.5);
    EXPECT_THAT(t, TransformIs(0.5, 0, 0, 0.5, 0, 0));
  }

  {
    Transform2d t = Transform2d::Scale({-1, 1});
    EXPECT_THAT(t, TransformIs(-1, 0, 0, 1, 0, 0));
  }

  {
    Transform2d t = Transform2d::Scale(1, -1);
    EXPECT_THAT(t, TransformIs(1, 0, 0, -1, 0, 0));
  }
}

TEST(Transform, Translate) {
  {
    Transform2d t = Transform2d::Translate({50, -100});
    EXPECT_THAT(t, TransformIs(1, 0, 0, 1, 50, -100));
  }

  {
    Transform2d t = Transform2d::Translate(-50, 100);
    EXPECT_THAT(t, TransformIs(1, 0, 0, 1, -50, 100));
  }
}

TEST(Transform, SkewX) {
  {
    Transform2d t = Transform2d::SkewX(MathConstants<double>::kHalfPi * 0.5);
    EXPECT_THAT(t, TransformIs(1, 0, 1, 1, 0, 0));
  }

  {
    Transform2d t = Transform2d::SkewX(-MathConstants<double>::kHalfPi * 0.5);
    EXPECT_THAT(t, TransformIs(1, 0, -1, 1, 0, 0));
  }
}

TEST(Transform, SkewY) {
  {
    Transform2d t = Transform2d::SkewY(MathConstants<double>::kHalfPi * 0.5);
    EXPECT_THAT(t, TransformIs(1, 1, 0, 1, 0, 0));
  }

  {
    Transform2d t = Transform2d::SkewY(-MathConstants<double>::kHalfPi * 0.5);
    EXPECT_THAT(t, TransformIs(1, -1, 0, 1, 0, 0));
  }
}

TEST(Transform, IsIdentity) {
  Transform2d defaultConstruct;
  EXPECT_TRUE(defaultConstruct.isIdentity());

  Transform2d noTranslation = Transform2d::Translate(0, 0);
  EXPECT_TRUE(noTranslation.isIdentity());
}

TEST(Transform, Determinant) {
  {
    Transform2d t = Transform2d::Rotate(MathConstants<double>::kHalfPi * 0.5);
    EXPECT_EQ(t.determinant(), 1);
  }

  {
    Transform2d t = Transform2d::Scale({2, 2});
    EXPECT_EQ(t.determinant(), 4);
  }
}

TEST(Transform, Inverse) {
  {
    Transform2d t = Transform2d::Rotate(MathConstants<double>::kHalfPi * 0.5);
    EXPECT_THAT(t.inverse(),
                TransformEq(Transform2d::Rotate(MathConstants<double>::kHalfPi * -0.5)));
  }

  {
    Transform2d t = Transform2d::Scale({2, 2});
    EXPECT_THAT(t.inverse(), TransformEq(Transform2d::Scale({0.5, 0.5})));
  }

  {
    Transform2d t = Transform2d::Translate({50, -100});
    EXPECT_THAT(t.inverse(), TransformEq(Transform2d::Translate({-50, 100})));
  }

  {
    Transform2d t = Transform2d::SkewX(0.5);
    EXPECT_THAT(t.inverse(), TransformEq(Transform2d::SkewX(-0.5)));
  }

  {
    Transform2d t = Transform2d::SkewY(0.2);
    EXPECT_THAT(t.inverse(), TransformEq(Transform2d::SkewY(-0.2)));
  }

  {
    Transform2d t = Transform2d::Rotate(MathConstants<double>::kHalfPi * 0.5) *
                   Transform2d::Scale({2, 2}) * Transform2d::Translate({-50, 100});

    // The inverse should apply the inverse transformations in reverse order
    EXPECT_THAT(t.inverse(),
                TransformEq(Transform2d::Translate({50, -100}) * Transform2d::Scale({0.5, 0.5}) *
                            Transform2d::Rotate(MathConstants<double>::kHalfPi * -0.5)));
  }
}

TEST(Transform, MultiplicationOrder) {
  const double angle = MathConstants<double>::kHalfPi * 0.5;  // 45 degrees
  const double cos45 = std::cos(angle);                       // cos(45 degrees)
  const double sin45 = std::sin(angle);                       // sin(45 degrees)
  const double scaleFactor = 2.0;

  const Transform2d t = Transform2d::Rotate(angle) * Transform2d::Scale({scaleFactor, scaleFactor}) *
                       Transform2d::Translate({-50, 100});

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
    Transform2d t = Transform2d::Rotate(MathConstants<double>::kHalfPi * 0.5);
    EXPECT_THAT(t.transformVector({100, 100}), Vector2Near(0, 100 * sqrt(2)));
    EXPECT_THAT(t.transformVector({-100, 0}), Vector2Near(-100 / sqrt(2), -100 / sqrt(2)));

    EXPECT_THAT(t.transformPosition({100, 100}), Vector2Near(0, 100 * sqrt(2)));
    EXPECT_THAT(t.transformPosition({-100, 0}), Vector2Near(-100 / sqrt(2), -100 / sqrt(2)));
  }

  {
    Transform2d t = Transform2d::Scale({-0.5, 2});
    EXPECT_THAT(t.transformVector({100, 100}), Vector2Near(-50, 200));
    EXPECT_THAT(t.transformVector({50, -200}), Vector2Near(-25, -400));

    EXPECT_THAT(t.transformPosition({100, 100}), Vector2Near(-50, 200));
    EXPECT_THAT(t.transformPosition({50, -200}), Vector2Near(-25, -400));
  }

  {
    Transform2d t = Transform2d::Translate({50, -100});
    EXPECT_THAT(t.transformVector({100, 100}), Vector2Near(100, 100));
    EXPECT_THAT(t.transformVector({50, -200}), Vector2Near(50, -200));

    EXPECT_THAT(t.transformPosition({100, 100}), Vector2Near(150, 0));
    EXPECT_THAT(t.transformPosition({50, -200}), Vector2Near(100, -300));
  }

  {
    Transform2d t = Transform2d::SkewX(MathConstants<double>::kHalfPi * 0.5);
    EXPECT_THAT(t.transformVector({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(t.transformVector({50, 50}), Vector2Near(100, 50));
    EXPECT_THAT(t.transformVector({50, 100}), Vector2Near(150, 100));

    EXPECT_THAT(t.transformPosition({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(t.transformPosition({50, 50}), Vector2Near(100, 50));
    EXPECT_THAT(t.transformPosition({50, 100}), Vector2Near(150, 100));
  }

  {
    Transform2d t = Transform2d::SkewY(MathConstants<double>::kHalfPi * -0.5);
    EXPECT_THAT(t.transformVector({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(t.transformVector({50, 50}), Vector2Near(50, 0));
    EXPECT_THAT(t.transformVector({100, 50}), Vector2Near(100, -50));

    EXPECT_THAT(t.transformPosition({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(t.transformPosition({50, 50}), Vector2Near(50, 0));
    EXPECT_THAT(t.transformPosition({100, 50}), Vector2Near(100, -50));
  }

  {
    Transform2d t = Transform2d::Rotate(MathConstants<double>::kHalfPi) * Transform2d::Scale({2, 2}) *
                   Transform2d::Translate({-50, 100});

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
    Transform2d t = Transform2d::Rotate(MathConstants<double>::kHalfPi * 0.5);
    EXPECT_THAT(t.transformBox(Box2d({-100, -100}, {100, 100})),
                BoxEq(Vector2Near(-100 * sqrt(2), -100 * sqrt(2)),
                      Vector2Near(100 * sqrt(2), 100 * sqrt(2))));
  }

  {
    Transform2d t = Transform2d::Scale({-0.5, 2});
    EXPECT_THAT(t.transformBox(Box2d({-200, -50}, {100, 150})),
                BoxEq(Vector2Near(-50, -100), Vector2Near(100, 300)));
  }

  {
    Transform2d t = Transform2d::Translate({50, -100});
    EXPECT_THAT(t.transformBox(Box2d({-200, -50}, {100, 150})),
                BoxEq(Vector2Near(-150, -150), Vector2Near(150, 50)));
  }
}

TEST(Transform, Output) {
  Transform2d t(Transform2d::uninitialized);
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
