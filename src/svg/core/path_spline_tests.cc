#include "src/svg/core/path_spline.h"

#include <gtest/gtest.h>

namespace donner {

TEST(PathSpline, Empty) {
  auto builder = PathSpline::Builder();
  PathSpline spline = builder.Build();
  EXPECT_TRUE(spline.IsEmpty());
}

}  // namespace donner
