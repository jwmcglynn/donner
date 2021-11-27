#include <gtest/gtest.h>

#include "src/base/tests/base_test_utils.h"
#include "src/base/vector2.h"

namespace donner {

struct ConvertMeToString {
  std::string value;

  ConvertMeToString(std::string value) : value(std::move(value)) {}

  friend std::ostream& operator<<(std::ostream& os, const ConvertMeToString& obj) {
    return os << "ConvertMeToString: " << obj.value;
  }
};

TEST(ToStringIs, Success) {
  EXPECT_THAT(ConvertMeToString("foo"), ToStringIs("ConvertMeToString: foo"));
}

TEST(ToStringIs, Failure) {
  testing::StringMatchResultListener resultListener;
  EXPECT_FALSE(testing::ExplainMatchResult(ToStringIs("ConvertMeToString: result"),
                                           ConvertMeToString("different"), &resultListener));

  EXPECT_EQ(resultListener.str(),
            "\nExpected string: ConvertMeToString: result\nMatching subset: ConvertMeToString: ");
}

TEST(Vector2Eq, Success) {
  EXPECT_THAT(Vector2i(1, 2), Vector2Eq(1, 2));
  EXPECT_THAT(Vector2i(1, 2), Vector2Eq(1, testing::_));
  EXPECT_THAT(Vector2i(1, 2), Vector2Eq(testing::Ge(0), 2));
}

TEST(Vector2Eq, Failure) {
  testing::StringMatchResultListener resultListener;
  EXPECT_FALSE(
      testing::ExplainMatchResult(Vector2Eq(testing::Ge(2), 2), Vector2i(1, 2), &resultListener));
  EXPECT_EQ(resultListener.str(), "");
}

// TODO: Coverage for remaining base test utils.

}  // namespace donner
