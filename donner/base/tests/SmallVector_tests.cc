#include "donner/base/SmallVector.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace donner {

using testing::ElementsAre;

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

/**
 * A non-trivial type for testing emplace_back.
 */
struct EmplaceableType {
  int value;
  std::string text;

  EmplaceableType(int v, const std::string& t) : value(v), text(t) { ++constructCount; }

  EmplaceableType(const EmplaceableType&) = default;
  EmplaceableType(EmplaceableType&&) = default;
  EmplaceableType& operator=(const EmplaceableType&) = default;
  EmplaceableType& operator=(EmplaceableType&&) = default;

  ~EmplaceableType() { ++destructCount; }

  bool operator==(const EmplaceableType& other) const {
    return value == other.value && text == other.text;
  }

  static int constructCount;
  static int destructCount;

  static void resetCounts() {
    constructCount = 0;
    destructCount = 0;
  }
};

int EmplaceableType::constructCount = 0;
int EmplaceableType::destructCount = 0;

/**
 * A simple type with operator<< for testing stream output.
 */
struct StreamableType {
  int value;

  explicit StreamableType(int v) : value(v) {}

  bool operator==(const StreamableType& other) const { return value == other.value; }

  friend std::ostream& operator<<(std::ostream& os, const StreamableType& obj) {
    return os << "StreamableType(" << obj.value << ")";
  }
};

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
 * Tests emplace_back functionality with various types.
 */
TEST(SmallVector, EmplaceBack) {
  // Test with int
  {
    SmallVector<int, 4> vec;
    vec.emplace_back(42);
    EXPECT_EQ(1, vec.size());
    EXPECT_EQ(42, vec[0]);
  }

  // Test with std::string
  {
    SmallVector<std::string, 4> vec;
    vec.emplace_back("hello");
    vec.emplace_back(5, 'a');  // Creates "aaaaa"
    EXPECT_EQ(2, vec.size());
    EXPECT_EQ("hello", vec[0]);
    EXPECT_EQ("aaaaa", vec[1]);
  }

  // Test with custom type
  {
    EmplaceableType::resetCounts();
    SmallVector<EmplaceableType, 4> vec;
    vec.emplace_back(10, "test");
    vec.emplace_back(20, "example");

    EXPECT_EQ(2, vec.size());
    EXPECT_EQ(10, vec[0].value);
    EXPECT_EQ("test", vec[0].text);
    EXPECT_EQ(20, vec[1].value);
    EXPECT_EQ("example", vec[1].text);
    EXPECT_EQ(2, EmplaceableType::constructCount);
  }

  // Test emplace_back with reallocation
  {
    SmallVector<std::string, 2> vec;
    vec.emplace_back("first");
    vec.emplace_back("second");
    vec.emplace_back("third");  // Should trigger reallocation

    EXPECT_EQ(3, vec.size());
    EXPECT_GT(vec.capacity(), 2);
    EXPECT_EQ("first", vec[0]);
    EXPECT_EQ("second", vec[1]);
    EXPECT_EQ("third", vec[2]);
  }

  // Test return value of emplace_back
  {
    SmallVector<std::string, 4> vec;
    auto& ref = vec.emplace_back("test");
    EXPECT_EQ("test", ref);
    ref = "modified";
    EXPECT_EQ("modified", vec[0]);
  }
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

/**
 * Validates that the gtest ElementsAre matcher works for this class.
 */
TEST(SmallVector, ElementsAreMatcher) {
  SmallVector<int, 4> vec = {1, 2, 3, 4};
  EXPECT_THAT(vec, ElementsAre(1, 2, 3, 4));

  vec.push_back(5);
  EXPECT_THAT(vec, ElementsAre(1, 2, 3, 4, 5));

  vec.pop_back();
  EXPECT_THAT(vec, ElementsAre(1, 2, 3, 4));
}

/**
 * Validates that the gtest ElementsAre matcher works for non-trivial types.
 */
TEST(SmallVector, ElementsAreMatcherNonTrivialType) {
  SmallVector<std::string, 4> vec;
  vec.push_back("hello");
  vec.push_back("world");
  EXPECT_THAT(vec, ElementsAre("hello", "world"));

  vec.pop_back();
  EXPECT_THAT(vec, ElementsAre("hello"));

  vec.clear();
  EXPECT_THAT(vec, ElementsAre());
}

/**
 * Tests the insert method of SmallVector.
 */
TEST(SmallVector, Insert) {
  // Test inserting at the beginning
  {
    SmallVector<int, 5> vec;
    vec.push_back(2);
    vec.push_back(3);

    auto it = vec.insert(vec.begin(), 1);
    EXPECT_EQ(*it, 1);
    EXPECT_EQ(vec.size(), 3);
    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[1], 2);
    EXPECT_EQ(vec[2], 3);
  }

  // Test inserting in the middle
  {
    SmallVector<int, 5> vec;
    vec.push_back(1);
    vec.push_back(3);

    auto it = vec.insert(vec.begin() + 1, 2);
    EXPECT_EQ(*it, 2);
    EXPECT_EQ(vec.size(), 3);
    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[1], 2);
    EXPECT_EQ(vec[2], 3);
  }

  // Test inserting at the end
  {
    SmallVector<int, 5> vec;
    vec.push_back(1);
    vec.push_back(2);

    auto it = vec.insert(vec.end(), 3);
    EXPECT_EQ(*it, 3);
    EXPECT_EQ(vec.size(), 3);
    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[1], 2);
    EXPECT_EQ(vec[2], 3);
  }

  // Test inserting in an empty vector
  {
    SmallVector<int, 5> vec;

    auto it = vec.insert(vec.begin(), 1);
    EXPECT_EQ(*it, 1);
    EXPECT_EQ(vec.size(), 1);
    EXPECT_EQ(vec[0], 1);
  }

  // Test inserting with invalid position (beyond end)
  {
    SmallVector<int, 5> vec;
    vec.push_back(1);

    // Should insert at the end
    auto it = vec.insert(vec.begin() + 5, 2);
    EXPECT_EQ(*it, 2);
    EXPECT_EQ(vec.size(), 2);
    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[1], 2);
  }

  // Test inserting with data growth beyond small size
  {
    SmallVector<int, 4> vec;
    for (int i = 0; i < 4; ++i) {
      vec.push_back(i + 1);
    }

    // This will trigger reallocation
    auto it = vec.insert(vec.begin(), 0);
    EXPECT_EQ(*it, 0);
    EXPECT_EQ(vec.size(), 5);

    for (int i = 0; i < 5; ++i) {
      EXPECT_EQ(vec[i], i);
    }
  }

  // Test with non-trivial type (std::string)
  {
    SmallVector<std::string, 3> vec;
    vec.push_back("apple");
    vec.push_back("cherry");

    auto it = vec.insert(vec.begin() + 1, "banana");
    EXPECT_EQ(*it, "banana");
    EXPECT_EQ(vec.size(), 3);
    EXPECT_EQ(vec[0], "apple");
    EXPECT_EQ(vec[1], "banana");
    EXPECT_EQ(vec[2], "cherry");
  }
}

