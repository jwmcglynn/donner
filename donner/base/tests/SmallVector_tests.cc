#include <gtest/gtest.h>
#include "donner/base/SmallVector.h"

using namespace donner;

/**
 * @brief Tests default construction of SmallVector.
 * 
 * Validates that a default constructed SmallVector is empty, has a size of 0, and a capacity equal to its template parameter.
 */
TEST(SmallVector, DefaultConstruction) {
    SmallVector<int, 4> vec;
    EXPECT_TRUE(vec.empty());
    EXPECT_EQ(0, vec.size());
    EXPECT_EQ(4, vec.capacity());
}

/**
 * Validates that a SmallVector constructed with an initializer list contains the correct elements, size, and capacity.
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
 * Validates that a SmallVector can exceed its default size and correctly manages its capacity and elements.
 */
TEST(SmallVector, ExceedsDefaultSize) {
    SmallVector<int, 4> vec = {1, 2, 3, 4, 5};
    EXPECT_FALSE(vec.empty());
    EXPECT_EQ(5, vec.size());
    EXPECT_LE(4, vec.capacity()); // Capacity must be at least 4
    EXPECT_EQ(1, vec[0]);
    EXPECT_EQ(5, vec[4]);
}

/**
 * Validates that a copy-constructed SmallVector contains the same elements, size, and capacity as the original.
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
 * Validates that a move-constructed SmallVector correctly transfers elements from the source, leaving the source empty.
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
 * Validates that elements can be added to and removed from the SmallVector, and that the size is updated accordingly.
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
