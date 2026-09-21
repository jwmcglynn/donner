#include <gtest/gtest.h>

#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGSetElement.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/animation/AnimationStateComponent.h"
#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/components/animation/AnimationTimingComponent.h"
#include "donner/svg/components/animation/SetAnimationComponent.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererUtils.h"

namespace donner::svg {

namespace {

SVGDocument parseSVGWithExperimental(std::string_view svg) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  ParseWarningSink warnings;
  auto result = parser::SVGParser::ParseSVG(svg, warnings, options);
  EXPECT_THAT(result, NoParseError()) << "Failed to parse SVG";
  return std::move(result.result());
}

}  // namespace

namespace {

Entity EntityById(SVGDocument& document, std::string_view id) {
  const std::optional<SVGElement> element = document.querySelector(std::string(id));
  EXPECT_TRUE(element.has_value()) << "missing " << id;
  return element.has_value() ? element->entityHandle().entity() : entt::null;
}

}  // namespace

TEST(SVGSetElement, IsAnimationTargetResolvesParentAndHref) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="layer">
        <rect id="r" width="10" height="10" />
        <set attributeName="fill" to="blue" />
      </g>
      <g id="hashTarget" />
      <g id="rawTarget" />
      <g id="untouched" />
      <set attributeName="fill" to="blue" href="#hashTarget" />
      <set attributeName="fill" to="blue" href="rawTarget" />
    </svg>
  )");

  auto& registry = document.registry();
  const components::AnimationSystem system;

  EXPECT_TRUE(system.isAnimationTarget(registry, EntityById(document, "#layer")))
      << "an animation with no href targets its parent";
  EXPECT_TRUE(system.isAnimationTarget(registry, EntityById(document, "#hashTarget")));
  EXPECT_TRUE(system.isAnimationTarget(registry, EntityById(document, "#rawTarget")))
      << "href without a leading '#' names the same element";
  EXPECT_FALSE(system.isAnimationTarget(registry, EntityById(document, "#untouched")));
  EXPECT_FALSE(system.isAnimationTarget(registry, EntityById(document, "#r")));
  EXPECT_FALSE(system.isAnimationTarget(registry, entt::null));
}

TEST(SVGSetElement, IsAnimationTargetMatchesBeforeAndAfterAdvance) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="layer">
        <rect id="r" width="10" height="10" />
        <set attributeName="fill" to="blue" />
      </g>
    </svg>
  )");

  auto& registry = document.registry();
  const components::AnimationSystem system;
  const Entity layer = EntityById(document, "#layer");

  EXPECT_TRUE(system.isAnimationTarget(registry, layer)) << "before any advance resolved the state";

  components::AnimationSystem().advance(registry, 0.0);
  EXPECT_TRUE(system.isAnimationTarget(registry, layer)) << "after the state was resolved";
}

TEST(SVGSetElement, ParseBasic) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" fill="red" width="100" height="100">
        <set attributeName="fill" to="blue" begin="2s" dur="3s" />
      </rect>
    </svg>
  )");

  // Find the set element via the registry
  auto& registry = document.registry();
  auto view = registry.view<components::SetAnimationComponent>();
  ASSERT_FALSE(view.begin() == view.end());

  auto entity = *view.begin();
  auto& setComp = registry.get<components::SetAnimationComponent>(entity);
  EXPECT_EQ(setComp.attributeName, "fill");
  EXPECT_EQ(setComp.to, "blue");

  auto& timing = registry.get<components::AnimationTimingComponent>(entity);
  ASSERT_TRUE(timing.beginOffset.has_value());
  EXPECT_DOUBLE_EQ(timing.beginOffset->seconds(), 2.0);
  ASSERT_TRUE(timing.dur.has_value());
  EXPECT_DOUBLE_EQ(timing.dur->seconds(), 3.0);
}

TEST(SVGSetElement, ParseFillFreeze) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect fill="red" width="100" height="100">
        <set attributeName="fill" to="blue" begin="0s" dur="1s" fill="freeze" />
      </rect>
    </svg>
  )");

  auto& registry = document.registry();
  auto view = registry.view<components::AnimationTimingComponent>();
  ASSERT_FALSE(view.begin() == view.end());

  auto entity = *view.begin();
  auto& timing = registry.get<components::AnimationTimingComponent>(entity);
  EXPECT_EQ(timing.fill, components::AnimationFill::Freeze);
}

TEST(SVGSetElement, AnimationBeforeBeginTime) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" fill="red" width="100" height="100">
        <set attributeName="fill" to="blue" begin="2s" dur="3s" />
      </rect>
    </svg>
  )");

  auto& registry = document.registry();

  // Advance to t=1s (before begin)
  components::AnimationSystem().advance(registry, 1.0, nullptr);

  // The rect should NOT have animated overrides
  auto rect = document.querySelector("#r");
  ASSERT_TRUE(rect.has_value());
  EXPECT_FALSE(
      registry.try_get<components::AnimatedValuesComponent>(rect->entityHandle().entity()));
}

