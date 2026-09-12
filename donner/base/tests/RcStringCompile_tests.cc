/// @file
/// Optimized default-string moves must neither read uninitialized storage nor change values.

#include <gtest/gtest.h>

#include <vector>

#include "donner/base/RcString.h"

namespace donner {

std::vector<RcString> MakeAlternatingStrings(unsigned count) {
  std::vector<RcString> values;
  for (unsigned index = 0; index < count; ++index) {
    RcString empty;
    values.push_back(std::move(empty));
    values.emplace_back("test");
  }
  return values;
}

TEST(RcStringCompile, DefaultMovesSurviveVectorGrowth) {
  const auto values = MakeAlternatingStrings(32);
  ASSERT_EQ(values.size(), 64u);
  for (size_t index = 0; index < values.size(); ++index) {
    SCOPED_TRACE(index);
    EXPECT_EQ(values[index], index % 2 == 0 ? "" : "test");
  }
}

}  // namespace donner
