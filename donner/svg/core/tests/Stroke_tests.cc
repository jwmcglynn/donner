/**
 * @file Tests for Stroke helpers, such as \ref donner::svg::StrokeLinecap, \ref
 * donner::svg::StrokeLinejoin, and \ref donner::svg::StrokeDasharray.
 */

#include "donner/svg/core/Stroke.h"

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::svg {

/// @test Ostream output \c operator<< for all \ref StrokeLinecap values.
TEST(StrokeLinecapTest, OstreamOutput) {
  EXPECT_THAT(StrokeLinecap::Butt, ToStringIs("butt"));
  EXPECT_THAT(StrokeLinecap::Round, ToStringIs("round"));
  EXPECT_THAT(StrokeLinecap::Square, ToStringIs("square"));
}

/// @test Ostream output \c operator<< for all \ref StrokeLinejoin values.
TEST(StrokeLinejoinTest, OstreamOutput) {
  EXPECT_THAT(StrokeLinejoin::Miter, ToStringIs("miter"));
  EXPECT_THAT(StrokeLinejoin::MiterClip, ToStringIs("miter-clip"));
  EXPECT_THAT(StrokeLinejoin::Round, ToStringIs("round"));
  EXPECT_THAT(StrokeLinejoin::Bevel, ToStringIs("bevel"));
  EXPECT_THAT(StrokeLinejoin::Arcs, ToStringIs("arcs"));
}

/// @test Default construction yields an empty StrokeDasharray.
TEST(StrokeDasharrayTest, DefaultConstructor) {
  StrokeDasharray dash;
  EXPECT_TRUE(dash.empty());
  EXPECT_EQ(dash.size(), 0);
}

/// @test Construction from an initializer list initializes elements correctly.
TEST(StrokeDasharrayTest, InitializerListConstructor) {
  StrokeDasharray dash{Lengthd(5.0, Lengthd::Unit::Px), Lengthd(10.0, Lengthd::Unit::Px)};
  EXPECT_EQ(dash.size(), 2);
  EXPECT_EQ(dash[0], Lengthd(5.0, Lengthd::Unit::Px));
  EXPECT_EQ(dash[1], Lengthd(10.0, Lengthd::Unit::Px));
}

/// @test Accessor method \c at returns the correct element and throws on out-of-range.
TEST(StrokeDasharrayTest, AtMethod) {
  StrokeDasharray dash{Lengthd(1.0, Lengthd::Unit::Px), Lengthd(2.0, Lengthd::Unit::Px)};
  EXPECT_EQ(dash.at(0), Lengthd(1.0, Lengthd::Unit::Px));
  EXPECT_EQ(dash.at(1), Lengthd(2.0, Lengthd::Unit::Px));
  EXPECT_DEATH(std::ignore = dash.at(2), "");
}

/// @test \c front and \c back return the first and last elements, respectively.
TEST(StrokeDasharrayTest, FrontAndBack) {
  StrokeDasharray dash;
  dash.push_back(Lengthd(1.0, Lengthd::Unit::Px));
  dash.push_back(Lengthd(2.0, Lengthd::Unit::Px));
  dash.push_back(Lengthd(3.0, Lengthd::Unit::Px));
  EXPECT_EQ(dash.front(), Lengthd(1.0, Lengthd::Unit::Px));
  EXPECT_EQ(dash.back(), Lengthd(3.0, Lengthd::Unit::Px));
}

/// @test \c data returns a pointer to the underlying array.
TEST(StrokeDasharrayTest, DataMethod) {
  StrokeDasharray dash{Lengthd(4.0, Lengthd::Unit::Px), Lengthd(8.0, Lengthd::Unit::Px)};
  const Lengthd* dataPtr = dash.data();
  ASSERT_NE(dataPtr, nullptr);
  EXPECT_EQ(dataPtr[0], Lengthd(4.0, Lengthd::Unit::Px));
  EXPECT_EQ(dataPtr[1], Lengthd(8.0, Lengthd::Unit::Px));
}

/// @test \c capacity and \c reserve work as expected.
TEST(StrokeDasharrayTest, CapacityAndReserve) {
  StrokeDasharray dash;
  dash.reserve(20);
  EXPECT_GE(dash.capacity(), 20);
  EXPECT_EQ(dash.size(), 0);

  dash.push_back(Lengthd(1.0, Lengthd::Unit::Px));
  dash.push_back(Lengthd(2.0, Lengthd::Unit::Px));
  EXPECT_GE(dash.capacity(), dash.size());
}

/// @test \c push_back and \c emplace_back correctly add elements.
TEST(StrokeDasharrayTest, PushBackAndEmplaceBack) {
  StrokeDasharray dash;
  dash.push_back(Lengthd(1.0, Lengthd::Unit::Px));
  dash.emplace_back(Lengthd(2.0, Lengthd::Unit::Px));
  EXPECT_EQ(dash.size(), 2);
  EXPECT_EQ(dash[0], Lengthd(1.0, Lengthd::Unit::Px));
  EXPECT_EQ(dash[1], Lengthd(2.0, Lengthd::Unit::Px));
}

/// @test \c insert places an element at the specified position.
TEST(StrokeDasharrayTest, InsertMethod) {
  StrokeDasharray dash{Lengthd(1.0, Lengthd::Unit::Px), Lengthd(3.0, Lengthd::Unit::Px)};
  // Insert an element with value 2.0 Px at position 1.
  auto it = dash.insert(dash.begin() + 1, Lengthd(2.0, Lengthd::Unit::Px));
  EXPECT_EQ(*it, Lengthd(2.0, Lengthd::Unit::Px));
  EXPECT_EQ(dash.size(), 3);
  EXPECT_EQ(dash[0], Lengthd(1.0, Lengthd::Unit::Px));
  EXPECT_EQ(dash[1], Lengthd(2.0, Lengthd::Unit::Px));
  EXPECT_EQ(dash[2], Lengthd(3.0, Lengthd::Unit::Px));
}

/// @test \c erase removes the specified element.
TEST(StrokeDasharrayTest, EraseMethod) {
  StrokeDasharray dash{Lengthd(1.0, Lengthd::Unit::Px), Lengthd(2.0, Lengthd::Unit::Px),
                       Lengthd(3.0, Lengthd::Unit::Px)};
  // Erase the middle element.
  auto it = dash.erase(dash.begin() + 1);
  EXPECT_EQ(*it, Lengthd(3.0, Lengthd::Unit::Px));
  EXPECT_EQ(dash.size(), 2);
  EXPECT_EQ(dash[0], Lengthd(1.0, Lengthd::Unit::Px));
  EXPECT_EQ(dash[1], Lengthd(3.0, Lengthd::Unit::Px));
}

/// @test \c clear removes all elements.
TEST(StrokeDasharrayTest, ClearMethod) {
  StrokeDasharray dash{Lengthd(1.0, Lengthd::Unit::Px), Lengthd(2.0, Lengthd::Unit::Px)};
  EXPECT_FALSE(dash.empty());
  dash.clear();
  EXPECT_TRUE(dash.empty());
  EXPECT_EQ(dash.size(), 0);
}

/// @test \c resize modifies the size of the StrokeDasharray and correctly fills new elements.
TEST(StrokeDasharrayTest, ResizeMethod) {
  StrokeDasharray dash;
  // Resize to a larger size with a default fill value.
  dash.resize(3, Lengthd(0.0, Lengthd::Unit::Px));
  EXPECT_EQ(dash.size(), 3);
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(dash[i], Lengthd(0.0, Lengthd::Unit::Px));
  }
  // Modify one element.
  dash[1] = Lengthd(5.0, Lengthd::Unit::Px);
  // Resize to a smaller size.
  dash.resize(2);
  EXPECT_EQ(dash.size(), 2);
  EXPECT_EQ(dash[0], Lengthd(0.0, Lengthd::Unit::Px));
  EXPECT_EQ(dash[1], Lengthd(5.0, Lengthd::Unit::Px));
}

