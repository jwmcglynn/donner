#include "donner/svg/SVGSymbolElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/core/PreserveAspectRatio.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {

TEST(SVGSymbolElementTests, Defaults) {
  auto symbol = instantiateSubtreeElementAs<SVGSymbolElement>("<symbol />");

  EXPECT_THAT(symbol->viewBox(), testing::Eq(std::nullopt));
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

  ASSERT_TRUE(symbol->viewBox());
  const auto box = *symbol->viewBox();
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

/**
 * @test when refX/refY are at their defaults (0,0), the symbol's origin should align exactly with
 * the <use> position (8,8); the square therefore spans x,y in [8,11].
 */
TEST(SVGSymbolElementRenderingTests, DefaultReferencePoint) {
  // clang-format off
  SVGDocument document = instantiateSubtree(R"-(
    <svg viewBox="0 0 16 16">
      <defs>
        <symbol id="square" viewBox="0 0 4 4"
                width="4" height="4"
                refX="0" refY="0">
          <rect width="4" height="4" fill="white"/>
        </symbol>
      </defs>
      <use href="#square" x="8" y="8" width="4" height="4"/>
    </svg>
  )-");
  // clang-format on

  const AsciiImage ascii = RendererTestUtils::renderToAsciiImage(document);
  EXPECT_TRUE(ascii.matches(R"(
    ................
    ................
    ................
    ................
    ................
    ................
    ................
    ................
    ........@@@@....
    ........@@@@....
    ........@@@@....
    ........@@@@....
    ................
    ................
    ................
    ................
  )"));
}

/**
 * @test With refX/refY = (2,2) the symbol's internal point (2,2) is aligned with (8,8); the square
 * therefore starts two units up/left, spanning [6,9].
 */
TEST(SVGSymbolElementRenderingTests, CustomReferencePoint) {
  SVGDocument document = instantiateSubtree(R"-(
    <svg viewBox="0 0 16 16">
      <defs>
        <symbol id="square" viewBox="0 0 6 6"
                width="6" height="6"
                refX="3" refY="3" style="overflow: visible">
          <rect width="6" height="6" fill="white"/>
        </symbol>
      </defs>
      <use href="#square" x="8" y="8" width="6" height="6"/>
    </svg>
  )-");

  const AsciiImage ascii = RendererTestUtils::renderToAsciiImage(document);
  EXPECT_TRUE(ascii.matches(R"(
    ................
    ................
    ................
    ................
    ................
    .....@@@@@@.....
    .....@@@@@@.....
    .....@@@@@@.....
    .....@@@@@@.....
    .....@@@@@@.....
    .....@@@@@@.....
    ................
    ................
    ................
    ................
    ................
  )"));
}

}  // namespace donner::svg
