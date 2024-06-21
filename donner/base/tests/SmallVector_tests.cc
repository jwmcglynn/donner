#include <gtest/gtest.h>
#include "donner/base/SmallVector.h"

using namespace donner;

TEST(SmallVector, DefaultConstruction) {
    SmallVector<int, 4> vec;
    EXPECT_TRUE(vec.empty());
    EXPECT_EQ(0, vec.size());
    EXPECT_EQ(4, vec.capacity());
}

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

TEST(SmallVector, ExceedsDefaultSize) {
    SmallVector<int, 4> vec = {1, 2, 3, 4, 5};
    EXPECT_FALSE(vec.empty());
    EXPECT_EQ(5, vec.size());
    EXPECT_LE(4, vec.capacity()); // Capacity must be at least 4
    EXPECT_EQ(1, vec[0]);
    EXPECT_EQ(5, vec[4]);
}

TEST(SmallVector, CopyConstruction) {
    SmallVector<int, 4> original = {1, 2, 3, 4};
    SmallVector<int, 4> copy = original;
    EXPECT_EQ(original.size(), copy.size());
    for (size_t i = 0; i < original.size(); ++i) {
        EXPECT_EQ(original[i], copy[i]);
    }
}

TEST(SmallVector, MoveConstruction) {
    SmallVector<int, 4> original = {1, 2, 3, 4};
    SmallVector<int, 4> moved = std::move(original);
    EXPECT_TRUE(original.empty());
    EXPECT_EQ(4, moved.size());
    EXPECT_EQ(1, moved[0]);
    EXPECT_EQ(4, moved[3]);
}

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