TEST(SVGSetElement, AnimationDuringActiveInterval) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" fill="red" width="100" height="100">
        <set attributeName="fill" to="blue" begin="2s" dur="3s" />
      </rect>
    </svg>
  )");

  auto& registry = document.registry();

  // Advance to t=3s (during active interval: 2s <= 3s < 5s)
  components::AnimationSystem().advance(registry, 3.0, nullptr);

  // The rect should have animated override for fill
  auto rect = document.querySelector("#r");
  ASSERT_TRUE(rect.has_value());
  auto* animValues =
      registry.try_get<components::AnimatedValuesComponent>(rect->entityHandle().entity());
  ASSERT_NE(animValues, nullptr);
  ASSERT_TRUE(animValues->overrides.count("fill"));
  EXPECT_EQ(animValues->overrides.at("fill"), "blue");
}

TEST(SVGSetElement, AnimationAfterEndRemove) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" fill="red" width="100" height="100">
        <set attributeName="fill" to="blue" begin="2s" dur="3s" />
      </rect>
    </svg>
  )");

  auto& registry = document.registry();

  // Advance to t=6s (after end: 2s + 3s = 5s)
  components::AnimationSystem().advance(registry, 6.0, nullptr);

  // Default fill="remove", so no override should be present
  auto rect = document.querySelector("#r");
  ASSERT_TRUE(rect.has_value());
  EXPECT_FALSE(
      registry.try_get<components::AnimatedValuesComponent>(rect->entityHandle().entity()));
}

TEST(SVGSetElement, AnimationAfterEndFreeze) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" fill="red" width="100" height="100">
        <set attributeName="fill" to="blue" begin="2s" dur="3s" fill="freeze" />
      </rect>
    </svg>
  )");

  auto& registry = document.registry();

  // Advance to t=6s (after end, but fill="freeze")
  components::AnimationSystem().advance(registry, 6.0, nullptr);

  // fill="freeze" persists the value
  auto rect = document.querySelector("#r");
  ASSERT_TRUE(rect.has_value());
  auto* animValues =
      registry.try_get<components::AnimatedValuesComponent>(rect->entityHandle().entity());
  ASSERT_NE(animValues, nullptr);
  ASSERT_TRUE(animValues->overrides.count("fill"));
  EXPECT_EQ(animValues->overrides.at("fill"), "blue");
}

TEST(SVGSetElement, IndefiniteDuration) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" fill="red" width="100" height="100">
        <set attributeName="fill" to="blue" begin="0s" />
      </rect>
    </svg>
  )");

  auto& registry = document.registry();

  // With no dur, <set> has indefinite duration, so it's always active after begin
  components::AnimationSystem().advance(registry, 100.0, nullptr);

  auto rect = document.querySelector("#r");
  ASSERT_TRUE(rect.has_value());
  auto* animValues =
      registry.try_get<components::AnimatedValuesComponent>(rect->entityHandle().entity());
  ASSERT_NE(animValues, nullptr);
  EXPECT_EQ(animValues->overrides.at("fill"), "blue");
}

TEST(SVGSetElement, DocumentSetTime) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" fill="red" width="100" height="100">
        <set attributeName="fill" to="blue" begin="1s" dur="2s" />
      </rect>
    </svg>
  )");

  // Test the public setTime() API
  EXPECT_DOUBLE_EQ(document.currentTime(), 0.0);

  document.setTime(1.5);
  EXPECT_DOUBLE_EQ(document.currentTime(), 1.5);
}

TEST(SVGSetElement, MissingAttributeNameProducesNoOverride) {
  // <set> without attributeName cannot target an attribute, so no override is produced even
  // while the animation is active.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" fill="red" width="100" height="100">
        <set to="blue" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  auto& registry = document.registry();
  components::AnimationSystem().advance(registry, 1.0, nullptr);

  auto rect = document.querySelector("#r");
  ASSERT_TRUE(rect.has_value());
  EXPECT_EQ(registry.try_get<components::AnimatedValuesComponent>(rect->entityHandle().entity()),
            nullptr);
}

TEST(SVGSetElement, OversizedSetValueConsumesSourceBudgetBeforeCopyingOutput) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r"><set attributeName="fill" to="blue" begin="0s" dur="2s" /></rect>
    </svg>
  )");
  auto& registry = document.registry();
  auto view = registry.view<components::SetAnimationComponent>();
  ASSERT_NE(view.begin(), view.end());
  registry.get<components::SetAnimationComponent>(*view.begin())
      .to.assign(components::AnimationResourceBudget::kMaximumSourceBytes + 1, 'x');

  components::AnimationSystem().advance(registry, 1.0, nullptr);

  const auto* budget = registry.ctx().find<components::AnimationResourceBudget>();
  ASSERT_NE(budget, nullptr);
  EXPECT_TRUE(budget->rejected());
  EXPECT_EQ(registry.try_get<components::AnimatedValuesComponent>(
                document.querySelector("#r")->entityHandle().entity()),
            nullptr);
}

}  // namespace donner::svg
