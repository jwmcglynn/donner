#include "donner/base/SmallVector.h"

#include <gtest/gtest.h>

namespace donner {

namespace {

/**
 * A non-trivial type for testing.
 */
struct NonTrivialType {
  NonTrivialType() { ++constructCount; }
  NonTrivialType(const NonTrivialType&) { ++constructCount; }
  NonTrivialType(NonTrivialType&&) noexcept { ++constructCount; }
  ~NonTrivialType() { ++destructCount; }

  NonTrivialType& operator=(const NonTrivialType&) = default;
  NonTrivialType& operator=(NonTrivialType&&) noexcept = default;

  static int constructCount;  // NOLINT
  static int destructCount;   // NOLINT
};

int NonTrivialType::constructCount = 0;  // NOLINT
int NonTrivialType::destructCount = 0;   // NOLINT

}  // namespace

/**
 * @brief Tests default construction of SmallVector.
 *
 * Validates that a default constructed SmallVector is empty, has a size of 0, and a capacity equal
 * to its template parameter.
 */
TEST(SmallVector, DefaultConstruction) {
  SmallVector<int, 4> vec;
  EXPECT_TRUE(vec.empty());
  EXPECT_EQ(0, vec.size());
  EXPECT_EQ(4, vec.capacity());
}

/**
 * Validates that a SmallVector constructed with an initializer list contains the correct elements,
 * size, and capacity.
 */
TEST(SmallVector, InitializerListConstruction) {
  SmallVector<int, 4> vec = {1, 2, 3, 4};
  EXPECT_FALSE(vec.empty());
  EXPECT_EQ(4, vec.size());
  EXPECT_EQ(4, vec.capacity());
  EXPECT_EQ(1, vec[0]);
  EXPECT_EQ(2, vec[1]);
  EXPECT_EQ(3, vec[2]);
  EXPECT_EQ(4, vec[3]);
}

/**
 * Validates that a SmallVector can exceed its default size and correctly manages its capacity and
 * elements.
 */
TEST(SmallVector, ExceedsDefaultSize) {
  SmallVector<int, 4> vec = {1, 2, 3, 4, 5};
  EXPECT_FALSE(vec.empty());
  EXPECT_EQ(5, vec.size());
  EXPECT_LE(4, vec.capacity());  // Capacity must be at least 4
  EXPECT_EQ(1, vec[0]);
  EXPECT_EQ(5, vec[4]);
}

/**
 * Validates that a copy-constructed SmallVector contains the same elements, size, and capacity as
 * the original.
 */
TEST(SmallVector, CopyConstruction) {
  SmallVector<int, 4> original = {1, 2, 3, 4};
  SmallVector<int, 4> copy = original;
  EXPECT_EQ(original.size(), copy.size());
  for (size_t i = 0; i < original.size(); ++i) {
    EXPECT_EQ(original[i], copy[i]);
  }
}

/**
 * Validates that a move-constructed SmallVector correctly transfers elements from the source,
 * leaving the source empty.
 */
TEST(SmallVector, MoveConstruction) {
  SmallVector<int, 4> original = {1, 2, 3, 4};
  SmallVector<int, 4> moved = std::move(original);
  EXPECT_TRUE(original.empty());
  EXPECT_EQ(4, moved.size());
  EXPECT_EQ(1, moved[0]);
  EXPECT_EQ(4, moved[3]);
}

/**
 * Validates that elements can be added to and removed from the SmallVector, and that the size is
 * updated accordingly.
 */
TEST(SmallVector, PushBackAndPopBack) {
  SmallVector<int, 4> vec;
  vec.push_back(1);
  vec.push_back(2);
  EXPECT_EQ(2, vec.size());
  EXPECT_EQ(1, vec[0]);
  EXPECT_EQ(2, vec[1]);

  vec.pop_back();
  EXPECT_EQ(1, vec.size());
  EXPECT_EQ(1, vec[0]);

  vec.clear();
  EXPECT_TRUE(vec.empty());
}

/**
 * Validates that a SmallVector can handle elements of non-trivial types, such as std::string.
 */
TEST(SmallVector, NonTrivialType) {
  SmallVector<std::string, 4> vec;
  vec.push_back("hello");
  vec.push_back("world");
  EXPECT_EQ(2, vec.size());
  EXPECT_EQ("hello", vec[0]);
  EXPECT_EQ("world", vec[1]);

  vec.pop_back();
  EXPECT_EQ(1, vec.size());
  EXPECT_EQ("hello", vec[0]);

  vec.clear();
  EXPECT_TRUE(vec.empty());
}

/**
 * Validates that a SmallVector can be correctly copy assigned from another vector.
 */
TEST(SmallVector, CopyAssignment) {
  SmallVector<int, 4> original = {1, 2, 3, 4};
  SmallVector<int, 4> copy;
  copy = original;
  EXPECT_EQ(original.size(), copy.size());
  for (size_t i = 0; i < original.size(); ++i) {
    EXPECT_EQ(original[i], copy[i]);
  }
}

/**
 * Validates that a SmallVector can be correctly move assigned from another vector.
 */
TEST(SmallVector, MoveAssignment) {
  SmallVector<int, 4> original = {1, 2, 3, 4};
  SmallVector<int, 4> moved;
  moved = std::move(original);
  EXPECT_TRUE(original.empty());
  EXPECT_EQ(4, moved.size());
  EXPECT_EQ(1, moved[0]);
  EXPECT_EQ(4, moved[3]);
}

/**
 * Validates that a SmallVector correctly resizes and preserves elements when capacity is increased.
 */
TEST(SmallVector, EnsureCapacity) {
  SmallVector<int, 2> vec = {1, 2};
  vec.push_back(3);
  vec.push_back(4);
  vec.push_back(5);  // Should trigger reallocation

  EXPECT_EQ(5, vec.size());
  EXPECT_EQ(1, vec[0]);
  EXPECT_EQ(2, vec[1]);
  EXPECT_EQ(3, vec[2]);
  EXPECT_EQ(4, vec[3]);
  EXPECT_EQ(5, vec[4]);
}

/**
 * @brief Tests the destructor for non-trivial types.
 *
 * Validates that the destructor is correctly called for non-trivial types, such as std::string.
 */
TEST(SmallVector, DestructorNonTrivialType) {
  {
    SmallVector<NonTrivialType, 4> vec;
    vec.push_back(NonTrivialType());
    vec.push_back(NonTrivialType());
  }

  EXPECT_EQ(NonTrivialType::constructCount, NonTrivialType::destructCount);
}

/**
 * @brief Tests for SmallVector with non-trivial types.
 *
 * Validates that a SmallVector can handle elements of non-trivial types, such as std::string.
 */
TEST(SmallVector, NonTrivialTypeUsage) {
  SmallVector<std::string, 4> vec;
  vec.push_back("hello");
  vec.push_back("world");
  EXPECT_EQ(2, vec.size());
  EXPECT_EQ("hello", vec[0]);
  EXPECT_EQ("world", vec[1]);

  vec.pop_back();
  EXPECT_EQ(1, vec.size());
  EXPECT_EQ("hello", vec[0]);

  vec.clear();
  EXPECT_TRUE(vec.empty());
}

/**
 * @brief Tests push_back with move semantics.
 *
 * Validates that a SmallVector can push back elements using move semantics.
 */
TEST(SmallVector, PushBackMove) {
  SmallVector<std::string, 4> vec;
  std::string hello = "hello";
  vec.push_back(std::move(hello));  // NOLINT
  EXPECT_EQ(1, vec.size());
  EXPECT_EQ("hello", vec[0]);
  EXPECT_FALSE(hello.empty());
}

/**
 * @brief Tests pop_back on an empty vector.
 *
 * Validates that calling pop_back on an empty vector does not cause errors.
 */
TEST(SmallVector, PopBackEmpty) {
  SmallVector<int, 4> vec;
  vec.pop_back();  // Should not cause an error
  EXPECT_TRUE(vec.empty());
}

/**
 * @brief Tests clear on a vector with elements.
 *
 * Validates that clear correctly removes all elements and resets size.
 */
TEST(SmallVector, ClearWithElements) {
  SmallVector<int, 4> vec = {1, 2, 3, 4};
  vec.clear();
  EXPECT_TRUE(vec.empty());
  EXPECT_EQ(0, vec.size());
}

/**
 * @brief Tests the capacity method.
 *
 * Validates that the capacity method returns the correct value.
 */
TEST(SmallVector, CapacityMethod) {
  SmallVector<int, 4> vec;
  EXPECT_EQ(4, vec.capacity());
  vec.push_back(1);
  EXPECT_EQ(4, vec.capacity());
  vec.push_back(2);
  vec.push_back(3);
  vec.push_back(4);
  vec.push_back(5);  // Should trigger reallocation
  EXPECT_GT(vec.capacity(), 4);
}

/**
 * @brief Tests the begin and end methods.
 *
 * Validates that the begin and end methods return correct iterators.
 */
TEST(SmallVector, BeginEndMethods) {
  SmallVector<int, 4> vec = {1, 2, 3, 4};
  EXPECT_EQ(vec.begin(), &vec[0]);
  EXPECT_EQ(vec.end(), &vec[0] + vec.size());
}

}  // namespace donner