/**
 * Tests operator<< for SmallVector with int elements.
 */
TEST(SmallVectorStream, OutputOperatorInt) {
  // Test empty vector
  {
    SmallVector<int, 4> vec;
    std::ostringstream oss;
    oss << vec;
    EXPECT_EQ(oss.str(), "[]");
  }

  // Test single element
  {
    SmallVector<int, 4> vec = {42};
    std::ostringstream oss;
    oss << vec;
    EXPECT_EQ(oss.str(), "[42]");
  }

  // Test multiple elements
  {
    SmallVector<int, 4> vec = {1, 2, 3, 4};
    std::ostringstream oss;
    oss << vec;
    EXPECT_EQ(oss.str(), "[1, 2, 3, 4]");
  }

  // Test vector that exceeds small size
  {
    SmallVector<int, 2> vec = {1, 2, 3, 4, 5};
    std::ostringstream oss;
    oss << vec;
    EXPECT_EQ(oss.str(), "[1, 2, 3, 4, 5]");
  }
}

/**
 * Tests operator<< for SmallVector with string elements.
 */
TEST(SmallVectorStream, OutputOperatorString) {
  // Test empty vector
  {
    SmallVector<std::string, 4> vec;
    std::ostringstream oss;
    oss << vec;
    EXPECT_EQ(oss.str(), "[]");
  }

  // Test single element
  {
    SmallVector<std::string, 4> vec = {"hello"};
    std::ostringstream oss;
    oss << vec;
    EXPECT_EQ(oss.str(), "[hello]");
  }

  // Test multiple elements
  {
    SmallVector<std::string, 4> vec = {"hello", "world", "test"};
    std::ostringstream oss;
    oss << vec;
    EXPECT_EQ(oss.str(), "[hello, world, test]");
  }

  // Test with empty strings
  {
    SmallVector<std::string, 4> vec = {"", "middle", ""};
    std::ostringstream oss;
    oss << vec;
    EXPECT_EQ(oss.str(), "[, middle, ]");
  }
}

/**
 * Tests operator<< for SmallVector with custom streamable type.
 */
