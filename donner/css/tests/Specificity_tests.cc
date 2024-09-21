#include "donner/css/Specificity.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/css/tests/SelectorTestUtils.h"

namespace donner::css {

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

TEST(Specificity, SpecialValue) {
  EXPECT_EQ(Specificity::FromABC(0, 0, 0).specialValue(), Specificity::SpecialType::None);
  EXPECT_EQ(Specificity::Important().specialValue(), Specificity::SpecialType::Important);
  EXPECT_EQ(Specificity::StyleAttribute().specialValue(), Specificity::SpecialType::StyleAttribute);
  EXPECT_EQ(Specificity::Override().specialValue(), Specificity::SpecialType::Override);
  EXPECT_EQ(Specificity::FromABC(0, 0, 0).toUserAgentSpecificity().specialValue(),
            Specificity::SpecialType::UserAgent);
}

TEST(Specificity, ToUserAgentSpecificity) {
  EXPECT_LT(Specificity::FromABC(0, 0, 0).toUserAgentSpecificity(), Specificity::FromABC(0, 0, 0));
  EXPECT_EQ(Specificity::Important().toUserAgentSpecificity(), Specificity::Important());
  EXPECT_EQ(Specificity::StyleAttribute().toUserAgentSpecificity(), Specificity::StyleAttribute());
  EXPECT_EQ(Specificity::Override().toUserAgentSpecificity(), Specificity::Override());
}

TEST(Specificity, Selectors) {
  EXPECT_EQ(computeSpecificity("test"), Specificity::FromABC(0, 0, 1));
  EXPECT_EQ(computeSpecificity("#id"), Specificity::FromABC(1, 0, 0));
  EXPECT_EQ(computeSpecificity(".class"), Specificity::FromABC(0, 1, 0));
  EXPECT_EQ(computeSpecificity("div"), Specificity::FromABC(0, 0, 1));
  EXPECT_EQ(computeSpecificity("#id.class"), Specificity::FromABC(1, 1, 0));
  EXPECT_EQ(computeSpecificity("#id.class div"), Specificity::FromABC(1, 1, 1));
  EXPECT_EQ(computeSpecificity("#id.class > div"), Specificity::FromABC(1, 1, 1));
  EXPECT_EQ(computeSpecificity("[class~=\"class\"]"), Specificity::FromABC(0, 1, 0));
}

}  // namespace donner::css
