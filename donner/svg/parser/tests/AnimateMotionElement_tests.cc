#include <gtest/gtest.h>

#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/animation/AnimateMotionComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::svg {

namespace {

SVGDocument parseSVGWithExperimental(std::string_view svg) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  std::vector<ParseError> warnings;
  auto result = parser::SVGParser::ParseSVG(svg, &warnings, options);
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

/// Parse translate(x, y) from a transform string and return the x,y values.
bool parseTranslateFromString(const std::string& transform, double& x, double& y) {
  auto pos = transform.find("translate(");
  if (pos == std::string::npos) {
    return false;
  }
  const char* ptr = transform.c_str() + pos + 10;  // skip "translate("
  char* endPtr = nullptr;
  x = std::strtod(ptr, &endPtr);
  if (endPtr == ptr) {
    return false;
  }
  ptr = endPtr;
  while (*ptr == ' ' || *ptr == ',') {
    ++ptr;
  }
  y = std::strtod(ptr, &endPtr);
  if (endPtr == ptr) {
    y = 0.0;
  }
  return true;
}

}  // namespace

// --- Parsing tests ---

TEST(SVGAnimateMotionElement, ParsePath) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <circle id="c" cx="0" cy="0" r="5">
        <animateMotion path="M0,0 L100,0 L100,100" begin="0s" dur="3s" />
      </circle>
    </svg>
  )");

  auto& registry = document.registry();
  auto view = registry.view<components::AnimateMotionComponent>();
  ASSERT_FALSE(view.begin() == view.end());

  auto entity = *view.begin();
  auto& comp = registry.get<components::AnimateMotionComponent>(entity);
  ASSERT_TRUE(comp.path.has_value());
  EXPECT_EQ(comp.path.value(), "M0,0 L100,0 L100,100");
}

TEST(SVGAnimateMotionElement, ParseFromTo) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <circle id="c" cx="0" cy="0" r="5">
        <animateMotion from="0,0" to="100,50" begin="0s" dur="2s" />
      </circle>
    </svg>
  )");

  auto& registry = document.registry();
  auto view = registry.view<components::AnimateMotionComponent>();
  auto entity = *view.begin();
  auto& comp = registry.get<components::AnimateMotionComponent>(entity);

  ASSERT_TRUE(comp.from.has_value());
  EXPECT_EQ(comp.from.value(), "0,0");
  ASSERT_TRUE(comp.to.has_value());
  EXPECT_EQ(comp.to.value(), "100,50");
}

TEST(SVGAnimateMotionElement, ParseRotateAuto) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <circle id="c" cx="0" cy="0" r="5">
        <animateMotion path="M0,0 L100,0" rotate="auto" begin="0s" dur="2s" />
      </circle>
    </svg>
  )");

  auto& registry = document.registry();
  auto view = registry.view<components::AnimateMotionComponent>();
  auto entity = *view.begin();
  auto& comp = registry.get<components::AnimateMotionComponent>(entity);
  EXPECT_EQ(comp.rotate, "auto");
}

// --- Motion interpolation tests ---

TEST(SVGAnimateMotionElement, StraightLineFromTo) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <circle id="c" cx="0" cy="0" r="5">
        <animateMotion from="0,0" to="100,0" begin="0s" dur="2s" />
      </circle>
    </svg>
  )");

  // At t=0: position (0,0)
  auto val0 = getAnimatedTransform(document, "#c", 0.0);
  ASSERT_TRUE(val0.has_value());
  double x = 0, y = 0;
  ASSERT_TRUE(parseTranslateFromString(val0.value(), x, y));
  EXPECT_NEAR(x, 0.0, 0.5);
  EXPECT_NEAR(y, 0.0, 0.5);

  // At t=1s (50%): position (50,0)
  auto val1 = getAnimatedTransform(document, "#c", 1.0);
  ASSERT_TRUE(val1.has_value());
  ASSERT_TRUE(parseTranslateFromString(val1.value(), x, y));
  EXPECT_NEAR(x, 50.0, 0.5);
  EXPECT_NEAR(y, 0.0, 0.5);

  // At t=1.5s (75%): position (75,0)
  auto val15 = getAnimatedTransform(document, "#c", 1.5);
  ASSERT_TRUE(val15.has_value());
  ASSERT_TRUE(parseTranslateFromString(val15.value(), x, y));
  EXPECT_NEAR(x, 75.0, 0.5);
  EXPECT_NEAR(y, 0.0, 0.5);
}

