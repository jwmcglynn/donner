#include "donner/base/OptionalRef.h"

#include <gtest/gtest.h>

#include <sstream>

#include "donner/base/Utils.h"

namespace donner {

namespace {

bool isSet(const OptionalRef<int>& ref) {
  return ref.hasValue();
}

}  // namespace

TEST(OptionalRef, DefaultConstruct) {
  OptionalRef<int> ref;
  EXPECT_FALSE(ref.hasValue());
  EXPECT_FALSE(static_cast<bool>(ref));
  EXPECT_EQ(ref, std::nullopt);
}

TEST(OptionalRef, ConstructFromNullopt) {
  OptionalRef<int> ref(std::nullopt);
  EXPECT_FALSE(ref.hasValue());
  EXPECT_FALSE(static_cast<bool>(ref));
  EXPECT_EQ(ref, std::nullopt);
}

TEST(OptionalRef, ConstructFromValue) {
  int x = 42;
  OptionalRef<int> ref(x);
  EXPECT_TRUE(ref.hasValue());
  EXPECT_TRUE(static_cast<bool>(ref));
  EXPECT_EQ(ref.value(), 42);
  EXPECT_EQ(*ref, 42);
  EXPECT_EQ(ref, x);
}

TEST(OptionalRef, CopyConstruct) {
  int x = 42;
  OptionalRef<int> ref1(x);
  OptionalRef<int> ref2(ref1);
  EXPECT_TRUE(ref2.hasValue());
  EXPECT_EQ(ref2.value(), 42);
}

TEST(OptionalRef, CopyConstructEmpty) {
  OptionalRef<int> ref1;
  OptionalRef<int> ref2(ref1);
  EXPECT_FALSE(ref2.hasValue());
}

TEST(OptionalRef, MoveConstruct) {
  int x = 42;
  OptionalRef<int> ref1(x);
  OptionalRef<int> ref2(std::move(ref1));  // NOLINT
  EXPECT_TRUE(ref2.hasValue());
  EXPECT_EQ(ref2.value(), 42);
  EXPECT_TRUE(ref1.hasValue());  // ref1 remains valid
  EXPECT_EQ(ref1.value(), 42);
}

TEST(OptionalRef, MoveConstructEmpty) {
  OptionalRef<int> ref1;
  OptionalRef<int> ref2(std::move(ref1));  // NOLINT
  EXPECT_FALSE(ref2.hasValue());
  EXPECT_FALSE(ref1.hasValue());
}

TEST(OptionalRef, CopyAssign) {
  int x = 42;
  OptionalRef<int> ref1(x);
  OptionalRef<int> ref2;
  ref2 = ref1;
  EXPECT_TRUE(ref2.hasValue());
  EXPECT_EQ(ref2.value(), 42);
}

TEST(OptionalRef, CopyAssignEmpty) {
  OptionalRef<int> ref1;
  OptionalRef<int> ref2;
  ref2 = ref1;
  EXPECT_FALSE(ref2.hasValue());
}

TEST(OptionalRef, MoveAssign) {
  int x = 42;
  OptionalRef<int> ref1(x);
  OptionalRef<int> ref2;
  ref2 = std::move(ref1);  // NOLINT
  EXPECT_TRUE(ref2.hasValue());
  EXPECT_EQ(ref2.value(), 42);
  EXPECT_TRUE(ref1.hasValue());  // ref1 remains valid
  EXPECT_EQ(ref1.value(), 42);
}

TEST(OptionalRef, MoveAssignEmpty) {
  OptionalRef<int> ref1;
  OptionalRef<int> ref2;
  ref2 = std::move(ref1);  // NOLINT
  EXPECT_FALSE(ref2.hasValue());
}

TEST(OptionalRef, AssignValue) {
  int x = 42;
  OptionalRef<int> ref;
  ref = x;
  EXPECT_TRUE(ref.hasValue());
  EXPECT_EQ(ref.value(), 42);
}

TEST(OptionalRef, Reset) {
  int x = 42;
  OptionalRef<int> ref(x);
  EXPECT_TRUE(ref.hasValue());
  ref.reset();
  EXPECT_FALSE(ref.hasValue());
  EXPECT_EQ(ref, std::nullopt);
}

TEST(OptionalRef, ValueWhenEmpty) {
  OptionalRef<int> ref;
#if UTILS_EXCEPTIONS_ENABLED()
  EXPECT_THROW(ref.value(), std::runtime_error);
#else
  EXPECT_DEATH(ref.value(), ".*OptionalRef::value\\(\\) called on empty OptionalRef.*");
#endif
}

TEST(OptionalRef, DereferenceOperator) {
  int x = 42;
  OptionalRef<int> ref(x);
  EXPECT_EQ(*ref, 42);
}

TEST(OptionalRef, DereferenceOperatorEmpty) {
  OptionalRef<int> ref;
#if UTILS_EXCEPTIONS_ENABLED()
  EXPECT_THROW(*ref, std::runtime_error);
#else
  EXPECT_DEATH(*ref, ".*OptionalRef::value\\(\\) called on empty OptionalRef.*");
#endif
}

struct TestStruct {
  int value;
};

TEST(OptionalRef, ArrowOperator) {
  TestStruct s{42};
  OptionalRef<TestStruct> ref(s);
  EXPECT_EQ(ref->value, 42);
}

TEST(OptionalRef, ArrowOperatorEmpty) {
  OptionalRef<TestStruct> ref;
#if UTILS_EXCEPTIONS_ENABLED()
  EXPECT_THROW((void)ref->value, std::runtime_error);
#else
  EXPECT_DEATH((void)ref->value, ".*OptionalRef::operator->\\(\\) called on empty OptionalRef.*");
#endif
}

TEST(OptionalRef, CompareWithOptionalRef) {
  int x = 42;
  int y = 43;
  OptionalRef<int> ref1(x);
  OptionalRef<int> ref2(x);
  OptionalRef<int> ref3(y);
  OptionalRef<int> refEmpty;

  EXPECT_TRUE(ref1 == ref2);
  EXPECT_FALSE(ref1 == ref3);
  EXPECT_FALSE(ref1 == refEmpty);
  EXPECT_TRUE(refEmpty == refEmpty);
}

TEST(OptionalRef, CompareWithNullopt) {
  int x = 42;
  OptionalRef<int> ref(x);
  OptionalRef<int> refEmpty;

  EXPECT_FALSE(ref == std::nullopt);
  EXPECT_TRUE(refEmpty == std::nullopt);
}

TEST(OptionalRef, CompareWithValue) {
  int x = 42;
  int y = 43;
  OptionalRef<int> ref(x);
  OptionalRef<int> refEmpty;

  EXPECT_TRUE(ref == x);
  EXPECT_FALSE(ref == y);
  EXPECT_FALSE(refEmpty == x);
}

TEST(OptionalRef, OutputOperator) {
  int x = 42;
  OptionalRef<int> ref(x);
  std::ostringstream oss;
  oss << ref;
  EXPECT_EQ(oss.str(), "42");

  OptionalRef<int> refEmpty;
  std::ostringstream ossEmpty;
  ossEmpty << refEmpty;
  EXPECT_EQ(ossEmpty.str(), "nullopt");
}

TEST(OptionalRef, FunctionCall) {
  int x = 42;
  OptionalRef<int> ref(x);
  EXPECT_TRUE(isSet(ref));
  EXPECT_FALSE(isSet(std::nullopt));
}

}  // namespace donner
