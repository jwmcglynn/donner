#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/components/animation/AnimateMotionComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/parser/SVGParser.h"

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

TEST(SVGAnimateMotionElement, PacedPathUsesArcLength) {
  auto document = ParseMotion(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animateMotion path="M0 0L100 0L100 100" dur="2s" fill="freeze"/>
    </rect></svg>)svg");
  EXPECT_THAT(MotionAt(document, 0.5), testing::Optional(testing::StrEq("translate(50, 0)")));
  EXPECT_THAT(MotionAt(document, 1.5), testing::Optional(testing::StrEq("translate(100, 50)")));
  EXPECT_THAT(MotionAt(document, 3), testing::Optional(testing::StrEq("translate(100, 100)")));
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
  EXPECT_EQ(href(), "#primary");
  reference->setAttribute("href", "");
  EXPECT_THAT(MotionAt(document, 1), testing::Eq(std::nullopt));
  EXPECT_THAT(href(), testing::IsEmpty());
  reference->removeAttribute("href");
  EXPECT_THAT(MotionAt(document, 1), testing::Optional(testing::StrEq("translate(50, 50)")));
  EXPECT_EQ(href(), "#replacement");
  reference->setAttribute("href", "#primary");
  EXPECT_THAT(MotionAt(document, 1), testing::Optional(testing::StrEq("translate(0, 50)")));
  EXPECT_EQ(href(), "#primary");
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

TEST(SVGAnimateMotionElement, MotionSupplementsTheUnderlyingTransform) {
  auto document = ParseMotion(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r" transform="translate(100)">
      <animateMotion from="0 0" to="100 0" dur="2s"/>
    </rect></svg>)svg");
  EXPECT_THAT(MotionAt(document, 0.5),
              testing::Optional(testing::StrEq("translate(100) translate(25, 0)")));
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