TEST(SVGAnimateMotionElement, PathMotion) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <circle id="c" cx="0" cy="0" r="5">
        <animateMotion path="M0,0 L100,0 L100,100" begin="0s" dur="4s" />
      </circle>
    </svg>
  )");

  // Path is L-shaped: 100px right, then 100px down. Total length = 200px.
  // At t=1s (25%): 50px along first segment → (50, 0)
  auto val1 = getAnimatedTransform(document, "#c", 1.0);
  ASSERT_TRUE(val1.has_value());
  double x = 0, y = 0;
  ASSERT_TRUE(parseTranslateFromString(val1.value(), x, y));
  EXPECT_NEAR(x, 50.0, 1.0);
  EXPECT_NEAR(y, 0.0, 1.0);

  // At t=2s (50%): at the corner → (100, 0)
  auto val2 = getAnimatedTransform(document, "#c", 2.0);
  ASSERT_TRUE(val2.has_value());
  ASSERT_TRUE(parseTranslateFromString(val2.value(), x, y));
  EXPECT_NEAR(x, 100.0, 1.0);
  EXPECT_NEAR(y, 0.0, 1.0);

  // At t=3s (75%): 50px along second segment → (100, 50)
  auto val3 = getAnimatedTransform(document, "#c", 3.0);
  ASSERT_TRUE(val3.has_value());
  ASSERT_TRUE(parseTranslateFromString(val3.value(), x, y));
  EXPECT_NEAR(x, 100.0, 1.0);
  EXPECT_NEAR(y, 50.0, 1.0);
}

TEST(SVGAnimateMotionElement, RotateAuto) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <circle id="c" cx="0" cy="0" r="5">
        <animateMotion from="0,0" to="100,0" rotate="auto" begin="0s" dur="2s" />
      </circle>
    </svg>
  )");

  // Moving right → tangent angle is 0 degrees.
  auto val = getAnimatedTransform(document, "#c", 1.0);
  ASSERT_TRUE(val.has_value());
  // Should contain "rotate(0)" since direction is along positive x-axis.
  EXPECT_NE(val.value().find("rotate("), std::string::npos);
}

TEST(SVGAnimateMotionElement, FreezeAtEnd) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <circle id="c" cx="0" cy="0" r="5">
        <animateMotion from="0,0" to="100,50" begin="0s" dur="1s" fill="freeze" />
      </circle>
    </svg>
  )");

  // After end with freeze: should be at (100, 50)
  auto val = getAnimatedTransform(document, "#c", 2.0);
  ASSERT_TRUE(val.has_value());
  double x = 0, y = 0;
  ASSERT_TRUE(parseTranslateFromString(val.value(), x, y));
  EXPECT_NEAR(x, 100.0, 0.5);
  EXPECT_NEAR(y, 50.0, 0.5);
}

// --- <mpath> child element tests ---

TEST(SVGAnimateMotionElement, MPathParsing) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <path id="p1" d="M0,0 L200,0" />
      <circle id="c" cx="0" cy="0" r="5">
        <animateMotion begin="0s" dur="2s">
          <mpath href="#p1" />
        </animateMotion>
      </circle>
    </svg>
  )");

  // At t=1s (50%): should be at (100, 0)
  auto val = getAnimatedTransform(document, "#c", 1.0);
  ASSERT_TRUE(val.has_value());
  double x = 0, y = 0;
  ASSERT_TRUE(parseTranslateFromString(val.value(), x, y));
  EXPECT_NEAR(x, 100.0, 1.0);
  EXPECT_NEAR(y, 0.0, 1.0);
}

TEST(SVGAnimateMotionElement, MPathXlinkHref) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink">
      <path id="p1" d="M0,0 L0,100" />
      <circle id="c" cx="0" cy="0" r="5">
        <animateMotion begin="0s" dur="2s">
          <mpath xlink:href="#p1" />
        </animateMotion>
      </circle>
    </svg>
  )");

  // At t=1s (50%): should be at (0, 50)
  auto val = getAnimatedTransform(document, "#c", 1.0);
  ASSERT_TRUE(val.has_value());
  double x = 0, y = 0;
  ASSERT_TRUE(parseTranslateFromString(val.value(), x, y));
  EXPECT_NEAR(x, 0.0, 1.0);
  EXPECT_NEAR(y, 50.0, 1.0);
}

TEST(SVGAnimateMotionElement, MPathOverridesPathAttribute) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <path id="p1" d="M0,0 L0,200" />
      <circle id="c" cx="0" cy="0" r="5">
        <animateMotion path="M0,0 L200,0" begin="0s" dur="2s">
          <mpath href="#p1" />
        </animateMotion>
      </circle>
    </svg>
  )");

  // mpath should take precedence over path attribute.
  // mpath points down (y direction), path attribute points right (x direction).
  auto val = getAnimatedTransform(document, "#c", 1.0);
  ASSERT_TRUE(val.has_value());
  double x = 0, y = 0;
  ASSERT_TRUE(parseTranslateFromString(val.value(), x, y));
  // Should follow mpath (down), not path attribute (right).
  EXPECT_NEAR(x, 0.0, 1.0);
  EXPECT_NEAR(y, 100.0, 1.0);
}

