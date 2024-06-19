#include <gtest/gtest.h>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/base/tests/BaseTestUtils.h"

namespace donner {

struct ConvertMeToString {
  std::string value;

  explicit ConvertMeToString(std::string value) : value(std::move(value)) {}

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

TEST(Vector2Near, Success) {
  EXPECT_THAT(Vector2d(1.0, 2.0), Vector2Near(1.0, 2.0));
  EXPECT_THAT(Vector2d(1.0, 2.0), Vector2Near(1.00001, 1.9999));
}

TEST(Vector2Near, Failure) {
  testing::StringMatchResultListener resultListener;
  EXPECT_FALSE(
      testing::ExplainMatchResult(Vector2Near(1.0, 2.0), Vector2d(1.1, 2.1), &resultListener));
  EXPECT_EQ(resultListener.str(), "which is 0.1 from 1");

  resultListener.Clear();
  EXPECT_FALSE(
      testing::ExplainMatchResult(Vector2Near(1.0, 2.0), Vector2d(1.0, 2.1), &resultListener));
  EXPECT_EQ(resultListener.str(), "which is 0.1 from 2");
}

TEST(NormalizedEq, Success) {
  EXPECT_THAT(Vector2d(2.0, 0.0), NormalizedEq(Vector2d(1.0, 0.0)));
  EXPECT_THAT(Vector2d(0.0, 2.0), NormalizedEq(Vector2d(0.0, 1.0)));
  EXPECT_THAT(Vector2d(1.0, 1.0), NormalizedEq(Vector2d(0.707, 0.707)));
  EXPECT_THAT(Vector2d(1.0, 2.0), NormalizedEq(Vector2d(0.4472, 0.8944)));
  EXPECT_THAT(Vector2d(1.0, 2.0), NormalizedEq(Vector2d(2.0, 4.0)));
}

TEST(NormalizedEq, Failure) {
  testing::StringMatchResultListener resultListener;
  EXPECT_FALSE(testing::ExplainMatchResult(NormalizedEq(Vector2d(1.0, 1.0)), Vector2d(1.0, 2.0),
                                           &resultListener));
  EXPECT_EQ(resultListener.str(), "which is -0.259893 from 0.707107");

  resultListener.Clear();
  EXPECT_FALSE(testing::ExplainMatchResult(NormalizedEq(Vector2d(0.0, 2.0)), Vector2d(1.0, 1.0),
                                           &resultListener));
  EXPECT_EQ(resultListener.str(), "which is 0.707107 from 0");
}

TEST(TransformEq, Success) {
  EXPECT_THAT(Transformd(), TransformEq(Transformd()));
  EXPECT_THAT(Transformd(), TransformEq(Transformd::Translate(Vector2d(0.0, 0.0))));
  EXPECT_THAT(Transformd::Translate(Vector2d(1.0, 2.0)),
              TransformEq(Transformd::Translate(Vector2d(1.0, 2.0))));
  EXPECT_THAT(Transformd::Translate(Vector2d(1.0, 2.0)),
              TransformEq(Transformd::Translate(Vector2d(1.00001, 1.9999))));
}

TEST(TransformEq, Failure) {
  testing::StringMatchResultListener resultListener;
  EXPECT_FALSE(testing::ExplainMatchResult(TransformEq(Transformd::Translate(Vector2d(1.0, 2.0))),
                                           Transformd::Translate(Vector2d(1.1, 2.1)),
                                           &resultListener));
  EXPECT_EQ(resultListener.str(),
            "where the value pair (1.1, 1) at index #4 don't match, which is -0.1 from 1.1");

  resultListener.Clear();
  EXPECT_FALSE(testing::ExplainMatchResult(TransformEq(Transformd::Translate(Vector2d(1.0, 2.0))),
                                           Transformd::Translate(Vector2d(1.0, 2.1)),
                                           &resultListener));
  EXPECT_EQ(resultListener.str(),
            "where the value pair (2.1, 2) at index #5 don't match, which is -0.1 from 2.1");
}

TEST(TransformIs, Success) {
  EXPECT_THAT(Transformd(), TransformIs(1.0, 0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_THAT(Transformd::Translate(Vector2d(1.0, 2.0)), TransformIs(1.0, 0.0, 0.0, 1.0, 1.0, 2.0));
  EXPECT_THAT(Transformd::Scale(Vector2d(2.0, 3.0)), TransformIs(2.0, 0.0, 0.0, 3.0, 0.0, 0.0));
  EXPECT_THAT(Transformd::Rotation(0.5),
              TransformIs(0.877582, 0.479426, -0.479426, 0.877582, 0.0, 0.0));
}

TEST(TransformIs, Failure) {
  testing::StringMatchResultListener resultListener;
  EXPECT_FALSE(testing::ExplainMatchResult(TransformIs(1.0, 0.0, 0.0, 1.0, 1.0, 2.0),
                                           Transformd::Translate(Vector2d(1.1, 2.1)),
                                           &resultListener));
  EXPECT_EQ(resultListener.str(), "whose element #4 doesn't match, which is 0.1 from 1");

  resultListener.Clear();
  EXPECT_FALSE(testing::ExplainMatchResult(TransformIs(1.0, 0.0, 0.0, 1.0, 1.0, 2.0),
                                           Transformd::Translate(Vector2d(1.0, 2.1)),
                                           &resultListener));
  EXPECT_EQ(resultListener.str(), "whose element #5 doesn't match, which is 0.1 from 2");
}

TEST(TransformIsIdentity, Success) {
  EXPECT_THAT(Transformd(), TransformIsIdentity());
  EXPECT_THAT(Transformd::Translate(Vector2d(0.0, 0.0)), TransformIsIdentity());
}

TEST(TransformIsIdentity, Failure) {
  testing::StringMatchResultListener resultListener;
  EXPECT_FALSE(testing::ExplainMatchResult(
      TransformIsIdentity(), Transformd::Translate(Vector2d(1.0, 2.0)), &resultListener));
  EXPECT_EQ(resultListener.str(), "");
}

TEST(BoxEq, Success) {
  EXPECT_THAT(Boxd::CreateEmpty(Vector2d()), BoxEq(Vector2d(), Vector2d()));
  EXPECT_THAT(Boxd(Vector2d(1.0, 2.0), Vector2d(3.0, 4.0)),
              BoxEq(Vector2d(1.0, 2.0), Vector2d(3.0, 4.0)));
  EXPECT_THAT(Boxd(Vector2d(1.0, 2.0), Vector2d(3.0, 4.0)),
              BoxEq(Vector2Near(1.00001, 1.9999), Vector2Near(3.00001, 4.00001)));
}

TEST(BoxEq, Failure) {
  testing::StringMatchResultListener resultListener;
  EXPECT_FALSE(testing::ExplainMatchResult(BoxEq(Vector2d(1.0, 2.0), Vector2d(3.0, 4.0)),
                                           Boxd(Vector2d(1.1, 2.1), Vector2d(3.1, 4.1)),
                                           &resultListener));
  EXPECT_EQ(resultListener.str(), "");

  resultListener.Clear();
  EXPECT_FALSE(testing::ExplainMatchResult(BoxEq(Vector2d(1.0, 2.0), Vector2d(3.0, 4.0)),
                                           Boxd(Vector2d(1.0, 2.1), Vector2d(3.1, 4.1)),
                                           &resultListener));
  EXPECT_EQ(resultListener.str(), "");
}

}  // namespace donner