/// @test Iterator methods: using begin, end, cbegin, and cend.
TEST(StrokeDasharrayTest, IteratorMethods) {
  StrokeDasharray dash;
  dash.push_back(Lengthd(1.0, Lengthd::Unit::Px));
  dash.push_back(Lengthd(2.0, Lengthd::Unit::Px));
  dash.push_back(Lengthd(3.0, Lengthd::Unit::Px));

  // Test using non-const iterator.
  auto it = dash.begin();
  EXPECT_EQ(*it, Lengthd(1.0, Lengthd::Unit::Px));
  ++it;
  EXPECT_EQ(*it, Lengthd(2.0, Lengthd::Unit::Px));
  ++it;
  EXPECT_EQ(*it, Lengthd(3.0, Lengthd::Unit::Px));

  // Test using const iterator.
  const StrokeDasharray& constDash = dash;
  auto cit = constDash.cbegin();
  EXPECT_EQ(*cit, Lengthd(1.0, Lengthd::Unit::Px));
  ++cit;
  EXPECT_EQ(*cit, Lengthd(2.0, Lengthd::Unit::Px));
  ++cit;
  EXPECT_EQ(*cit, Lengthd(3.0, Lengthd::Unit::Px));
}

/// @test Ostream output \c operator<< for \ref StrokeDasharray.
TEST(StrokeDasharrayTest, OstreamOutput) {
  EXPECT_THAT(StrokeDasharray(), ToStringIs(""));
  EXPECT_THAT((StrokeDasharray{Lengthd(5.0, Lengthd::Unit::Px)}), ToStringIs("5px"));
  EXPECT_THAT((StrokeDasharray{Lengthd(5.0, Lengthd::Unit::Px), Lengthd(10.0, Lengthd::Unit::Px)}),
              ToStringIs("5px,10px"));
  EXPECT_THAT(
      (StrokeDasharray{Lengthd(5.0, Lengthd::Unit::Em), Lengthd(10.0, Lengthd::Unit::Percent)}),
      ToStringIs("5em,10%"));
}

}  // namespace donner::svg
