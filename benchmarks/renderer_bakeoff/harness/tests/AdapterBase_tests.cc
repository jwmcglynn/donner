#include "harness/AdapterBase.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>
#include <string_view>
#include <vector>

#include "harness/NullRecordingRenderer.h"
#include "harness/tests/RecordingAdapter.h"

namespace donner::bakeoff::tests {
namespace {

using testing::ElementsAre;
using testing::Field;
using testing::SizeIs;

CapabilityTable BasicCapabilities() {
  return CapabilityTable::All(Support::Native)
      .set(Feature::MaskLuminance, Support::Unsupported)
      .set(Feature::MaskAlpha, Support::Unsupported)
      .set(Feature::ClipPath, Support::Unsupported);
}

TEST(AdapterBaseTest, ForwardsSupportedDrawsAndRendersFully) {
  svg::SVGDocument document = ParseForTest(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect x="10" y="10" width="20" height="20" fill="red"/>
      <circle cx="70" cy="70" r="10" fill="none" stroke="blue" stroke-width="4"/>
    </svg>)svg");
  RecordingAdapter adapter(BasicCapabilities());
  adapter.draw(document);

  EXPECT_THAT(adapter.draws,
              ElementsAre(Field(&RecordedDraw::stroke, false), Field(&RecordedDraw::stroke, true)));
  EXPECT_THAT(adapter.usage(), RendersFully());
  EXPECT_EQ(adapter.usage().uses(Feature::Stroke), 1u);
  EXPECT_EQ(adapter.width(), 100);
}

TEST(AdapterBaseTest, DropsUnsupportedMaskAndSkipsItsDefinitionContent) {
  svg::SVGDocument document = ParseForTest(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <mask id="m"><rect width="50" height="100" fill="white"/></mask>
      <rect width="100" height="100" fill="green" mask="url(#m)"/>
    </svg>)svg");
  RecordingAdapter adapter(BasicCapabilities());
  adapter.draw(document);

  EXPECT_THAT(adapter.usage(), testing::Not(RendersFully()));
  EXPECT_EQ(adapter.usage().dropped(Feature::MaskLuminance), 1u);
  // Only the masked content is drawn; the white mask rectangle never reaches the frame.
  ASSERT_THAT(adapter.draws, SizeIs(1));
  EXPECT_EQ(adapter.draws[0].paint.color.toRgba(), css::RGBA(0, 128, 0, 255));
  EXPECT_THAT(adapter.events, ElementsAre("begin", "end"));
}

TEST(AdapterBaseTest, DropsUnsupportedClipPathAndCountsIt) {
  svg::SVGDocument document = ParseForTest(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <clipPath id="c"><circle cx="50" cy="50" r="30"/></clipPath>
      <rect width="100" height="100" fill="blue" clip-path="url(#c)"/>
    </svg>)svg");
  RecordingAdapter adapter(BasicCapabilities());
  adapter.draw(document);

  EXPECT_EQ(adapter.usage().dropped(Feature::ClipPath), 1u);
  EXPECT_THAT(adapter.usage(), testing::Not(RendersFully()));
  EXPECT_THAT(adapter.events, ElementsAre("begin", "end"));
  EXPECT_THAT(adapter.draws, SizeIs(1));
}

TEST(AdapterBaseTest, ForwardsSupportedLayerWithBalancedPops) {
  svg::SVGDocument document = ParseForTest(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <g opacity="0.5">
        <rect width="40" height="40" fill="red"/>
        <rect x="20" y="20" width="40" height="40" fill="blue"/>
      </g>
    </svg>)svg");
  RecordingAdapter adapter(BasicCapabilities());
  adapter.draw(document);

  EXPECT_THAT(adapter.events, ElementsAre("begin", "pushLayer", "popLayer", "end"));
  EXPECT_THAT(adapter.draws, SizeIs(2));
}

TEST(AdapterBaseTest, ResolvesObjectBoundingBoxLinearGradientIntoPathSpace) {
  svg::SVGDocument document = ParseForTest(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <linearGradient id="g">
        <stop offset="0" stop-color="#ff0000"/>
        <stop offset="1" stop-color="#0000ff" stop-opacity="0.5"/>
      </linearGradient>
      <rect x="10" y="20" width="50" height="40" fill="url(#g)"/>
    </svg>)svg");
  RecordingAdapter adapter(BasicCapabilities());
  adapter.draw(document);

  ASSERT_THAT(adapter.draws, SizeIs(1));
  const PaintDesc& paint = adapter.draws[0].paint;
  ASSERT_EQ(paint.kind, PaintDesc::Kind::Gradient);
  EXPECT_EQ(paint.gradient.type, GradientDesc::Type::Linear);
  ASSERT_THAT(paint.gradient.stops, SizeIs(2));
  EXPECT_EQ(paint.gradient.stops[1].color.toRgba(), css::RGBA(0, 0, 255, 127));
  // Unit-box (0,0)-(1,0) maps onto the rect's left and right edges at its top.
  EXPECT_EQ(paint.gradient.pathFromGradient.transformPosition(paint.gradient.start),
            Vector2d(10, 20));
  EXPECT_EQ(paint.gradient.pathFromGradient.transformPosition(paint.gradient.end),
            Vector2d(60, 20));
  EXPECT_EQ(adapter.usage().uses(Feature::LinearGradient), 1u);
}

TEST(AdapterBaseTest, NullRendererCountsSceneFeatures) {
  svg::SVGDocument document = ParseForTest(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <clipPath id="c"><circle cx="50" cy="50" r="40"/></clipPath>
      <g clip-path="url(#c)">
        <path d="M0 0 L100 0 L50 100 Z M20 20 L80 20 L50 80 Z" fill-rule="evenodd"/>
      </g>
      <line x1="0" y1="0" x2="100" y2="100" stroke="black" stroke-dasharray="4 2"/>
    </svg>)svg");
  NullRecordingRenderer renderer;
  renderer.draw(document);

  const FeatureUsage& usage = renderer.usage();
  EXPECT_THAT(usage, RendersFully());
  EXPECT_EQ(usage.uses(Feature::ClipPath), 1u);
  EXPECT_EQ(usage.uses(Feature::EvenOddFill), 1u);
  EXPECT_EQ(usage.uses(Feature::StrokeDash), 1u);
}

TEST(AdapterBaseTest, ResolvesUserSpaceRadialGradientWithFocalPoint) {
  svg::SVGDocument document = ParseForTest(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <radialGradient id="g" gradientUnits="userSpaceOnUse" cx="50" cy="40" r="30" fx="45"
                      fy="35" fr="5" spreadMethod="reflect" gradientTransform="translate(10 0)">
        <stop offset="0" stop-color="#ffffff"/>
        <stop offset="1" stop-color="#000000"/>
      </radialGradient>
      <rect width="100" height="100" fill="url(#g)"/>
    </svg>)svg");
  RecordingAdapter adapter(BasicCapabilities());
  adapter.draw(document);

  ASSERT_THAT(adapter.draws, SizeIs(1));
  const GradientDesc& gradient = adapter.draws[0].paint.gradient;
  EXPECT_EQ(adapter.draws[0].paint.kind, PaintDesc::Kind::Gradient);
  EXPECT_EQ(gradient.type, GradientDesc::Type::Radial);
  EXPECT_EQ(gradient.center, Vector2d(50, 40));
  EXPECT_EQ(gradient.focal, Vector2d(45, 35));
  EXPECT_EQ(gradient.radius, 30.0);
  EXPECT_EQ(gradient.focalRadius, 5.0);
  EXPECT_EQ(gradient.spread, SpreadMode::Reflect);
  EXPECT_EQ(gradient.pathFromGradient.transformPosition(gradient.center), Vector2d(60, 40));
  EXPECT_EQ(adapter.usage().uses(Feature::RadialGradient), 1u);
}

TEST(AdapterBaseTest, ClassifiesNestedClipPathsAndKeepsTheirLayers) {
  svg::SVGDocument document = ParseForTest(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <clipPath id="inner"><rect width="50" height="100"/></clipPath>
      <clipPath id="outer"><circle cx="50" cy="50" r="40" clip-path="url(#inner)"/></clipPath>
      <rect width="100" height="100" fill="red" clip-path="url(#outer)"/>
    </svg>)svg");
  RecordingAdapter adapter(CapabilityTable::All(Support::Native));
  adapter.draw(document);

  EXPECT_EQ(adapter.usage().uses(Feature::ClipPathNested), 1u);
  ASSERT_THAT(adapter.clips, SizeIs(1));
  ASSERT_THAT(adapter.clips[0].shapes, SizeIs(2));
  EXPECT_NE(adapter.clips[0].shapes[0].layer, adapter.clips[0].shapes[1].layer);
}

TEST(AdapterBaseTest, CountsFilterTheAdapterRejectsAsDropped) {
  svg::SVGDocument document = ParseForTest(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <filter id="f"><feGaussianBlur stdDeviation="2"/></filter>
      <rect width="50" height="50" fill="blue" filter="url(#f)"/>
    </svg>)svg");
  RecordingAdapter adapter(CapabilityTable::All(Support::Native));
  adapter.acceptFilters = false;
  adapter.draw(document);

  EXPECT_EQ(adapter.usage().dropped(Feature::Filter), 1u);
  EXPECT_THAT(adapter.events, ElementsAre("begin", "pushFilter", "end"));
  EXPECT_THAT(adapter.draws, SizeIs(1));
}

TEST(AdapterBaseTest, ForwardsSupportedMaskInDefinitionThenContentOrder) {
  svg::SVGDocument document = ParseForTest(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <mask id="m"><rect width="50" height="100" fill="white"/></mask>
      <rect width="100" height="100" fill="green" mask="url(#m)"/>
    </svg>)svg");
  RecordingAdapter adapter(CapabilityTable::All(Support::Native));
  adapter.draw(document);

  EXPECT_THAT(adapter.usage(), RendersFully());
  EXPECT_THAT(adapter.events, ElementsAre("begin", "pushMask", "maskContent", "popMask", "end"));
  ASSERT_THAT(adapter.draws, SizeIs(2));
  EXPECT_EQ(adapter.draws[0].paint.color.toRgba(), css::RGBA(255, 255, 255, 255));
  EXPECT_EQ(adapter.draws[1].paint.color.toRgba(), css::RGBA(0, 128, 0, 255));
}

TEST(AdapterBaseTest, PaintsWithAnAcceptedPatternTileAndDropsARejectedOne) {
  constexpr std::string_view kSvg = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <pattern id="p" width="10" height="10" patternUnits="userSpaceOnUse">
        <rect width="5" height="5" fill="black"/>
      </pattern>
      <rect width="100" height="100" fill="url(#p) red"/>
    </svg>)svg";

