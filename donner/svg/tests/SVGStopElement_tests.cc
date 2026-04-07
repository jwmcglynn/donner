#include "donner/svg/SVGStopElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/SVGLinearGradientElement.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::DoubleNear;
using testing::Eq;
using testing::FloatNear;
using testing::Ne;

namespace donner::svg {
namespace {

TEST(SVGStopElementTests, Defaults) {
  auto stop = instantiateSubtreeElementAs<SVGStopElement>("<stop />");
  EXPECT_THAT(stop->offset(), FloatNear(0.0f, 0.001f));
  EXPECT_EQ(stop->stopColor(), css::Color(css::RGBA(0, 0, 0, 0xFF)));
  EXPECT_THAT(stop->stopOpacity(), DoubleNear(1.0, 0.001));
}

TEST(SVGStopElementTests, ParseFromXml) {
  auto doc = instantiateSubtree(R"(
    <svg>
      <linearGradient>
        <stop offset="0.5" stop-color="red" stop-opacity="0.8" />
      </linearGradient>
    </svg>
  )");
  auto maybeStop = doc.querySelector("stop");
  ASSERT_THAT(maybeStop, Ne(std::nullopt));
  auto stop = maybeStop->cast<SVGStopElement>();

  EXPECT_THAT(stop.offset(), FloatNear(0.5f, 0.001f));
  EXPECT_EQ(stop.stopColor(), css::Color(css::RGBA(0xFF, 0, 0, 0xFF)));
  EXPECT_THAT(stop.stopOpacity(), DoubleNear(0.8, 0.001));
}

TEST(SVGStopElementTests, SetOffsetProgrammatically) {
  auto stop = instantiateSubtreeElementAs<SVGStopElement>("<stop />");
  stop->setOffset(0.25f);
  EXPECT_THAT(stop->offset(), FloatNear(0.25f, 0.001f));

  stop->setOffset(0.0f);
  EXPECT_THAT(stop->offset(), FloatNear(0.0f, 0.001f));

  stop->setOffset(1.0f);
  EXPECT_THAT(stop->offset(), FloatNear(1.0f, 0.001f));
}

TEST(SVGStopElementTests, SetStopColorProgrammatically) {
  auto stop = instantiateSubtreeElementAs<SVGStopElement>("<stop />");
  stop->setStopColor(css::Color(css::RGBA(0, 0xFF, 0, 0xFF)));
  EXPECT_EQ(stop->stopColor(), css::Color(css::RGBA(0, 0xFF, 0, 0xFF)));
}

TEST(SVGStopElementTests, SetStopOpacityProgrammatically) {
  auto stop = instantiateSubtreeElementAs<SVGStopElement>("<stop />");
  stop->setStopOpacity(0.3);
  EXPECT_THAT(stop->stopOpacity(), DoubleNear(0.3, 0.001));

  stop->setStopOpacity(0.0);
  EXPECT_THAT(stop->stopOpacity(), DoubleNear(0.0, 0.001));

  stop->setStopOpacity(1.0);
  EXPECT_THAT(stop->stopOpacity(), DoubleNear(1.0, 0.001));
}

TEST(SVGStopElementTests, OffsetClampingFromParsing) {
  // Values > 1 should clamp to 1 during parsing.
  {
    auto doc = instantiateSubtree(R"(
      <svg>
        <linearGradient>
          <stop offset="2.0" stop-color="red" />
        </linearGradient>
      </svg>
    )");
    auto stop = doc.querySelector("stop")->cast<SVGStopElement>();
    EXPECT_THAT(stop.offset(), FloatNear(1.0f, 0.001f));
  }

  // Values < 0 should clamp to 0 during parsing.
  {
    auto doc = instantiateSubtree(R"(
      <svg>
        <linearGradient>
          <stop offset="-1.0" stop-color="blue" />
        </linearGradient>
      </svg>
    )");
    auto stop = doc.querySelector("stop")->cast<SVGStopElement>();
    EXPECT_THAT(stop.offset(), FloatNear(0.0f, 0.001f));
  }
}

TEST(SVGStopElementTests, OffsetPercentage) {
  auto doc = instantiateSubtree(R"(
    <svg>
      <linearGradient>
        <stop offset="75%" stop-color="blue" />
      </linearGradient>
    </svg>
  )");
  auto stop = doc.querySelector("stop")->cast<SVGStopElement>();
  EXPECT_THAT(stop.offset(), FloatNear(0.75f, 0.001f));
}

TEST(SVGStopElementTests, StopColorNamedColors) {
  // Named color: blue.
  {
    auto stop = instantiateSubtreeElementAs<SVGStopElement>(R"(<stop stop-color="blue" />)");
    EXPECT_EQ(stop->stopColor(), css::Color(css::RGBA(0, 0, 0xFF, 0xFF)));
  }

  // Named color: green (CSS green is #008000).
  {
    auto stop = instantiateSubtreeElementAs<SVGStopElement>(R"(<stop stop-color="green" />)");
    EXPECT_EQ(stop->stopColor(), css::Color(css::RGBA(0, 0x80, 0, 0xFF)));
  }

  // Named color: white.
  {
    auto stop = instantiateSubtreeElementAs<SVGStopElement>(R"(<stop stop-color="white" />)");
    EXPECT_EQ(stop->stopColor(), css::Color(css::RGBA(0xFF, 0xFF, 0xFF, 0xFF)));
  }
}

TEST(SVGStopElementTests, StopColorHexFormat) {
  // 6-digit hex.
  {
    auto stop = instantiateSubtreeElementAs<SVGStopElement>(R"(<stop stop-color="#FF8000" />)");
    EXPECT_EQ(stop->stopColor(), css::Color(css::RGBA(0xFF, 0x80, 0x00, 0xFF)));
  }

  // 3-digit hex.
  {
    auto stop = instantiateSubtreeElementAs<SVGStopElement>(R"(<stop stop-color="#F00" />)");
    EXPECT_EQ(stop->stopColor(), css::Color(css::RGBA(0xFF, 0, 0, 0xFF)));
  }
}

TEST(SVGStopElementTests, StopColorRgbFunction) {
  auto stop = instantiateSubtreeElementAs<SVGStopElement>(
      R"-(<stop stop-color="rgb(128, 64, 32)" />)-");
  EXPECT_EQ(stop->stopColor(), css::Color(css::RGBA(128, 64, 32, 0xFF)));
}

TEST(SVGStopElementTests, ComputedStopColor) {
  auto doc = instantiateSubtree(R"(
    <svg>
      <linearGradient>
        <stop offset="0.5" stop-color="red" />
      </linearGradient>
    </svg>
  )");
  auto stop = doc.querySelector("stop")->cast<SVGStopElement>();
  EXPECT_EQ(stop.computedStopColor(), css::Color(css::RGBA(0xFF, 0, 0, 0xFF)));
}

TEST(SVGStopElementTests, ComputedStopOpacity) {
  auto doc = instantiateSubtree(R"(
    <svg>
      <linearGradient>
        <stop offset="0" stop-opacity="0.4" />
      </linearGradient>
    </svg>
  )");
  auto stop = doc.querySelector("stop")->cast<SVGStopElement>();
  EXPECT_THAT(stop.computedStopOpacity(), DoubleNear(0.4, 0.001));
}

TEST(SVGStopElementTests, ComputedStopColorDefault) {
  auto stop = instantiateSubtreeElementAs<SVGStopElement>("<stop />");
  // Default computed stop-color should be black.
  EXPECT_EQ(stop->computedStopColor(), css::Color(css::RGBA(0, 0, 0, 0xFF)));
}

TEST(SVGStopElementTests, ComputedStopOpacityDefault) {
  auto stop = instantiateSubtreeElementAs<SVGStopElement>("<stop />");
  // Default computed stop-opacity should be 1.0.
  EXPECT_THAT(stop->computedStopOpacity(), DoubleNear(1.0, 0.001));
}

TEST(SVGStopElementTests, ComputedStopColorAfterSet) {
  auto stop = instantiateSubtreeElementAs<SVGStopElement>("<stop />");
  stop->setStopColor(css::Color(css::RGBA(0, 0xFF, 0xFF, 0xFF)));
  EXPECT_EQ(stop->computedStopColor(), css::Color(css::RGBA(0, 0xFF, 0xFF, 0xFF)));
}

TEST(SVGStopElementTests, ComputedStopOpacityAfterSet) {
  auto stop = instantiateSubtreeElementAs<SVGStopElement>("<stop />");
  stop->setStopOpacity(0.6);
  EXPECT_THAT(stop->computedStopOpacity(), DoubleNear(0.6, 0.001));
}

TEST(SVGStopElementTests, Cast) {
  auto stop = instantiateSubtreeElementAs<SVGStopElement>("<stop />");
  EXPECT_THAT(stop->tryCast<SVGElement>(), Ne(std::nullopt));
  EXPECT_THAT(stop->tryCast<SVGStopElement>(), Ne(std::nullopt));
  // Ensure that an unrelated type does not match.
  EXPECT_THAT(stop->tryCast<SVGLinearGradientElement>(), Eq(std::nullopt));
}

TEST(SVGStopElementTests, ComputedStopColorWithCssStylesheet) {
  auto doc = instantiateSubtree(R"(
    <svg>
      <style>stop { stop-color: lime; }</style>
      <linearGradient>
        <stop offset="0" />
      </linearGradient>
    </svg>
  )");
  auto stop = doc.querySelector("stop")->cast<SVGStopElement>();
  // CSS "lime" is #00FF00.
  EXPECT_EQ(stop.computedStopColor(), css::Color(css::RGBA(0, 0xFF, 0, 0xFF)));
}

TEST(SVGStopElementTests, ComputedStopOpacityWithCssStylesheet) {
  auto doc = instantiateSubtree(R"(
    <svg>
      <style>stop { stop-opacity: 0.25; }</style>
      <linearGradient>
        <stop offset="0" />
      </linearGradient>
    </svg>
  )");
  auto stop = doc.querySelector("stop")->cast<SVGStopElement>();
  EXPECT_THAT(stop.computedStopOpacity(), DoubleNear(0.25, 0.001));
}

}  // namespace
}  // namespace donner::svg