TEST(SmallVectorStream, OutputOperatorCustomType) {
  // Test empty vector
  {
    SmallVector<StreamableType, 4> vec;
    std::ostringstream oss;
    oss << vec;
    EXPECT_EQ(oss.str(), "[]");
  }

  // Test single element
  {
    SmallVector<StreamableType, 4> vec;
    vec.emplace_back(42);
    std::ostringstream oss;
    oss << vec;
    EXPECT_EQ(oss.str(), "[StreamableType(42)]");
  }

  // Test multiple elements
  {
    SmallVector<StreamableType, 4> vec;
    vec.emplace_back(1);
    vec.emplace_back(2);
    vec.emplace_back(3);
    std::ostringstream oss;
    oss << vec;
    EXPECT_EQ(oss.str(), "[StreamableType(1), StreamableType(2), StreamableType(3)]");
  }
}

/**
 * Tests the front() method for accessing the first element.
 */
TEST(SmallVector, Front) {
  // Test with int
  {
    SmallVector<int, 4> vec = {1, 2, 3, 4};
    EXPECT_EQ(vec.front(), 1);
    vec.front() = 10;
    EXPECT_EQ(vec.front(), 10);
    EXPECT_EQ(vec[0], 10);
  }

  // Test with single element
  {
    SmallVector<int, 4> vec = {42};
    EXPECT_EQ(vec.front(), 42);
  }

  // Test with string (non-trivial type)
  {
    SmallVector<std::string, 4> vec = {"hello", "world", "test"};
    EXPECT_EQ(vec.front(), "hello");
    vec.front() = "modified";
    EXPECT_EQ(vec.front(), "modified");
    EXPECT_EQ(vec[0], "modified");
  }

  // Test with vector that exceeds small size
  {
    SmallVector<int, 2> vec = {1, 2, 3, 4, 5};
    EXPECT_EQ(vec.front(), 1);
  }
}

/**
 * Tests the front() const method for accessing the first element.
 */
TEST(SmallVector, FrontConst) {
  const SmallVector<int, 4> vec = {1, 2, 3, 4};
  EXPECT_EQ(vec.front(), 1);

  const SmallVector<std::string, 4> strVec = {"hello", "world"};
  EXPECT_EQ(strVec.front(), "hello");
}

/**
 * Tests the back() method for accessing the last element.
 */
TEST(SmallVector, Back) {
  // Test with int
  {
    SmallVector<int, 4> vec = {1, 2, 3, 4};
    EXPECT_EQ(vec.back(), 4);
    vec.back() = 40;
    EXPECT_EQ(vec.back(), 40);
    EXPECT_EQ(vec[3], 40);
  }

  // Test with single element
  {
    SmallVector<int, 4> vec = {42};
    EXPECT_EQ(vec.back(), 42);
    EXPECT_EQ(vec.front(), vec.back());
  }

  // Test with string (non-trivial type)
  {
    SmallVector<std::string, 4> vec = {"hello", "world", "test"};
    EXPECT_EQ(vec.back(), "test");
    vec.back() = "modified";
    EXPECT_EQ(vec.back(), "modified");
    EXPECT_EQ(vec[2], "modified");
  }

  // Test with vector that exceeds small size
  {
    SmallVector<int, 2> vec = {1, 2, 3, 4, 5};
    EXPECT_EQ(vec.back(), 5);
  }

  // Test back() after push_back and pop_back
  {
    SmallVector<int, 4> vec = {1, 2, 3};
    EXPECT_EQ(vec.back(), 3);
    vec.push_back(4);
    EXPECT_EQ(vec.back(), 4);
    vec.pop_back();
    EXPECT_EQ(vec.back(), 3);
  }
}

/**
 * Tests the back() const method for accessing the last element.
 */
TEST(SmallVector, BackConst) {
  const SmallVector<int, 4> vec = {1, 2, 3, 4};
  EXPECT_EQ(vec.back(), 4);

  const SmallVector<std::string, 4> strVec = {"hello", "world"};
  EXPECT_EQ(strVec.back(), "world");
}

/**
 * Tests front() and back() together on the same vector.
 */
TEST(SmallVector, FrontAndBack) {
  SmallVector<int, 4> vec = {1, 2, 3, 4, 5};
  EXPECT_EQ(vec.front(), 1);
  EXPECT_EQ(vec.back(), 5);

  vec.front() = 10;
  vec.back() = 50;
  EXPECT_EQ(vec[0], 10);
  EXPECT_EQ(vec[4], 50);
  EXPECT_THAT(vec, ElementsAre(10, 2, 3, 4, 50));
}

}  // namespace donner
