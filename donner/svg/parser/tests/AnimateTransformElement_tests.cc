#include <gtest/gtest.h>

#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/animation/AnimateTransformComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/parser/SVGParser.h"

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

std::optional<std::string> getAnimatedTransform(SVGDocument& document, const char* selector,
                                                double time) {
  auto& registry = document.registry();
  components::AnimationSystem().advance(registry, time, nullptr);

  auto element = document.querySelector(selector);
  if (!element.has_value()) {
    return std::nullopt;
  }

  auto* animValues =
      registry.try_get<components::AnimatedValuesComponent>(element->entityHandle().entity());
  if (!animValues) {
    return std::nullopt;
  }

  auto it = animValues->overrides.find("transform");
  if (it == animValues->overrides.end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace

// --- Parsing tests ---

TEST(SVGAnimateTransformElement, ParseRotate) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animateTransform attributeName="transform" type="rotate"
                          from="0 50 50" to="360 50 50" begin="0s" dur="4s" />
      </rect>
    </svg>
  )");

  auto& registry = document.registry();
  auto view = registry.view<components::AnimateTransformComponent>();
  ASSERT_FALSE(view.begin() == view.end());

  auto entity = *view.begin();
  auto& comp = registry.get<components::AnimateTransformComponent>(entity);
  EXPECT_EQ(comp.type, components::TransformAnimationType::Rotate);
  ASSERT_TRUE(comp.from.has_value());
  EXPECT_EQ(comp.from.value(), "0 50 50");
  ASSERT_TRUE(comp.to.has_value());
  EXPECT_EQ(comp.to.value(), "360 50 50");
}

TEST(SVGAnimateTransformElement, ParseTranslate) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animateTransform attributeName="transform" type="translate"
                          from="0" to="100" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  auto& registry = document.registry();
  auto view = registry.view<components::AnimateTransformComponent>();
  auto entity = *view.begin();
  auto& comp = registry.get<components::AnimateTransformComponent>(entity);
  EXPECT_EQ(comp.type, components::TransformAnimationType::Translate);
}

TEST(SVGAnimateTransformElement, ParseScale) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animateTransform attributeName="transform" type="scale"
                          from="1" to="2" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  auto& registry = document.registry();
  auto view = registry.view<components::AnimateTransformComponent>();
  auto entity = *view.begin();
  auto& comp = registry.get<components::AnimateTransformComponent>(entity);
  EXPECT_EQ(comp.type, components::TransformAnimationType::Scale);
}

// --- Interpolation tests ---

TEST(SVGAnimateTransformElement, RotateInterpolation) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animateTransform attributeName="transform" type="rotate"
                          from="0 50 50" to="360 50 50" begin="0s" dur="4s" />
      </rect>
    </svg>
  )");

  // At t=0: rotate(0, 50, 50)
  auto val0 = getAnimatedTransform(document, "#r", 0.0);
  ASSERT_TRUE(val0.has_value());
  EXPECT_EQ(val0.value(), "rotate(0, 50, 50)");

  // At t=2s (50%): rotate(180, 50, 50)
  auto val2 = getAnimatedTransform(document, "#r", 2.0);
  ASSERT_TRUE(val2.has_value());
  EXPECT_EQ(val2.value(), "rotate(180, 50, 50)");
}

TEST(SVGAnimateTransformElement, TranslateInterpolation) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animateTransform attributeName="transform" type="translate"
                          from="0 0" to="100 50" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  // At t=1s (50%): translate(50, 25)
  auto val = getAnimatedTransform(document, "#r", 1.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), "translate(50, 25)");
}

TEST(SVGAnimateTransformElement, ScaleInterpolation) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animateTransform attributeName="transform" type="scale"
                          from="1 1" to="2 3" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  // At t=1s (50%): scale(1.5, 2)
  auto val = getAnimatedTransform(document, "#r", 1.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), "scale(1.5, 2)");
}

TEST(SVGAnimateTransformElement, SkewXInterpolation) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animateTransform attributeName="transform" type="skewX"
                          from="0" to="30" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  // At t=1s (50%): skewX(15)
  auto val = getAnimatedTransform(document, "#r", 1.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), "skewX(15)");
}

TEST(SVGAnimateTransformElement, ByOnlyTranslate) {
  // A by-only animation goes from the neutral value (translate 0 0) to `by`.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animateTransform attributeName="transform" type="translate"
                          by="100 50" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  // At t=1s (50%): translate(50, 25)
  auto val = getAnimatedTransform(document, "#r", 1.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), "translate(50, 25)");
}

