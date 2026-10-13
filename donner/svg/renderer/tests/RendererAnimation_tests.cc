#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/SVGFEOffsetElement.h"
#include "donner/svg/SVGLineElement.h"
#include "donner/svg/SVGPolygonElement.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/components/shape/PathComponent.h"
#include "donner/svg/components/shape/PolyComponent.h"
#include "donner/svg/renderer/AnimationPresentation.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#include "donner/svg/tests/ParserTestUtils.h"

/// @file
/// Time-sampled golden render checks for the initial SMIL animation subset. Each test renders a
/// small SVG at fixed document times and compares the result against a hand-verifiable ASCII golden
/// (black fill renders as '@', background as '.'). Covers before/mid/after phases, freeze vs remove
/// fill, remove revert across time samples of one live document, transform animation, and
/// repeatCount.

namespace donner::svg {
namespace {

/// Parse an animated fragment through the default parser on a 16x16 canvas.
SVGDocument parseAnimated(std::string_view fragment) {
  parser::SVGParser::Options options;
  return instantiateSubtree(fragment, options, Vector2i(16, 16));
}

}  // namespace

TEST(RendererAnimation, GeometryGrowFreeze) {
  // <animate> on rect width from 4 to 12 over 2s, frozen after.
  SVGDocument document = parseAnimated(R"(
    <rect x="0" y="0" width="4" height="16" fill="black">
      <animate attributeName="width" from="4" to="12" begin="0s" dur="2s" fill="freeze" />
    </rect>
  )");

  // t=0 (before/start): width = 4.
  document.setTime(0.0);
  EXPECT_TRUE(RendererTestUtils::renderToAsciiImage(document).matches(R"(
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    )"));

  // t=1 (mid, 50%): width = 8.
  document.setTime(1.0);
  EXPECT_TRUE(RendererTestUtils::renderToAsciiImage(document).matches(R"(
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    )"));

  // t=3 (after, frozen): width = 12.
  document.setTime(3.0);
  EXPECT_TRUE(RendererTestUtils::renderToAsciiImage(document).matches(R"(
    @@@@@@@@@@@@....
    @@@@@@@@@@@@....
    @@@@@@@@@@@@....
    @@@@@@@@@@@@....
    @@@@@@@@@@@@....
    @@@@@@@@@@@@....
    @@@@@@@@@@@@....
    @@@@@@@@@@@@....
    @@@@@@@@@@@@....
    @@@@@@@@@@@@....
    @@@@@@@@@@@@....
    @@@@@@@@@@@@....
    @@@@@@@@@@@@....
    @@@@@@@@@@@@....
    @@@@@@@@@@@@....
    @@@@@@@@@@@@....
    )"));
}

TEST(RendererAnimation, GeometryRemoveReverts) {
  // Default fill="remove": after the active interval the width reverts to its base (4). Rendering
  // the same live document at t=1 (active) then t=3 (after) exercises the base-component
  // backup/restore path.
  SVGDocument document = parseAnimated(R"(
    <rect x="0" y="0" width="4" height="16" fill="black">
      <animate attributeName="width" from="4" to="12" begin="0s" dur="2s" />
    </rect>
  )");

  // t=1 (active, 50%): width = 8.
  document.setTime(1.0);
  EXPECT_TRUE(RendererTestUtils::renderToAsciiImage(document).matches(R"(
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    )"));

  // t=3 (after, remove): reverts to base width = 4.
  document.setTime(3.0);
  EXPECT_TRUE(RendererTestUtils::renderToAsciiImage(document).matches(R"(
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    )"));
}

TEST(RendererAnimation, TransformTranslateFreeze) {
  // <animateTransform> translate from 0 to 8 over 2s, frozen after. A 4-wide bar slides right.
  SVGDocument document = parseAnimated(R"(
    <rect x="0" y="0" width="4" height="16" fill="black">
      <animateTransform attributeName="transform" type="translate"
                        from="0 0" to="8 0" begin="0s" dur="2s" fill="freeze" />
    </rect>
  )");

  // t=0: translate(0) -> columns 0-3.
  document.setTime(0.0);
  EXPECT_TRUE(RendererTestUtils::renderToAsciiImage(document).matches(R"(
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    )"));

  // t=1 (mid): translate(4) -> columns 4-7.
  document.setTime(1.0);
  EXPECT_TRUE(RendererTestUtils::renderToAsciiImage(document).matches(R"(
    ....@@@@........
    ....@@@@........
    ....@@@@........
    ....@@@@........
    ....@@@@........
    ....@@@@........
    ....@@@@........
    ....@@@@........
    ....@@@@........
    ....@@@@........
    ....@@@@........
    ....@@@@........
    ....@@@@........
    ....@@@@........
    ....@@@@........
    ....@@@@........
    )"));

  // t=3 (after, frozen): translate(8) -> columns 8-11.
  document.setTime(3.0);
  EXPECT_TRUE(RendererTestUtils::renderToAsciiImage(document).matches(R"(
    ........@@@@....
    ........@@@@....
    ........@@@@....
    ........@@@@....
    ........@@@@....
    ........@@@@....
    ........@@@@....
    ........@@@@....
    ........@@@@....
    ........@@@@....
    ........@@@@....
    ........@@@@....
    ........@@@@....
    ........@@@@....
    ........@@@@....
    ........@@@@....
    )"));
}

TEST(RendererAnimation, RepeatCount) {
  // width 0->16 over 2s, repeated twice. The frame at t=0.5 (iteration 0, 25%) and t=2.5
  // (iteration 1, 25%) must be identical (width = 4), demonstrating the repeat.
  SVGDocument document = parseAnimated(R"(
    <rect x="0" y="0" width="0" height="16" fill="black">
      <animate attributeName="width" from="0" to="16" begin="0s" dur="2s"
               repeatCount="2" fill="freeze" />
    </rect>
  )");

  // t=0.5: iteration 0, 25% -> width = 4.
  document.setTime(0.5);
  EXPECT_TRUE(RendererTestUtils::renderToAsciiImage(document).matches(R"(
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    )"));

  // t=2.5: iteration 1, 25% -> width = 4 again (repeat).
  document.setTime(2.5);
  EXPECT_TRUE(RendererTestUtils::renderToAsciiImage(document).matches(R"(
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    @@@@............
    )"));

  // t=5: after active duration (4s), frozen at final width = 16.
  document.setTime(5.0);
  EXPECT_TRUE(RendererTestUtils::renderToAsciiImage(document).matches(R"(
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    )"));
}

TEST(RendererAnimation, AnimatedGroupFillIsInheritedAndReverts) {
  auto document = parseAnimated(R"svg(
    <g fill="white"><set attributeName="fill" to="black" begin="1s" dur="1s"/>
      <rect width="16" height="16"/></g>)svg");
  document.setTime(1.5);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith(std::string(16, '@')));
  document.setTime(3);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith(std::string(16, '.')));
}

TEST(RendererAnimation, AnimatedOpacityOverridesStyleButRespectsImportant) {
  auto document = parseAnimated(R"svg(
    <rect width="16" height="16" fill="black" style="opacity:1">
      <animate attributeName="opacity" from="0" to="0" dur="2s"/>
    </rect>)svg");
  document.setTime(1);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith(std::string(16, '.')));
  auto important = parseAnimated(R"svg(
    <rect width="16" height="16" fill="black" style="opacity:1!important">
      <animate attributeName="opacity" from="0" to="0" dur="2s"/>
    </rect>)svg");
  important.setTime(1);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(important).generated,
              testing::StartsWith(std::string(16, '@')));
}

TEST(RendererAnimation, AuthoredGeometryChangeSurvivesAnimationRemoval) {
  auto document = parseAnimated(R"svg(
    <rect id="r" width="4" height="16" fill="black">
      <animate attributeName="width" from="4" to="8" dur="1s"/>
    </rect>)svg");
  document.setTime(0.5);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@@@.........."));
  auto rect = document.querySelector("#r");
  ASSERT_THAT(rect, testing::Ne(std::nullopt));
  rect->setAttribute("width", "12");
  document.setTime(2);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@@@@@@@@@...."));
}

TEST(RendererAnimation, PolygonPointAnimationRendersAndReverts) {
  auto document = parseAnimated(R"svg(
    <polygon points="0 0 4 0 4 16 0 16" fill="black">
      <animate attributeName="points" from="0 0 4 0 4 16 0 16" to="0 0 12 0 12 16 0 16" dur="2s"/>
    </polygon>)svg");
  document.setTime(1);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@@@@@........"));
  document.setTime(3);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@............"));
}

TEST(RendererAnimation, SecureStaticDoesNotEvaluateAnimation) {
  parser::SVGParser::Options options;
  SVGDocument::Settings settings;
  settings.processingMode = ProcessingMode::SecureStatic;
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <rect width="16" height="16" fill="black"><set attributeName="fill" to="white"/></rect>
    </svg>)svg",
                                            warnings, options, std::move(settings));
  ASSERT_THAT(parsed, NoParseError());
  parsed.result().setTime(1);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(parsed.result()).generated,
              testing::StartsWith(std::string(16, '@')));
  EXPECT_THAT(parsed.result().animationEvents(-1, 2).events, testing::IsEmpty());
}

TEST(RendererAnimation, LinearRgbColorInterpolationUsesLinearLight) {
  auto document = parseAnimated(R"svg(
    <rect width="16" height="16" color-interpolation="linearRGB">
      <animate attributeName="fill" from="black" to="white" dur="2s"/>
    </rect>)svg");
  document.setTime(1);
  const auto bitmap = RenderDocumentWithActiveBackend(document);
  EXPECT_THAT(test::PixelAt(bitmap, 8, 8), test::Rgba(188, 188, 188, 255));
}

TEST(RendererAnimation, ToOnlyGeometryUsesStylesheetUnderlyingValue) {
  auto document = parseAnimated(R"svg(
    <style>rect { width: 4px; }</style>
    <rect width="2" height="16" fill="black">
      <animate attributeName="width" to="12" dur="2s"/>
    </rect>)svg");
  document.setTime(1);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@@@@@........"));
}

TEST(RendererAnimation, FilterOffsetAnimationRendersAndRestores) {
  auto document = parseAnimated(R"svg(
    <defs><filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="16" height="16">
      <feOffset dx="0"><animate attributeName="dx" from="0" to="8" dur="2s"/></feOffset>
    </filter></defs><rect width="4" height="16" fill="black" filter="url(#f)"/>)svg");
  document.setTime(1);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("....@@@@........"));
  document.setTime(3);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@............"));
}

TEST(RendererAnimation, GradientGeometryMatchesIndependentStaticGradient) {
  auto document = parseAnimated(R"svg(
    <defs><linearGradient id="g" gradientUnits="userSpaceOnUse" x2="8">
      <animate attributeName="x2" from="8" to="24" dur="2s"/>
      <stop stop-color="black"/><stop offset="1" stop-color="white"/>
    </linearGradient></defs><rect width="16" height="16" fill="url(#g)"/>)svg");
  auto expected = parseAnimated(R"svg(
    <defs><linearGradient id="g" gradientUnits="userSpaceOnUse" x2="16">
      <stop stop-color="black"/><stop offset="1" stop-color="white"/>
    </linearGradient></defs><rect width="16" height="16" fill="url(#g)"/>)svg");
  document.setTime(1);
  EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated,
            RendererTestUtils::renderToAsciiImage(expected).generated);
}

TEST(RendererAnimation, AnimatedViewBoxMatchesIndependentStaticViewport) {
  auto document = parseAnimated(R"svg(
    <svg width="16" height="16" viewBox="0 0 16 16" preserveAspectRatio="none">
      <animate attributeName="viewBox" from="0 0 16 16" to="0 0 32 16" dur="1s" fill="freeze"/>
      <rect width="16" height="16" fill="black"/>
    </svg>)svg");
  document.setTime(2);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@@@@@........"));
}

TEST(RendererAnimation, AuthoredShapeEditsDuringPlaybackSurviveRemoval) {
  struct Case {
    std::string_view fragment;
    std::string_view attribute;
    std::string_view replacement;
    std::string_view expected;
    std::string_view removed;
  };
  const Case cases[] = {
      {R"(<path id="s" d="M0 0H4V16H0Z"><set attributeName="d" to="M0 0H8V16H0Z" dur="1s"/></path>)",
       "d", "M0 0H12V16H0Z", "@@@@@@@@@@@@....", "................"},
      {R"(<polygon id="s" points="0 0 4 0 4 16 0 16"><set attributeName="points" to="0 0 8 0 8 16 0 16" dur="1s"/></polygon>)",
       "points", "0 0 12 0 12 16 0 16", "@@@@@@@@@@@@....", "................"},
      {R"(<line id="s" x2="4" y1="1" y2="1" stroke="black" stroke-width="2"><set attributeName="x2" to="8" dur="1s"/></line>)",
       "x2", "12", "@@@@@@@@@@@@....", "................"},
      {R"svg(<rect id="s" width="4" height="16" transform="translate(4)"><set attributeName="transform" to="translate(8)" dur="1s"/></rect>)svg",
       "transform", "translate(12)", "............@@@@", "@@@@............"}};
  for (const auto& item : cases) {
    for (bool remove : {false, true}) {
      SCOPED_TRACE(testing::Message() << item.attribute << " remove=" << remove);
      auto document = parseAnimated(item.fragment);
      document.setTime(0.5);
      (void)RendererTestUtils::renderToAsciiImage(document);
      auto element = document.querySelector("#s").value();
      if (remove) {
        element.removeAttribute(item.attribute);
      } else {
        element.setAttribute(item.attribute, item.replacement);
      }
      document.setTime(2);
      EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
                  testing::StartsWith(std::string(remove ? item.removed : item.expected)));
    }
  }
}

TEST(RendererAnimation, TypedLineEditDuringPlaybackSurvivesRemoval) {
  auto document = parseAnimated(R"(
    <line id="s" x2="4" y1="1" y2="1" stroke="black" stroke-width="2">
      <set attributeName="x2" to="8" dur="1s"/></line>)");
  document.setTime(0.5);
  (void)RendererTestUtils::renderToAsciiImage(document);
  document.querySelector("#s")->cast<SVGLineElement>().setX2(Lengthd(12));
  document.setTime(2);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@@@@@@@@@...."));
}

TEST(RendererAnimation, TypedFilterEditDuringPlaybackSurvivesRemoval) {
  auto document = parseAnimated(R"svg(
    <defs><filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="16" height="16">
      <feOffset id="s" dx="0"><set attributeName="dx" to="4" dur="1s"/></feOffset>
    </filter></defs><rect width="4" height="16" fill="black" filter="url(#f)"/>)svg");
  document.setTime(0.5);
  (void)RendererTestUtils::renderToAsciiImage(document);
  document.querySelector("#s")->cast<SVGFEOffsetElement>().setOffset(12, 0);
  document.setTime(2);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("............@@@@"));
}

TEST(RendererAnimation, GeometryRestorationStorageSharesTheOutputBudget) {
  for (bool path : {false, true}) {
    SCOPED_TRACE(path);
    auto document = parseAnimated(
        path
            ? R"(<path id="s" d="M0 0L4 4"><set attributeName="d" to="M0 0L8 8" dur="2s"/></path>)"
            : R"(<polygon id="s" points="0 0 4 0 4 4"><set attributeName="points" to="0 0 8 0 8 8" dur="2s"/></polygon>)");
    auto handle = document.querySelector("#s")->entityHandle();
    components::AnimationSystem().advance(document.registry(), 1);
    components::AnimationResourceBudget smallGeometry;
    ASSERT_THAT(smallGeometry.reserveOutput(smallGeometry.kMaximumOutputBytes - 65536),
                testing::IsTrue());
    EXPECT_THAT(components::ReserveAnimationGeometryBackups(document.registry(), smallGeometry),
                testing::IsTrue());
    if (path) {
      PathBuilder spline;
      spline.moveTo({0, 0});
      for (int i = 0; i < 5000; ++i) {
        spline.lineTo({double(i), 0});
      }
      handle.get<components::PathComponent>().splineOverride = spline.build();
    } else {
      handle.get<components::PolyComponent>().points.resize(5000);
    }
    components::AnimationResourceBudget largeGeometry;
    ASSERT_THAT(largeGeometry.reserveOutput(largeGeometry.kMaximumOutputBytes - 65536),
                testing::IsTrue());
    EXPECT_THAT(components::ReserveAnimationGeometryBackups(document.registry(), largeGeometry),
                testing::IsFalse());
    EXPECT_THAT(largeGeometry.rejected(), testing::IsTrue());
  }
}

TEST(RendererAnimation, TypedEditsEqualToPresentationBecomeNewBaseValues) {
  auto document = parseAnimated(R"svg(
    <line id="line" x2="4"><set attributeName="x2" to="8" dur="1s"/></line>
    <polygon id="poly" points="0 0 4 0 4 4"><set attributeName="points" to="0 0 8 0 8 8" dur="1s"/></polygon>
    <filter><feOffset id="offset" dx="0"><set attributeName="dx" to="4" dur="1s"/></feOffset></filter>)svg");
  document.setTime(0.5);
  (void)RendererTestUtils::renderToAsciiImage(document);
  auto line = document.querySelector("#line")->cast<SVGLineElement>();
  auto poly = document.querySelector("#poly")->cast<SVGPolygonElement>();
  auto offset = document.querySelector("#offset")->cast<SVGFEOffsetElement>();
  line.setX2(Lengthd(8));
  poly.setPoints({{0, 0}, {8, 0}, {8, 8}});
  offset.setOffset(4, 0);
  document.setTime(2);
  (void)RendererTestUtils::renderToAsciiImage(document);
  EXPECT_THAT(line.x2(), testing::Eq(Lengthd(8)));
  EXPECT_THAT(poly.points(), testing::ElementsAre(Vector2d(0, 0), Vector2d(8, 0), Vector2d(8, 8)));
  EXPECT_EQ(offset.dx(), 4);
}

TEST(RendererAnimation, RejectedPropertyProbePreservesAnimatedAppearance) {
  auto document = parseAnimated(
      R"(<rect id="r" width="4" height="16"><set attributeName="width" to="8" dur="2s"/></rect>)");
  document.setTime(1);
  const auto before = RendererTestUtils::renderToAsciiImage(document).generated;
  auto rect = document.querySelector("#r").value();
  EXPECT_THAT(rect.trySetPresentationAttribute("unknown-property", "12"), NoParseError());
  EXPECT_THAT(rect.trySetPresentationAttribute("fill", "this-is-not-a-valid-color").hasError(),
              testing::IsTrue());
  EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated, before);
}

}  // namespace donner::svg