TEST(SVGAnimateMotionElement, MPathWithComplexPath) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <path id="lpath" d="M0,0 L100,0 L100,100" />
      <circle id="c" cx="0" cy="0" r="5">
        <animateMotion begin="0s" dur="4s">
          <mpath href="#lpath" />
        </animateMotion>
      </circle>
    </svg>
  )");

  // L-shaped path: 100px right then 100px down. Same as PathMotion test but via <mpath>.
  // At t=2s (50%): at the corner (100, 0)
  auto val = getAnimatedTransform(document, "#c", 2.0);
  ASSERT_TRUE(val.has_value());
  double x = 0, y = 0;
  ASSERT_TRUE(parseTranslateFromString(val.value(), x, y));
  EXPECT_NEAR(x, 100.0, 1.0);
  EXPECT_NEAR(y, 0.0, 1.0);

  // At t=3s (75%): 50px along second segment → (100, 50)
  auto val3 = getAnimatedTransform(document, "#c", 3.0);
  ASSERT_TRUE(val3.has_value());
  ASSERT_TRUE(parseTranslateFromString(val3.value(), x, y));
  EXPECT_NEAR(x, 100.0, 1.0);
  EXPECT_NEAR(y, 50.0, 1.0);
}

TEST(SVGAnimateMotionElement, MPathInvalidHrefFallsBack) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <circle id="c" cx="0" cy="0" r="5">
        <animateMotion path="M0,0 L100,0" begin="0s" dur="2s">
          <mpath href="#nonexistent" />
        </animateMotion>
      </circle>
    </svg>
  )");

  // mpath href is invalid, so should fall back to the path attribute.
  auto val = getAnimatedTransform(document, "#c", 1.0);
  ASSERT_TRUE(val.has_value());
  double x = 0, y = 0;
  ASSERT_TRUE(parseTranslateFromString(val.value(), x, y));
  EXPECT_NEAR(x, 50.0, 1.0);
  EXPECT_NEAR(y, 0.0, 1.0);
}

TEST(SVGAnimateMotionElement, ValuesPointList) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <circle id="c" cx="0" cy="0" r="5">
        <animateMotion values="0,0;100,0;100,100" begin="0s" dur="4s" />
      </circle>
    </svg>
  )");

  // Same as path L-shape test, but via values.
  auto val = getAnimatedTransform(document, "#c", 1.0);
  ASSERT_TRUE(val.has_value());
  double x = 0, y = 0;
  ASSERT_TRUE(parseTranslateFromString(val.value(), x, y));
  EXPECT_NEAR(x, 50.0, 1.0);
  EXPECT_NEAR(y, 0.0, 1.0);
}

// --- keyPoints tests ---

TEST(SVGAnimateMotionElement, KeyPointsNonUniform) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <circle id="c" cx="0" cy="0" r="5">
        <animateMotion path="M0,0 L200,0" begin="0s" dur="2s"
                       keyPoints="0;0.25;1" calcMode="linear" />
      </circle>
    </svg>
  )");

  // keyPoints=[0, 0.25, 1] with 3 points, evenly-spaced keyTimes.
  // At t=0.5s (25% progress): halfway through first interval [0, 0.25].
  // path progress = lerp(0, 0.25, 0.5) = 0.125 → position 25px along 200px path.
  auto val05 = getAnimatedTransform(document, "#c", 0.5);
  ASSERT_TRUE(val05.has_value());
  double x = 0, y = 0;
  ASSERT_TRUE(parseTranslateFromString(val05.value(), x, y));
  EXPECT_NEAR(x, 25.0, 2.0);
  EXPECT_NEAR(y, 0.0, 1.0);

  // At t=1.5s (75% progress): halfway through second interval [0.25, 1].
  // path progress = lerp(0.25, 1, 0.5) = 0.625 → position 125px.
  auto val15 = getAnimatedTransform(document, "#c", 1.5);
  ASSERT_TRUE(val15.has_value());
  ASSERT_TRUE(parseTranslateFromString(val15.value(), x, y));
  EXPECT_NEAR(x, 125.0, 2.0);
  EXPECT_NEAR(y, 0.0, 1.0);
}

TEST(SVGAnimateMotionElement, KeyPointsReversed) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <circle id="c" cx="0" cy="0" r="5">
        <animateMotion path="M0,0 L100,0" begin="0s" dur="2s"
                       keyPoints="1;0" calcMode="linear" />
      </circle>
    </svg>
  )");

  // keyPoints=[1, 0] → element starts at end of path and moves backwards.
  // At t=0: path progress = 1.0 → (100, 0).
  auto val0 = getAnimatedTransform(document, "#c", 0.0);
  ASSERT_TRUE(val0.has_value());
  double x = 0, y = 0;
  ASSERT_TRUE(parseTranslateFromString(val0.value(), x, y));
  EXPECT_NEAR(x, 100.0, 1.0);
  EXPECT_NEAR(y, 0.0, 1.0);

  // At t=1s (50%): path progress = 0.5 → (50, 0).
  auto val1 = getAnimatedTransform(document, "#c", 1.0);
  ASSERT_TRUE(val1.has_value());
  ASSERT_TRUE(parseTranslateFromString(val1.value(), x, y));
  EXPECT_NEAR(x, 50.0, 1.0);
  EXPECT_NEAR(y, 0.0, 1.0);
}

}  // namespace donner::svg
