#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <utility>

#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/components/animation/AnimateMotionComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/parser/TransformParser.h"

namespace donner::svg {
namespace {

SVGDocument ParseMotion(std::string_view source) {
  parser::SVGParser::Options options;
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(source, warnings, options);
  EXPECT_THAT(parsed, NoParseError());
  return std::move(parsed.result());
}

std::optional<std::string> MotionAt(SVGDocument& document, double time,
                                    std::string_view selector = "#r") {
  components::AnimationSystem().advance(document.registry(), time);
  const auto target = document.querySelector(selector);
  if (!target) {
    return std::nullopt;
  }
  const auto* values = document.registry().try_get<components::AnimatedValuesComponent>(
      target->entityHandle().entity());
  if (!values) {
    return std::nullopt;
  }
  const auto it = values->overrides.find("transform");
  return it != values->overrides.end() ? std::optional(it->second) : std::nullopt;
}

void ExpectMotionTransformNear(SVGDocument& document, double time, Vector2d expectedTranslation,
                               Vector2d expectedDirection) {
  const auto value = MotionAt(document, time);
  ASSERT_THAT(value, testing::Ne(std::nullopt));
  const auto parsed = parser::TransformParser::Parse(*value);
  ASSERT_THAT(parsed, NoParseError());
  const Vector2d translation = parsed.result().transformPosition(Vector2d::Zero());
  const Vector2d direction = parsed.result().transformVector({1, 0});
  EXPECT_NEAR(translation.x, expectedTranslation.x, 1e-6);
  EXPECT_NEAR(translation.y, expectedTranslation.y, 1e-6);
  EXPECT_NEAR(direction.x, expectedDirection.x, 1e-6);
  EXPECT_NEAR(direction.y, expectedDirection.y, 1e-6);
}

TEST(SVGAnimateMotionElement, PacedPathUsesArcLength) {
  auto document = ParseMotion(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animateMotion path="M0 0L100 0L100 100" dur="2s" fill="freeze"/>
    </rect></svg>)svg");
  EXPECT_THAT(MotionAt(document, 0.5), testing::Optional(testing::StrEq("translate(50, 0)")));
  EXPECT_THAT(MotionAt(document, 1.5), testing::Optional(testing::StrEq("translate(100, 50)")));
  EXPECT_THAT(MotionAt(document, 3), testing::Optional(testing::StrEq("translate(100, 100)")));
}

TEST(SVGAnimateMotionElement, AutoReverseAndNumericRotationUseDistinctBranches) {
  for (const auto& [rotate, expected] : {std::pair{"auto-reverse", "translate(50, 0) rotate(180)"},
                                         std::pair{"30", "translate(50, 0) rotate(30)"}}) {
    SCOPED_TRACE(rotate);
    auto document = ParseMotion(
        "<svg xmlns='http://www.w3.org/2000/svg'><rect id='r'>"
        "<animateMotion path='M0 0H100' rotate='" +
        std::string(rotate) + "' dur='2s'/></rect></svg>");
    EXPECT_THAT(MotionAt(document, 1), testing::Optional(testing::StrEq(expected)));
  }
}

TEST(SVGAnimateMotionElement, MpathOverridesPathAndTracksGeometryChanges) {
  auto document = ParseMotion(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><path id="route" d="M0 0L0 100"/>
      <rect id="r"><animateMotion path="M0 0L100 0" dur="2s" rotate="auto">
        <mpath href="#route"/>
      </animateMotion></rect></svg>)svg");
  EXPECT_THAT(MotionAt(document, 1),
              testing::Optional(testing::StrEq("translate(0, 50) rotate(90)")));
  auto route = document.querySelector("#route");
  ASSERT_THAT(route, testing::Ne(std::nullopt));
  route->setAttribute("d", "M0 0L0 200");
  EXPECT_THAT(MotionAt(document, 1),
              testing::Optional(testing::StrEq("translate(0, 100) rotate(90)")));
}

TEST(SVGAnimateMotionElement, MpathHrefPrecedesXlinkHrefIncludingEmptyAndMutationFallback) {
  auto document = ParseMotion(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink">
      <path id="primary" d="M0 0V100"/><path id="legacy" d="M0 0H100"/>
      <path id="replacement" d="M0 0L100 100"/>
      <rect id="r"><animateMotion dur="2s"><mpath id="reference"
        href="#primary" xlink:href="#legacy"/></animateMotion></rect>
    </svg>)svg");
  auto reference = document.querySelector("#reference");
  ASSERT_THAT(reference, testing::Ne(std::nullopt));
  const auto href = [&]() {
    return reference->entityHandle().get<components::MPathComponent>().href;
  };

  EXPECT_EQ(href(), "#primary");
  EXPECT_THAT(MotionAt(document, 1), testing::Optional(testing::StrEq("translate(0, 50)")));
  reference->setAttribute({"xlink", "href"}, "#replacement");
  EXPECT_THAT(MotionAt(document, 1), testing::Optional(testing::StrEq("translate(0, 50)")));
  reference->setAttribute("href", "");
  EXPECT_THAT(MotionAt(document, 1), testing::Eq(std::nullopt));
  reference->removeAttribute("href");
  EXPECT_THAT(MotionAt(document, 1), testing::Optional(testing::StrEq("translate(50, 50)")));
  reference->setAttribute("href", "#primary");
  EXPECT_THAT(MotionAt(document, 1), testing::Optional(testing::StrEq("translate(0, 50)")));
}

TEST(SVGAnimateMotionElement, AnimationHrefPrecedesXlinkHref) {
  auto document = ParseMotion(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink">
      <rect id="primary"/><rect id="legacy"/>
      <animateMotion href="#primary" xlink:href="#legacy"
                     from="0 0" to="100 0" dur="2s"/>
    </svg>)svg");
  EXPECT_THAT(MotionAt(document, 1, "#primary"),
              testing::Optional(testing::StrEq("translate(50, 0)")));
  EXPECT_THAT(MotionAt(document, 1, "#legacy"), testing::Eq(std::nullopt));
}

TEST(SVGAnimateMotionElement, KeyPointsAndTimesControlPathProgress) {
  auto document = ParseMotion(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animateMotion path="M0 0L100 0" calcMode="linear" keyPoints="0;0.8;1"
        keyTimes="0;0.5;1" dur="2s"/>
    </rect></svg>)svg");
  EXPECT_THAT(MotionAt(document, 0.5), testing::Optional(testing::StrEq("translate(40, 0)")));
  EXPECT_THAT(MotionAt(document, 1.5), testing::Optional(testing::StrEq("translate(90, 0)")));
}

TEST(SVGAnimateMotionElement, LinearPathUsesEqualSegmentTimes) {
  auto document = ParseMotion(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animateMotion path="M0 0L20 0L100 0" calcMode="linear" dur="2s"/>
    </rect></svg>)svg");
  EXPECT_THAT(MotionAt(document, 1), testing::Optional(testing::StrEq("translate(20, 0)")));
}

TEST(SVGAnimateMotionElement, ArcExpansionDoesNotCreateMotionTimingStops) {
  for (const auto mode : {"linear", "discrete"}) {
    SCOPED_TRACE(mode);
    auto document = ParseMotion(
        "<svg xmlns='http://www.w3.org/2000/svg'><rect id='r'>"
        "<animateMotion path='M10 0 A10 10 0 0 1 -10 0 L-20 0' calcMode='" +
        std::string(mode) + "' dur='2s' fill='freeze'/></rect></svg>");
    const auto expectTranslation = [&](double time, Vector2d expected) {
      const auto value = MotionAt(document, time);
      ASSERT_THAT(value, testing::Ne(std::nullopt));
      const auto parsed = parser::TransformParser::Parse(*value);
      ASSERT_THAT(parsed, NoParseError());
      const Vector2d actual = parsed.result().transformPosition(Vector2d::Zero());
      EXPECT_NEAR(actual.x, expected.x, 1e-6);
      EXPECT_NEAR(actual.y, expected.y, 1e-6);
    };
    if (mode == std::string_view("discrete")) {
      expectTranslation(0.5, {10, 0});
      expectTranslation(0.75, {-10, 0});
      expectTranslation(1.5, {-20, 0});
    }
    expectTranslation(1, {-10, 0});
  }
}

TEST(SVGAnimateMotionElement, MotionSupplementsTheUnderlyingTransform) {
  auto document = ParseMotion(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r" transform="translate(100)">
      <animateMotion from="0 0" to="100 0" dur="2s"/>
    </rect></svg>)svg");
  EXPECT_THAT(MotionAt(document, 0.5),
              testing::Optional(testing::StrEq("translate(100) translate(25, 0)")));
}

TEST(SVGAnimateMotionElement, ByOnlyAddsLowerMotionUnlessAnotherPathSourceWins) {
  struct Case {
    const char* attributes;
    const char* children;
    bool adds;
  };
  const Case cases[] = {
      {"by='0 100'", "", true},
      {"by='0 100' additive='replace'", "", true},
      {"by='0 100' values='0 0;0 100'", "", false},
      {"by='0 100' path='M0 0V100'", "", false},
      {"by='0 100'", "<mpath href='#route'/>", false},
  };
  for (const auto& item : cases) {
    SCOPED_TRACE(item.attributes);
    SCOPED_TRACE(item.children);
    auto document = ParseMotion(
        "<svg xmlns='http://www.w3.org/2000/svg'><path id='route' d='M0 0V100'/>"
        "<rect id='r' transform='translate(100)'><animateMotion path='M0 0H100' dur='2s'/>"
        "<animateMotion dur='2s' " +
        std::string(item.attributes) + ">" + item.children + "</animateMotion></rect></svg>");
    const char* expected = item.adds ? "translate(100) translate(50, 0) translate(0, 50)"
                                     : "translate(100) translate(0, 50)";
    EXPECT_THAT(MotionAt(document, 1), testing::Optional(testing::StrEq(expected)));
  }
}

TEST(SVGAnimateMotionElement, InvalidMpathDoesNotFallBackToPath) {
  auto document = ParseMotion(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animateMotion path="M0 0L100 0" dur="2s"><mpath href="#missing"/></animateMotion>
    </rect></svg>)svg");
  EXPECT_THAT(MotionAt(document, 1), testing::Eq(std::nullopt));
}

TEST(SVGAnimateMotionElement, ByAndAccumulationPreserveMotionAcrossRepeats) {
  auto document = ParseMotion(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animateMotion by="100 50" dur="2s" repeatCount="3" accumulate="sum"/>
    </rect></svg>)svg");
  EXPECT_THAT(MotionAt(document, 3), testing::Optional(testing::StrEq("translate(150, 75)")));
}

TEST(SVGAnimateMotionElement, AccumulationUsesTheFinalKeyPoint) {
  auto document = ParseMotion(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animateMotion path="M0 0L100 0" calcMode="linear" keyPoints="0;0.5"
        keyTimes="0;1" dur="1s" repeatCount="2" accumulate="sum"/>
    </rect></svg>)svg");
  EXPECT_THAT(MotionAt(document, 1.5), testing::Optional(testing::StrEq("translate(75, 0)")));
}

TEST(SVGAnimateMotionElement, AutoRotationUsesDegenerateCurveEndpointDirection) {
  auto document = ParseMotion(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animateMotion path="M0 0C0 0 0 100 0 100" rotate="auto" dur="1s" fill="freeze"/>
    </rect></svg>)svg");
  ExpectMotionTransformNear(document, 1, {0, 100}, {0, 1});
}

TEST(SVGAnimateMotionElement, AutoRotationUsesExactTerminalControlDirection) {
  auto document = ParseMotion(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animateMotion path="M0 0C0 100 100 100 100 100" rotate="auto"
        dur="1s" fill="freeze"/>
    </rect></svg>)svg");
  ExpectMotionTransformNear(document, 1, {100, 100}, {1, 0});
}

TEST(SVGAnimateMotionElement, AutoRotationUsesOutgoingDirectionAtInternalJoin) {
  for (const auto& [rotate, direction] :
       {std::pair{"auto", Vector2d(1, 0)}, std::pair{"auto-reverse", Vector2d(-1, 0)}}) {
    SCOPED_TRACE(rotate);
    auto document = ParseMotion(
        "<svg xmlns='http://www.w3.org/2000/svg'><rect id='r'>"
        "<animateMotion path='M0 0C0 0 0 100 0 100L100 100' rotate='" +
        std::string(rotate) + "' dur='2s'/></rect></svg>");
    ExpectMotionTransformNear(document, 1, {0, 100}, direction);
  }
}

TEST(SVGAnimateMotionElement, AutoRotationPreservesTinyNonzeroInteriorDirection) {
  auto document = ParseMotion(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animateMotion path="M0 0Q0 1e-13 1e-13 1e-13" rotate="auto" dur="1s"/>
    </rect></svg>)svg");
  constexpr double kSqrtHalf = 0.7071067811865476;
  ExpectMotionTransformNear(document, 0.5, {2.5e-14, 7.5e-14}, {kSqrtHalf, kSqrtHalf});
}

TEST(SVGAnimateMotionElement, OversizedReferencedPathIsRejectedBeforeCopying) {
  const std::string source =
      "<svg xmlns='http://www.w3.org/2000/svg'><path id='route' d='M0 0" +
      std::string(1024 * 1024, ' ') +
      "L100 0'/><rect id='r'>"
      "<animateMotion dur='2s'><mpath href='#route'/></animateMotion></rect></svg>";
  auto document = ParseMotion(source);
  EXPECT_THAT(MotionAt(document, 1), testing::Eq(std::nullopt));
}

TEST(SVGAnimateMotionElement, RemovedMpathReferenceDoesNotKeepOldRoute) {
  auto document = ParseMotion(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><path id="route" d="M0 0L100 0"/>
      <rect id="r"><animateMotion dur="2s"><mpath id="ref" href="#route"/></animateMotion></rect>
    </svg>)svg");
  EXPECT_THAT(MotionAt(document, 1), testing::Ne(std::nullopt));
  auto reference = document.querySelector("#ref");
  ASSERT_THAT(reference, testing::Ne(std::nullopt));
  reference->removeAttribute("href");
  EXPECT_THAT(MotionAt(document, 1), testing::Eq(std::nullopt));
}

TEST(SVGAnimateMotionElement, ForeignNamespaceMpathDoesNotOverrideInlinePath) {
  auto document = ParseMotion(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><path id="route" d="M0 0L0 100"/>
      <rect id="r"><animateMotion path="M0 0L100 0" dur="2s">
        <mpath id="m" href="#route"/>
      </animateMotion></rect></svg>)svg");
  EXPECT_THAT(MotionAt(document, 1), testing::Optional(testing::StrEq("translate(0, 50)")));
  document.querySelector("#m")->setAttribute("xmlns", "urn:foreign");
  EXPECT_THAT(MotionAt(document, 1), testing::Optional(testing::StrEq("translate(50, 0)")));
  document.querySelector("#m")->removeAttribute("xmlns");
  document.querySelector("#route")->setAttribute("xmlns", "urn:foreign");
  EXPECT_THAT(MotionAt(document, 1), testing::Eq(std::nullopt));
}

TEST(SVGAnimateMotionElement, MpathTracksTypedPathEdits) {
  auto document = ParseMotion(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><path id="route" d="M0 0L0 100"/>
      <rect id="r"><animateMotion dur="2s"><mpath href="#route"/></animateMotion></rect>
    </svg>)svg");
  auto route = document.querySelector("#route")->cast<SVGPathElement>();
  route.setD("M0 0L0 200");
  EXPECT_THAT(MotionAt(document, 1), testing::Optional(testing::StrEq("translate(0, 100)")));
  route.setSpline(PathBuilder().moveTo({0, 0}).lineTo({200, 0}).build());
  EXPECT_THAT(MotionAt(document, 1), testing::Optional(testing::StrEq("translate(100, 0)")));
}

TEST(SVGAnimateMotionElement, SharedMotionWorkExhaustionRejectsTheWholeSample) {
  std::string route = "M0 0";
  for (int i = 0; i < 4000; ++i) {
    route += "l1 0";
  }
  std::string source = "<svg xmlns='http://www.w3.org/2000/svg'><rect id='r'>";
  for (int count = 1; count <= 20; ++count) {
    source += "<animateMotion path='" + route + "' dur='2s'/>";
    auto document = ParseMotion(source + "</rect></svg>");
    const auto sampled = MotionAt(document, 1);
    if (document.registry().ctx().get<components::AnimationResourceBudget>().rejected()) {
      EXPECT_THAT(sampled, testing::Eq(std::nullopt))
          << "first rejected prefix has " << count << " animations";
      return;
    }
    ASSERT_THAT(sampled, testing::Ne(std::nullopt));
  }
  FAIL() << "The growing motion workload never reached its shared work limit";
}

}  // namespace
}  // namespace donner::svg
