#include <gtest/gtest.h>

#include <sstream>

#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGSetElement.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationBaseValue.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/animation/AnimationStateComponent.h"
#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/components/animation/AnimationTimingComponent.h"
#include "donner/svg/components/animation/AnimationValueSampler.h"
#include "donner/svg/components/animation/SetAnimationComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/properties/PropertyRegistry.h"
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

std::optional<std::string> AnimatedOverride(SVGDocument& document, std::string_view id,
                                            std::string_view attribute, double time) {
  components::AnimationSystem().advance(document.registry(), time);
  const auto element = document.querySelector(id);
  if (!element) {
    return std::nullopt;
  }
  const auto* values = document.registry().try_get<components::AnimatedValuesComponent>(
      element->entityHandle().entity());
  if (!values) {
    return std::nullopt;
  }
  const bool supportedRectWidth = element->type() == ElementType::Rect && attribute == "width";
  if (supportedRectWidth && values->contextualLengths.contains(std::string(attribute))) {
    const auto recipe = values->contextualLengths.at(std::string(attribute));
    const auto rect = element->cast<SVGRectElement>();
    auto& budget = document.registry().ctx().get<components::AnimationResourceBudget>();
    auto context = components::AnimationContextFor(
        document.registry(), element->entityHandle().entity(), attribute, budget);
    context.extent = Lengthd::Extent::X;
    if (!components::SampleContextualLengthRecipe(*recipe, rect.width(), context)) {
      return std::nullopt;
    }
    document.setTime(time);
    ParseWarningSink warnings = ParseWarningSink::Disabled();
    RendererUtils::prepareDocumentForRendering(document, false, warnings);
    std::ostringstream output;
    output.precision(12);
    output << rect.computedWidth();
    return output.str();
  }
  const auto value = values->overrides.find(std::string(attribute));
  return value == values->overrides.end() ? std::nullopt : std::optional(value->second);
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

TEST(SVGSetElement, HrefPrecedesXlinkHrefIncludingEmptyAndMutationFallback) {
  auto document = parseSVGWithExperimental(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink">
      <rect id="primary" width="10"/><rect id="legacy" width="20"/>
      <rect id="replacement" width="30"/>
      <set id="animation" href="#primary" xlink:href="#legacy"
           attributeName="width" to="40" dur="2s"/>
    </svg>)svg");
  auto animation = document.querySelector("#animation");
  ASSERT_THAT(animation, testing::Ne(std::nullopt));
  const auto href = [&]() {
    return animation->entityHandle().get<components::SetAnimationComponent>().href;
  };

  EXPECT_THAT(href(), testing::Optional(testing::StrEq("#primary")));
  EXPECT_THAT(AnimatedOverride(document, "#primary", "width", 1),
              testing::Optional(testing::StrEq("40")));
  EXPECT_THAT(AnimatedOverride(document, "#legacy", "width", 1), testing::Eq(std::nullopt));

  animation->setAttribute({"xlink", "href"}, "#replacement");
  EXPECT_THAT(AnimatedOverride(document, "#primary", "width", 1),
              testing::Optional(testing::StrEq("40")));
  EXPECT_THAT(AnimatedOverride(document, "#replacement", "width", 1), testing::Eq(std::nullopt));
  EXPECT_THAT(href(), testing::Optional(testing::StrEq("#primary")));

  animation->setAttribute("href", "");
  EXPECT_THAT(AnimatedOverride(document, "#primary", "width", 1), testing::Eq(std::nullopt));
  EXPECT_THAT(AnimatedOverride(document, "#legacy", "width", 1), testing::Eq(std::nullopt));
  EXPECT_THAT(href(), testing::Optional(testing::IsEmpty()));

  animation->removeAttribute("href");
  EXPECT_THAT(AnimatedOverride(document, "#replacement", "width", 1),
              testing::Optional(testing::StrEq("40")));
  EXPECT_THAT(href(), testing::Optional(testing::StrEq("#replacement")));

  animation->setAttribute("href", "#primary");
  EXPECT_THAT(AnimatedOverride(document, "#primary", "width", 1),
              testing::Optional(testing::StrEq("40")));
  EXPECT_THAT(href(), testing::Optional(testing::StrEq("#primary")));
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

TEST(SVGSetElement, ProcessingModesControlAnimationEvaluation) {
  constexpr std::string_view kSource =
      "<svg xmlns='http://www.w3.org/2000/svg'><rect id='r' fill='red'>"
      "<set attributeName='fill' to='blue' begin='0s' dur='1s' fill='freeze'/></rect></svg>";
  for (const ProcessingMode mode : {ProcessingMode::DynamicInteractive,
                                    ProcessingMode::SecureStatic, ProcessingMode::SecureAnimated}) {
    SCOPED_TRACE(static_cast<int>(mode));
    SVGDocument::Settings settings;
    settings.processingMode = mode;
    parser::SVGParser::Options options;
    options.enableExperimental = true;
    ParseWarningSink warnings;
    auto parsed = parser::SVGParser::ParseSVG(kSource, warnings, options, std::move(settings));
    ASSERT_THAT(parsed, NoParseError());
    auto& registry = parsed.result().registry();
    components::AnimationSystem().advance(registry, 0.0);
    const auto* values =
        registry.try_get<components::AnimatedValuesComponent>(EntityById(parsed.result(), "#r"));
    if (mode == ProcessingMode::SecureStatic) {
      EXPECT_EQ(values, nullptr);
    } else {
      ASSERT_NE(values, nullptr);
      ASSERT_TRUE(values->contextualLengths.contains("fill"));
    }
    ParseWarningSink renderingWarnings = ParseWarningSink::Disabled();
    RendererUtils::prepareDocumentForRendering(parsed.result(), false, renderingWarnings);
    const auto target = parsed.result().querySelector("#r");
    ASSERT_TRUE(target.has_value());
    const auto paint = target->getComputedStyle().fill.get();
    ASSERT_TRUE(paint && paint->is<PaintServer::Solid>());
    const auto expected = mode == ProcessingMode::SecureStatic ? css::RGBA::RGB(255, 0, 0)
                                                               : css::RGBA::RGB(0, 0, 255);
    EXPECT_EQ(paint->get<PaintServer::Solid>().color.resolve(css::RGBA::RGB(0, 0, 0), 1), expected);
  }
}

TEST(SVGSetElement, SecureStaticClearsPreviouslySampledOverrides) {
  auto document = parseSVGWithExperimental(
      "<svg xmlns='http://www.w3.org/2000/svg'><rect id='r' fill='red'>"
      "<set attributeName='fill' to='blue' begin='0s' dur='1s' fill='freeze'/></rect></svg>");
  auto& registry = document.registry();
  const Entity target = EntityById(document, "#r");
  components::AnimationSystem().advance(registry, 0.0);
  ASSERT_NE(registry.try_get<components::AnimatedValuesComponent>(target), nullptr);
  registry.ctx().get<components::ResourceManagerContext>().setProcessingMode(
      ProcessingMode::SecureStatic);
  components::AnimationSystem().advance(registry, 0.0);
  EXPECT_EQ(registry.try_get<components::AnimatedValuesComponent>(target), nullptr);
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
  document.setTime(3.0);
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  RendererUtils::prepareDocumentForRendering(document, false, warnings);

  // The rect should have animated override for fill
  auto rect = document.querySelector("#r");
  ASSERT_TRUE(rect.has_value());
  auto* animValues =
      registry.try_get<components::AnimatedValuesComponent>(rect->entityHandle().entity());
  ASSERT_NE(animValues, nullptr);
  ASSERT_TRUE(animValues->contextualLengths.contains("fill"));
  EXPECT_EQ(rect->getComputedStyle().fill.get(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA::RGB(0, 0, 255)))));
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
  document.setTime(6.0);
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  RendererUtils::prepareDocumentForRendering(document, false, warnings);

  // fill="freeze" persists the value
  auto rect = document.querySelector("#r");
  ASSERT_TRUE(rect.has_value());
  auto* animValues =
      registry.try_get<components::AnimatedValuesComponent>(rect->entityHandle().entity());
  ASSERT_NE(animValues, nullptr);
  ASSERT_TRUE(animValues->contextualLengths.contains("fill"));
  EXPECT_EQ(rect->getComputedStyle().fill.get(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA::RGB(0, 0, 255)))));
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
  document.setTime(100.0);
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  RendererUtils::prepareDocumentForRendering(document, false, warnings);

  auto rect = document.querySelector("#r");
  ASSERT_TRUE(rect.has_value());
  auto* animValues =
      registry.try_get<components::AnimatedValuesComponent>(rect->entityHandle().entity());
  ASSERT_NE(animValues, nullptr);
  ASSERT_TRUE(animValues->contextualLengths.contains("fill"));
  EXPECT_EQ(rect->getComputedStyle().fill.get(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA::RGB(0, 0, 255)))));
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
  components::AnimationSystem().advance(registry, 0.0, nullptr);
  registry.get<components::SetAnimationComponent>(*view.begin())
      .to.emplace(components::AnimationResourceBudget::kMaximumSourceBytes + 1, 'x');

  components::AnimationSystem().advance(registry, 1.0, nullptr);

  const auto* budget = registry.ctx().find<components::AnimationResourceBudget>();
  ASSERT_NE(budget, nullptr);
  EXPECT_TRUE(budget->rejected());
  EXPECT_EQ(registry.try_get<components::AnimatedValuesComponent>(
                document.querySelector("#r")->entityHandle().entity()),
            nullptr);
}

}  // namespace donner::svg
