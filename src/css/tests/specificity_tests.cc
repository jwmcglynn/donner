#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/tests/base_test_utils.h"
#include "src/css/specificity.h"

namespace donner {
namespace css {

static constexpr uint32_t kMaxValue = std::numeric_limits<uint32_t>::max();

TEST(Specificity, ToString) {
  EXPECT_THAT(Specificity(), ToStringIs("Specificity(0, 0, 0)"));
  EXPECT_THAT(Specificity::FromABC(1, 2, 3), ToStringIs("Specificity(1, 2, 3)"));
  EXPECT_THAT(Specificity::Important(), ToStringIs("Specificity(!important)"));
  EXPECT_THAT(Specificity::StyleAttribute(), ToStringIs("Specificity(style (second highest))"));
}

TEST(Specificity, ABCOrder) {
  EXPECT_GT(Specificity::FromABC(0, 0, 1), Specificity());
  EXPECT_GT(Specificity::FromABC(0, 0, 2), Specificity::FromABC(0, 0, 1));
  EXPECT_GT(Specificity::FromABC(0, 1, 0), Specificity::FromABC(0, 0, 10000000));
  EXPECT_GT(Specificity::FromABC(0, 1, 0), Specificity::FromABC(0, 0, kMaxValue));

  EXPECT_GT(Specificity::FromABC(0, 2, 0), Specificity::FromABC(0, 1, 0));
  EXPECT_GT(Specificity::FromABC(1, 0, 0), Specificity::FromABC(0, 10000000, 10000000));
  EXPECT_GT(Specificity::FromABC(1, 0, 0), Specificity::FromABC(0, kMaxValue, kMaxValue));
  EXPECT_GT(Specificity::FromABC(2, 0, 0), Specificity::FromABC(1, 0, 0));
}

TEST(Specificity, SpecialTypes) {
  EXPECT_GT(Specificity::StyleAttribute(), Specificity());
  EXPECT_GT(Specificity::StyleAttribute(), Specificity::FromABC(kMaxValue, kMaxValue, kMaxValue));
  EXPECT_GT(Specificity::Important(), Specificity());
  EXPECT_GT(Specificity::Important(), Specificity::FromABC(kMaxValue, kMaxValue, kMaxValue));
  EXPECT_GT(Specificity::Important(), Specificity::StyleAttribute());
}

}  // namespace css
}  // namespace donner
