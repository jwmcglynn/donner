#include <gtest/gtest.h>

#include <filesystem>

#include "donner/base/tests/Runfiles.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"

namespace donner::svg {
namespace {

using Params = ImageComparisonParams;

std::filesystem::path ResvgResourceRoot() {
  return Runfiles::instance().Rlocation("third_party/resvg-test-suite/");
}

ImageComparisonParams GoldenParams() {
  Params params;
  params.enableGoldenUpdateFromEnv();
  return params;
}

class RendererRegressionTests : public ImageComparisonTestFixture {};

TEST_F(RendererRegressionTests, MarkerPercentResolvesAgainstReferencingViewport) {
  const char* svg = "donner/svg/renderer/testdata/marker_percent_nested_viewport.svg";
  const char* golden = "donner/svg/renderer/testdata/golden/marker_percent_nested_viewport.png";

  SVGDocument document = loadSVG(svg, ResvgResourceRoot());
  renderAndCompare(document, svg, golden, GoldenParams());
}

TEST_F(RendererRegressionTests, TextDecorationUnderlineRenders) {
  const char* svg = "donner/svg/renderer/testdata/geode_text_decoration_underline.svg";
  const char* golden = "donner/svg/renderer/testdata/golden/geode_text_decoration_underline.png";

  SVGDocument document = loadSVG(svg, ResvgResourceRoot());
  renderAndCompare(document, svg, golden, GoldenParams());
}

TEST_F(RendererRegressionTests, PatternFillOnTextDoesNotLeakToNextShape) {
  const char* svg = "donner/svg/renderer/testdata/geode_text_pattern_fill.svg";
  const char* golden = "donner/svg/renderer/testdata/golden/geode_text_pattern_fill.png";

  SVGDocument document = loadSVG(svg, ResvgResourceRoot());
  renderAndCompare(document, svg, golden, GoldenParams());
}

TEST_F(RendererRegressionTests, SpanGradientOverridesElementPatternFill) {
  const char* svg = "donner/svg/renderer/testdata/geode_text_span_gradient_over_pattern.svg";
  const char* golden =
      "donner/svg/renderer/testdata/golden/geode_text_span_gradient_over_pattern.png";

  SVGDocument document = loadSVG(svg, ResvgResourceRoot());
  renderAndCompare(document, svg, golden, GoldenParams());
}

TEST_F(RendererRegressionTests, SpanGradientOverridesElementPatternStroke) {
  const char* svg = "donner/svg/renderer/testdata/geode_text_span_gradient_over_pattern_stroke.svg";
  const char* golden =
      "donner/svg/renderer/testdata/golden/geode_text_span_gradient_over_pattern_stroke.png";

  SVGDocument document = loadSVG(svg, ResvgResourceRoot());
  renderAndCompare(document, svg, golden, GoldenParams());
}

TEST_F(RendererRegressionTests, NestedBaselineShiftRedrawIsIdempotent) {
  const char* svg = "donner/svg/renderer/testdata/text_nested_baseline_shift_idempotency.svg";
  SVGDocument document = loadSVG(svg, ResvgResourceRoot());

  const RendererBitmap first = RenderDocumentWithBackend(document, RendererBackend::TinySkia);
  const RendererBitmap second = RenderDocumentWithBackend(document, RendererBackend::TinySkia);

  ASSERT_FALSE(first.empty());
  ASSERT_FALSE(second.empty());
  ExpectBitmapsIdentical(second, first, "nested_baseline_shift_redraw");
}

TEST_F(RendererRegressionTests, FeImageFragmentRedrawIsIdempotent) {
  const char* svg = "donner/svg/renderer/testdata/feimage_fragment_idempotency.svg";
  SVGDocument document = loadSVG(svg, ResvgResourceRoot());

  const RendererBitmap first = RenderDocumentWithBackend(document, RendererBackend::TinySkia);
  const RendererBitmap second = RenderDocumentWithBackend(document, RendererBackend::TinySkia);

  ASSERT_FALSE(first.empty());
  ASSERT_FALSE(second.empty());
  ExpectBitmapsIdentical(second, first, "feimage_fragment_redraw");
}

// vector-effect: non-scaling-stroke keeps the stroke a constant device width under a scaled
// viewBox and an additional transform. The golden captures the correct rendering: thin (2px) blue
// non-scaling strokes next to thick (8px / 16px) red strokes that scale with the transform.
TEST_F(RendererRegressionTests, VectorEffectNonScalingStrokeIsConstantWidth) {
  const char* svg = "donner/svg/renderer/testdata/vector_effect_non_scaling_stroke.svg";
  const char* golden = "donner/svg/renderer/testdata/golden/vector_effect_non_scaling_stroke.png";

  SVGDocument document = loadSVG(svg, ResvgResourceRoot());
  renderAndCompare(document, svg, golden, GoldenParams());
}

// Self-validating guard against a silent no-op: the same document with and without the
// vector-effect attribute must render differently. If non-scaling-stroke were ignored, the two
// renders would be identical.
TEST_F(RendererRegressionTests, VectorEffectNonScalingStrokeChangesOutput) {
  const char* nonScalingSvg = "donner/svg/renderer/testdata/vector_effect_non_scaling_stroke.svg";
  const char* controlSvg = "donner/svg/renderer/testdata/vector_effect_scaling_stroke_control.svg";

  SVGDocument nonScalingDoc = loadSVG(nonScalingSvg, ResvgResourceRoot());
  SVGDocument controlDoc = loadSVG(controlSvg, ResvgResourceRoot());

  const RendererBitmap nonScaling =
      RenderDocumentWithBackend(nonScalingDoc, RendererBackend::TinySkia);
  const RendererBitmap control = RenderDocumentWithBackend(controlDoc, RendererBackend::TinySkia);

  ASSERT_FALSE(nonScaling.empty());
  ASSERT_FALSE(control.empty());
  ASSERT_EQ(nonScaling.dimensions, control.dimensions);
  ASSERT_EQ(nonScaling.pixels.size(), control.pixels.size());
  EXPECT_NE(nonScaling.pixels, control.pixels)
      << "vector-effect: non-scaling-stroke had no effect on the rendered output";
}

}  // namespace
}  // namespace donner::svg
