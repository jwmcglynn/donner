#include "donner/svg/SVGSymbolElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/core/PreserveAspectRatio.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {

TEST(SVGSymbolElementTests, Defaults) {
  auto symbol = instantiateSubtreeElementAs<SVGSymbolElement>("<symbol />");

  EXPECT_THAT(symbol->viewbox(), testing::Eq(std::nullopt));
  EXPECT_THAT(symbol->preserveAspectRatio(),
              testing::Eq(PreserveAspectRatio{PreserveAspectRatio::Align::XMidYMid,
                                              PreserveAspectRatio::MeetOrSlice::Meet}));

  EXPECT_THAT(symbol->x(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(symbol->y(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(symbol->width(), testing::Eq(std::nullopt));
  EXPECT_THAT(symbol->height(), testing::Eq(std::nullopt));

  EXPECT_DOUBLE_EQ(symbol->refX(), 0.0);
  EXPECT_DOUBLE_EQ(symbol->refY(), 0.0);
}

TEST(SVGSymbolElementTests, ViewBoxAndPreserveAspectRatio) {
  auto symbol = instantiateSubtreeElementAs<SVGSymbolElement>(
      R"(<symbol viewBox="0 0 100 50" preserveAspectRatio="xMinYMin slice" />)");

  ASSERT_TRUE(symbol->viewbox());
  const auto box = *symbol->viewbox();
  EXPECT_DOUBLE_EQ(box.topLeft.x, 0.0);
  EXPECT_DOUBLE_EQ(box.topLeft.y, 0.0);
  EXPECT_DOUBLE_EQ(box.width(), 100.0);
  EXPECT_DOUBLE_EQ(box.height(), 50.0);

  EXPECT_THAT(symbol->preserveAspectRatio(),
              testing::Eq(PreserveAspectRatio{PreserveAspectRatio::Align::XMinYMin,
                                              PreserveAspectRatio::MeetOrSlice::Slice}));
}

TEST(SVGSymbolElementTests, PositionAttributes) {
  auto symbol = instantiateSubtreeElementAs<SVGSymbolElement>(R"(<symbol x="5" y="10" />)");

  EXPECT_THAT(symbol->x(), LengthIs(5.0, Lengthd::Unit::None));
  EXPECT_THAT(symbol->y(), LengthIs(10.0, Lengthd::Unit::None));

  symbol->setX(Lengthd(3.0, Lengthd::Unit::None));
  symbol->setY(Lengthd(4.0, Lengthd::Unit::None));

  EXPECT_THAT(symbol->x(), LengthIs(3.0, Lengthd::Unit::None));
  EXPECT_THAT(symbol->y(), LengthIs(4.0, Lengthd::Unit::None));
}

TEST(SVGSymbolElementTests, SizeAttributes) {
  auto symbol =
      instantiateSubtreeElementAs<SVGSymbolElement>(R"(<symbol width="100" height="50" />)");

  ASSERT_TRUE(symbol->width());
  ASSERT_TRUE(symbol->height());
  EXPECT_THAT(*symbol->width(), LengthIs(100.0, Lengthd::Unit::None));
  EXPECT_THAT(*symbol->height(), LengthIs(50.0, Lengthd::Unit::None));

  symbol->setWidth(Lengthd(120.0, Lengthd::Unit::None));
  symbol->setHeight(Lengthd(60.0, Lengthd::Unit::None));

  ASSERT_TRUE(symbol->width());
  ASSERT_TRUE(symbol->height());
  EXPECT_THAT(*symbol->width(), LengthIs(120.0, Lengthd::Unit::None));
  EXPECT_THAT(*symbol->height(), LengthIs(60.0, Lengthd::Unit::None));
}

TEST(SVGSymbolElementTests, ReferencePointAttributes) {
  auto symbol = instantiateSubtreeElementAs<SVGSymbolElement>(R"(<symbol refX="25" refY="30" />)");

  EXPECT_DOUBLE_EQ(symbol->refX(), 25.0);
  EXPECT_DOUBLE_EQ(symbol->refY(), 30.0);

  symbol->setRefX(5.0);
  symbol->setRefY(6.0);

  EXPECT_DOUBLE_EQ(symbol->refX(), 5.0);
  EXPECT_DOUBLE_EQ(symbol->refY(), 6.0);
}

// TODO: Additional tests (e.g., rendering, href resolution) can be added once the
// renderer supports <symbol> instantiation.

}  // namespace donner::svg