  svg::SVGDocument accepting = ParseForTest(kSvg);
  RecordingAdapter acceptor(CapabilityTable::All(Support::Native));
  acceptor.acceptPatterns = true;
  acceptor.draw(accepting);
  EXPECT_THAT(acceptor.usage(), RendersFully());
  ASSERT_THAT(acceptor.draws, testing::Not(testing::IsEmpty()));
  EXPECT_EQ(acceptor.draws.back().paint.kind, PaintDesc::Kind::Pattern);

  svg::SVGDocument rejecting = ParseForTest(kSvg);
  RecordingAdapter rejector(CapabilityTable::All(Support::Native));
  rejector.draw(rejecting);
  EXPECT_EQ(rejector.usage().dropped(Feature::PatternPaint), 1u);
  EXPECT_THAT(rejector.events, testing::Contains("beginPatternTile"));
}

TEST(AdapterBaseTest, PatternPaintsOnlyItsOwnElementAtItsOwnDepth) {
  svg::SVGDocument document = ParseForTest(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <pattern id="a" width="10" height="10" patternUnits="userSpaceOnUse">
        <rect width="5" height="5" fill="#ff0000"/>
      </pattern>
      <pattern id="b" width="10" height="10" patternUnits="userSpaceOnUse">
        <rect width="5" height="5" fill="#00ff00"/>
      </pattern>
      <rect width="50" height="50" fill="url(#a)" stroke="url(#b)" stroke-width="4"/>
      <rect x="60" y="60" width="20" height="20" fill="url(#a)" visibility="hidden"/>
      <rect x="60" y="0" width="20" height="20" fill="#0000ff"/>
    </svg>)svg");
  RecordingAdapter adapter(CapabilityTable::All(Support::Native));
  adapter.acceptPatterns = true;
  adapter.draw(document);

  // Only the patterned element's own fill and stroke use the pattern paint: the tile contents
  // drawn while recording, and the shape after the hidden patterned element, stay solid.
  std::vector<RecordedDraw> patterned;
  std::copy_if(
      adapter.draws.begin(), adapter.draws.end(), std::back_inserter(patterned),
      [](const RecordedDraw& draw) { return draw.paint.kind == PaintDesc::Kind::Pattern; });
  EXPECT_THAT(patterned,
              ElementsAre(Field(&RecordedDraw::stroke, false), Field(&RecordedDraw::stroke, true)));
  ASSERT_THAT(adapter.draws, testing::Not(testing::IsEmpty()));
  EXPECT_EQ(adapter.draws.back().paint.kind, PaintDesc::Kind::Solid);
  EXPECT_EQ(adapter.draws.back().paint.color.toRgba(), css::RGBA(0, 0, 255, 255));
}

TEST(AdapterBaseTest, NestedPatternTilesKeepBeginAndEndBalanced) {
  svg::SVGDocument document = ParseForTest(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <pattern id="inner" width="4" height="4" patternUnits="userSpaceOnUse">
        <rect width="2" height="2" fill="black"/>
      </pattern>
      <pattern id="outer" width="20" height="20" patternUnits="userSpaceOnUse">
        <rect width="10" height="10" fill="url(#inner)"/>
      </pattern>
      <rect width="100" height="100" fill="url(#outer)"/>
    </svg>)svg");
  RecordingAdapter adapter(CapabilityTable::All(Support::Native));
  adapter.acceptPatterns = true;
  adapter.draw(document);

  const auto count = [&](std::string_view event) {
    return std::count(adapter.events.begin(), adapter.events.end(), event);
  };
  EXPECT_EQ(count("beginPatternTile"), 2);
  EXPECT_EQ(count("endPatternTile(fill)"), 2);
  ASSERT_THAT(adapter.draws, testing::Not(testing::IsEmpty()));
  EXPECT_EQ(adapter.draws.back().paint.kind, PaintDesc::Kind::Pattern);
}

}  // namespace
}  // namespace donner::bakeoff::tests
