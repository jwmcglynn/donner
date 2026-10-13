#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/tests/RunfileGate.h"
#include "donner/svg/SVGCircleElement.h"
#include "donner/svg/SVGGElement.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"

namespace donner::svg {
namespace {

SVGDocument LoadAnimationFixture(std::string_view name) {
  const auto file = donner::tests::ReadRequiredRunfile("donner/svg/renderer/testdata/animation/" +
                                                       std::string(name));
  EXPECT_EQ(file.error, "");
  parser::SVGParser::Options options;
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(file.contents, warnings, options);
  EXPECT_THAT(parsed, NoParseError());
  return std::move(parsed.result());
}

TEST(RendererAnimationFixture, SwimRendersSplineMotionAtIndependentCoordinates) {
  auto document = LoadAnimationFixture("world_lock_swim.svg");
  const auto group = document.querySelector("g");
  ASSERT_THAT(group, testing::Ne(std::nullopt));
  for (const auto& [time, x] :
       {std::pair{0.5, 27.5}, std::pair{1.0, 55.0}, std::pair{3.0, -55.0}}) {
    SCOPED_TRACE(time);
    document.setTime(time);
    const auto bitmap = RenderDocumentWithActiveBackend(document);
    EXPECT_NEAR(group->cast<SVGGElement>().transform().data[4], x, 0.001);
    EXPECT_THAT(test::PixelAt(bitmap, static_cast<int>(240 + x), 280),
                test::Rgba(168, 209, 245, 255));
    EXPECT_THAT(test::PixelAt(bitmap, 50, 100), test::Rgba(34, 34, 34, 255));
  }
}

TEST(RendererAnimationFixture, JitterHoldsAndJumpsAtNonuniformKeyTimes) {
  auto document = LoadAnimationFixture("world_lock_jitter.svg");
  const auto group = document.querySelector("g");
  ASSERT_THAT(group, testing::Ne(std::nullopt));
  for (const auto& [time, x, y] :
       {std::tuple{0.04, 0.0, 0.0}, std::tuple{0.08, 0.0, 0.0}, std::tuple{0.8 * 0.11, 5.0, -3.0},
        std::tuple{0.16, 5.0, -3.0}}) {
    SCOPED_TRACE(time);
    document.setTime(time);
    const auto bitmap = RenderDocumentWithActiveBackend(document);
    const auto parentFromGroup = group->cast<SVGGElement>().transform();
    EXPECT_DOUBLE_EQ(parentFromGroup.data[4], x);
    EXPECT_DOUBLE_EQ(parentFromGroup.data[5], y);
    EXPECT_THAT(test::PixelAt(bitmap, 173, 280),
                x == 0 ? test::Rgba(239, 91, 82, 255) : test::Rgba(201, 47, 40, 255));
  }
}

TEST(RendererAnimationFixture, DeCasteljauRendersTheBezierPointAtKnownTimes) {
  auto document = LoadAnimationFixture("de_casteljau.svg");
  const auto point = document.querySelector("circle[fill='#111111']");
  ASSERT_THAT(point, testing::Ne(std::nullopt));
  for (const auto& [time, x, y] : {std::tuple{2.0, 449.125, 164.625}, std::tuple{4.0, 840.0, 56.0},
                                   std::tuple{6.0, 449.125, 164.625}}) {
    SCOPED_TRACE(time);
    document.setTime(time);
    const auto bitmap = RenderDocumentWithActiveBackend(document);
    const auto circle = point->cast<SVGCircleElement>();
    EXPECT_NEAR(circle.computedCx().value, x, 0.01);
    EXPECT_NEAR(circle.computedCy().value, y, 0.01);
    EXPECT_THAT(test::PixelAt(bitmap, static_cast<int>(x), static_cast<int>(y)),
                time == 4 ? test::Rgba(255, 255, 255, 255) : test::Rgba(17, 17, 17, 255));
  }
}

}  // namespace
}  // namespace donner::svg
