#include <gtest/gtest.h>

#include "src/base/transform.h"

namespace donner {

TEST(Transform, Construct) {
  Transformf transform_float;
  EXPECT_TRUE(transform_float.isIdentity());

  Transformd transform_double;
  EXPECT_TRUE(transform_double.isIdentity());
}

}  // namespace donner