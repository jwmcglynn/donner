#include "donner/base/Box.h"

#include <gtest/gtest.h>

#include <sstream>

namespace donner {

TEST(Box, Construct) {
  Box2d box(Vector2d(-1.0, -1.0), Vector2d(1.0, 1.0));
  EXPECT_EQ(box.topLeft, Vector2d(-1.0, -1.0));
  EXPECT_EQ(box.bottomRight, Vector2d(1.0, 1.0));
}

TEST(Box, CreateEmpty) {
  Box2d empty = Box2d::CreateEmpty(Vector2d(2.0, 1.0));
  EXPECT_EQ(empty.topLeft, Vector2d(2.0, 1.0));
  EXPECT_EQ(empty.bottomRight, Vector2d(2.0, 1.0));
}

TEST(Box, WithSize) {
  Box2d empty = Box2d::WithSize(Vector2d(4.0, 3.0));
  EXPECT_EQ(empty.topLeft, Vector2d(0, 0));
  EXPECT_EQ(empty.bottomRight, Vector2d(4.0, 3.0));
}

TEST(Box, AddPoint) {
  Box2d box(Vector2d(-1.0, -1.0), Vector2d(1.0, 1.0));

  // Zero is already in the box, this should no-op.
  box.addPoint(Vector2d::Zero());
  EXPECT_EQ(box, Box2d(Vector2d(-1.0, -1.0), Vector2d(1.0, 1.0)));

  // Expand the box in each dimension.
  box.addPoint(Vector2d(2.0, 0.0));
  EXPECT_EQ(box, Box2d(Vector2d(-1.0, -1.0), Vector2d(2.0, 1.0)));
  box.addPoint(Vector2d(0.0, 3.0));
  EXPECT_EQ(box, Box2d(Vector2d(-1.0, -1.0), Vector2d(2.0, 3.0)));
  box.addPoint(Vector2d(-4.0, 0.0));
  EXPECT_EQ(box, Box2d(Vector2d(-4.0, -1.0), Vector2d(2.0, 3.0)));
  box.addPoint(Vector2d(0.0, -5.0));
  EXPECT_EQ(box, Box2d(Vector2d(-4.0, -5.0), Vector2d(2.0, 3.0)));
}

TEST(Box, AddPointFromEmpty) {
  Box2d box = Box2d::CreateEmpty(Vector2d::Zero());

  // Zero is already in the box, this should no-op.
  box.addPoint(Vector2d::Zero());
  EXPECT_EQ(box, Box2d(Vector2d::Zero(), Vector2d::Zero()));

  box.addPoint(Vector2d(2.0, 0.0));
  EXPECT_EQ(box, Box2d(Vector2d(0.0, 0.0), Vector2d(2.0, 0.0)));
}

TEST(Box, AddBox) {
  Box2d box(Vector2d(1.0, 2.0), Vector2d(3.0, 4.0));
  box.addBox(Box2d(Vector2d(5.0, 6.0), Vector2d(7.0, 8.0)));
  EXPECT_EQ(box, Box2d(Vector2d(1.0, 2.0), Vector2d(7.0, 8.0)));
}

TEST(Box, ToOrigin) {
  // Test with negative coordinates
  EXPECT_EQ(Box2d(Vector2d(-3.0, -4.0), Vector2d(-1.0, -2.0)).toOrigin(),
            Box2d(Vector2d::Zero(), Vector2d(2.0, 2.0)));

  // Test with mixed positive and negative coordinates
  EXPECT_EQ(Box2d(Vector2d(-2.0, 1.0), Vector2d(2.0, 5.0)).toOrigin(),
            Box2d(Vector2d::Zero(), Vector2d(4.0, 4.0)));

  // Test with zero-width box
  EXPECT_EQ(Box2d(Vector2d(3.0, 3.0), Vector2d(3.0, 5.0)).toOrigin(),
            Box2d(Vector2d::Zero(), Vector2d(0.0, 2.0)));

  // Test with zero-height box
  EXPECT_EQ(Box2d(Vector2d(1.0, 4.0), Vector2d(5.0, 4.0)).toOrigin(),
            Box2d(Vector2d::Zero(), Vector2d(4.0, 0.0)));

  // Test with point (zero-width and zero-height)
  EXPECT_EQ(Box2d(Vector2d(2.0, 2.0), Vector2d(2.0, 2.0)).toOrigin(),
            Box2d(Vector2d::Zero(), Vector2d::Zero()));

  // Test with box already at origin
  EXPECT_EQ(Box2d(Vector2d::Zero(), Vector2d(3.0, 3.0)).toOrigin(),
            Box2d(Vector2d::Zero(), Vector2d(3.0, 3.0)));

  // Test with very large coordinates
  EXPECT_EQ(Box2d(Vector2d(1e6, 2e6), Vector2d(3e6, 5e6)).toOrigin(),
            Box2d(Vector2d::Zero(), Vector2d(2e6, 3e6)));

  // Test with very small coordinates
  EXPECT_EQ(Box2d(Vector2d(1e-6, 2e-6), Vector2d(3e-6, 5e-6)).toOrigin(),
            Box2d(Vector2d::Zero(), Vector2d(2e-6, 3e-6)));
}

TEST(Box, WidthHeight) {
  const Box2d kEmpty = Box2d::CreateEmpty(Vector2d(2.0, 1.0));
  EXPECT_EQ(kEmpty.width(), 0.0);
  EXPECT_EQ(kEmpty.height(), 0.0);

  const Box2d kBox1(Vector2d(1.0, 2.0), Vector2d(3.0, 5.0));
  EXPECT_EQ(kBox1.width(), 2.0);
  EXPECT_EQ(kBox1.height(), 3.0);
}

TEST(Box, Size) {
  const Box2d kEmpty = Box2d::CreateEmpty(Vector2d(2.0, 1.0));
  EXPECT_EQ(kEmpty.size(), Vector2d());

  const Box2d kBox1(Vector2d(1.0, 2.0), Vector2d(3.0, 5.0));
  EXPECT_EQ(kBox1.size(), Vector2d(2.0, 3.0));
}

TEST(Box, IsEmpty) {
  const Box2d kEmpty = Box2d::CreateEmpty(Vector2d(2.0, 1.0));
  EXPECT_TRUE(kEmpty.isEmpty());

  const Box2d kBox1(Vector2d(1.0, 2.0), Vector2d(3.0, 5.0));
  EXPECT_FALSE(kBox1.isEmpty());
}

TEST(Box, Contains) {
  const Box2d kBox(Vector2d(-1.0, -1.0), Vector2d(1.0, 1.0));

  // Test points inside the box
  EXPECT_TRUE(kBox.contains(Vector2d(0.0, 0.0)));
  EXPECT_TRUE(kBox.contains(Vector2d(-1.0, -1.0)));
  EXPECT_TRUE(kBox.contains(Vector2d(1.0, 1.0)));

  // Test points on the edge of the box
  EXPECT_TRUE(kBox.contains(Vector2d(1.0, 0.0)));
  EXPECT_TRUE(kBox.contains(Vector2d(-1.0, 0.0)));
  EXPECT_TRUE(kBox.contains(Vector2d(0.0, 1.0)));
  EXPECT_TRUE(kBox.contains(Vector2d(0.0, -1.0)));

  // Test points outside the box
  EXPECT_FALSE(kBox.contains(Vector2d(2.0, 0.0)));
  EXPECT_FALSE(kBox.contains(Vector2d(0.0, 2.0)));
  EXPECT_FALSE(kBox.contains(Vector2d(-2.0, 0.0)));
  EXPECT_FALSE(kBox.contains(Vector2d(0.0, -2.0)));
}

TEST(Box, InflatedBy) {
  const Box2d kBox(Vector2d(1.0, 2.0), Vector2d(3.0, 4.0));

  // Inflate by 1.0 in all directions
  Box2d inflatedBox = kBox.inflatedBy(1.0);
  EXPECT_EQ(inflatedBox, Box2d(Vector2d(0.0, 1.0), Vector2d(4.0, 5.0)));

  // Inflate by 0.0 (no change)
  inflatedBox = kBox.inflatedBy(0.0);
  EXPECT_EQ(inflatedBox, kBox);

  // Inflate by a negative value (shrink the box)
  inflatedBox = kBox.inflatedBy(-0.5);
  EXPECT_EQ(inflatedBox, Box2d(Vector2d(1.5, 2.5), Vector2d(2.5, 3.5)));

  // Inflate by a large value
  inflatedBox = kBox.inflatedBy(10.0);
  EXPECT_EQ(inflatedBox, Box2d(Vector2d(-9.0, -8.0), Vector2d(13.0, 14.0)));
}

// Operators
TEST(Box, OperatorAssign) {
  const Box2d kBox1(Vector2d(1.0, 2.0), Vector2d(3.0, 4.0));
  const Box2d kBox2(Vector2d(5.0, 6.0), Vector2d(7.0, 8.0));

  Box2d box(kBox1);
  EXPECT_EQ(box, kBox1);

  box = kBox2;
  EXPECT_EQ(box, kBox2);
}

TEST(Box, OperatorAdd) {
  EXPECT_EQ(Box2d(Vector2d(1.0, 2.0), Vector2d(3.0, 4.0)) + Vector2d(-1.0, 1.0),
            Box2d(Vector2d(0.0, 3.0), Vector2d(2.0, 5.0)));

  Box2d box(Vector2d::Zero(), Vector2d::Zero());
  box += Vector2d(5.0, 10.0);
  EXPECT_EQ(box, Box2d(Vector2d(5.0, 10.0), Vector2d(5.0, 10.0)));
}

TEST(Box, OperatorSubtract) {
  EXPECT_EQ(Box2d(Vector2d(1.0, 2.0), Vector2d(3.0, 4.0)) - Vector2d(-1.0, 1.0),
            Box2d(Vector2d(2.0, 1.0), Vector2d(4.0, 3.0)));

  Box2d box(Vector2d::Zero(), Vector2d::Zero());
  box -= Vector2d(5.0, 10.0);
  EXPECT_EQ(box, Box2d(Vector2d(-5.0, -10.0), Vector2d(-5.0, -10.0)));
}

TEST(Box, Equals) {
  EXPECT_TRUE(Box2d(Vector2d(0.0, 0.0), Vector2d(1.0, 1.0)) ==
              Box2d(Vector2d(0.0, 0.0), Vector2d(1.0, 1.0)));
  EXPECT_FALSE(Box2d(Vector2d(0.0, 0.0), Vector2d(1.0, 1.0)) ==
               Box2d(Vector2d(1.0, 0.0), Vector2d(1.0, 1.0)));
  EXPECT_TRUE(Box2d(Vector2d(0.0, 0.0), Vector2d(1.0, 1.0)) !=
              Box2d(Vector2d(1.0, 0.0), Vector2d(1.0, 1.0)));
  EXPECT_FALSE(Box2d(Vector2d(0.0, 0.0), Vector2d(1.0, 1.0)) !=
               Box2d(Vector2d(0.0, 0.0), Vector2d(1.0, 1.0)));
}

TEST(Box, Output) {
  {
    std::ostringstream ss;
    ss << Box2d(Vector2d(1.0, 2.0), Vector2d(3.0, 4.0));
    EXPECT_EQ(ss.str(), "(1, 2) => (3, 4)");
  }

  {
    std::ostringstream ss;
    ss << Box2d(Vector2d(-0.5, -1), Vector2d(-2, -2.5));
    EXPECT_EQ(ss.str(), "(-0.5, -1) => (-2, -2.5)");
  }
}

}  // namespace donner