TEST(SVGAnimateTransformElement, FromByTranslate) {
  // A from/by animation goes from `from` to `from + by`.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animateTransform attributeName="transform" type="translate"
                          from="10 20" by="100 50" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  // At t=1s (50%): from (10, 20) halfway to (110, 70) is translate(60, 45)
  auto val = getAnimatedTransform(document, "#r", 1.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), "translate(60, 45)");
}

TEST(SVGAnimateTransformElement, ByOnlyScale) {
  // For scale, the neutral value is 1, so by="2" animates from scale(1) to scale(3).
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animateTransform attributeName="transform" type="scale"
                          by="2" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  // At t=1s (50%): scale(2)
  auto val = getAnimatedTransform(document, "#r", 1.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), "scale(2)");
}

TEST(SVGAnimateTransformElement, ByOnlyRotate) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animateTransform attributeName="transform" type="rotate"
                          by="90" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  // At t=1s (50%): rotate(45)
  auto val = getAnimatedTransform(document, "#r", 1.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), "rotate(45)");
}

TEST(SVGAnimateTransformElement, FreezeAfterEnd) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animateTransform attributeName="transform" type="rotate"
                          from="0" to="90" begin="0s" dur="1s" fill="freeze" />
      </rect>
    </svg>
  )");

  // After end with freeze: should be rotate(90)
  auto val = getAnimatedTransform(document, "#r", 2.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), "rotate(90)");
}

TEST(SVGAnimateTransformElement, ValuesListInterpolation) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animateTransform attributeName="transform" type="rotate"
                          values="0;90;0" begin="0s" dur="4s" />
      </rect>
    </svg>
  )");

  // At t=1s (25%, midpoint of first interval): rotate(45)
  auto val1 = getAnimatedTransform(document, "#r", 1.0);
  ASSERT_TRUE(val1.has_value());
  EXPECT_EQ(val1.value(), "rotate(45)");

  // At t=2s (50%, start of second interval): rotate(90)
  auto val2 = getAnimatedTransform(document, "#r", 2.0);
  ASSERT_TRUE(val2.has_value());
  EXPECT_EQ(val2.value(), "rotate(90)");

  // At t=3s (75%, midpoint of second interval): rotate(45)
  auto val3 = getAnimatedTransform(document, "#r", 3.0);
  ASSERT_TRUE(val3.has_value());
  EXPECT_EQ(val3.value(), "rotate(45)");
}

TEST(SVGAnimateTransformElement, SkewYInterpolation) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animateTransform attributeName="transform" type="skewY"
                          from="0" to="40" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  auto val = getAnimatedTransform(document, "#r", 1.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), "skewY(20)");
}

TEST(SVGAnimateTransformElement, SkewXByOnlyStartsFromIdentity) {
  // A by-only skew animates from the neutral skewX(0) to skewX(by).
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animateTransform attributeName="transform" type="skewX"
                          by="30" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  auto val = getAnimatedTransform(document, "#r", 1.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), "skewX(15)");
}

TEST(SVGAnimateTransformElement, ToOnlyTranslateAppliesConstant) {
  // A to-only animateTransform has a single keyframe applied for the whole active interval.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animateTransform attributeName="transform" type="translate"
                          to="100 50" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  auto val05 = getAnimatedTransform(document, "#r", 0.5);
  ASSERT_TRUE(val05.has_value());
  EXPECT_EQ(val05.value(), "translate(100, 50)");

  auto val15 = getAnimatedTransform(document, "#r", 1.5);
  ASSERT_TRUE(val15.has_value());
  EXPECT_EQ(val15.value(), "translate(100, 50)");
}

TEST(SVGAnimateTransformElement, MissingValueSpecProducesNoOverride) {
  // <animateTransform> without values/from/to/by has nothing to apply.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animateTransform attributeName="transform" type="scale" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  EXPECT_FALSE(getAnimatedTransform(document, "#r", 1.0).has_value());
}

TEST(SVGAnimateTransformElement, SingleNumberTranslatePadsTyToZero) {
  // translate with one number pads ty to 0, which is omitted from the formatted result.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animateTransform attributeName="transform" type="translate"
                          from="0" to="100" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  auto val = getAnimatedTransform(document, "#r", 1.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), "translate(50)");
}

TEST(SVGAnimateTransformElement, TwoNumberRotatePadsCenterY) {
  // rotate with two numbers pads cy to 0; a nonzero cx keeps the center in the output.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animateTransform attributeName="transform" type="rotate"
                          from="0 25" to="90 25" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  auto val = getAnimatedTransform(document, "#r", 1.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), "rotate(45, 25, 0)");
}

}  // namespace donner::svg
