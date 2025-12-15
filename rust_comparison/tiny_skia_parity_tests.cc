#include <vector>

#include "rust_ffi.h"
#include "donner/backends/tiny_skia_cpp/Canvas.h"
#include "donner/backends/tiny_skia_cpp/Paint.h"
#include "donner/backends/tiny_skia_cpp/PathGeometry.h"
#include "donner/backends/tiny_skia_cpp/Shader.h"
#include "donner/backends/tiny_skia_cpp/Stroke.h"
#include "donner/svg/core/PathSpline.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"

using donner::backends::tiny_skia_cpp::BlendMode;
using donner::backends::tiny_skia_cpp::Canvas;
using donner::backends::tiny_skia_cpp::Color;
using donner::backends::tiny_skia_cpp::FillRule;
using donner::backends::tiny_skia_cpp::GradientStop;
using donner::backends::tiny_skia_cpp::Shader;
using donner::backends::tiny_skia_cpp::SpreadMode;
using donner::backends::tiny_skia_cpp::Stroke;
using donner::backends::tiny_skia_cpp::Transform;
using donner::svg::ImageComparisonParams;
using donner::svg::ImageComparisonTestFixture;
using donner::svg::PathSpline;
using donner::Vector2d;

namespace {

constexpr int kWidth = 192;
constexpr int kHeight = 192;

PathSpline BuildPath() {
  PathSpline path;
  path.moveTo(Vector2d(36.0, 36.0));
  path.lineTo(Vector2d(156.0, 48.0));
  path.curveTo(Vector2d(160.0, 84.0), Vector2d(108.0, 144.0), Vector2d(52.0, 156.0));
  path.closePath();
  return path;
}

Shader BuildGradient() {
  std::vector<GradientStop> stops;
  stops.emplace_back(0.0f, Color(44, 176, 255, 255));
  stops.emplace_back(1.0f, Color(244, 108, 92, 255));

  auto shader = Shader::MakeLinearGradient(Vector2d(20.0, 24.0), Vector2d(172.0, 172.0),
                                           std::move(stops), SpreadMode::kReflect,
                                           Transform::Rotate(0.35));
  EXPECT_TRUE(shader.hasValue()) << shader.error();
  return shader.value();
}

Color BuildStrokeColor() { return Color(250, 250, 252, 255); }

class TinySkiaParityTest : public ImageComparisonTestFixture {};

TEST_F(TinySkiaParityTest, CppMatchesRustReferenceScene) {
  auto canvas = Canvas::Create(kWidth, kHeight);
  ASSERT_TRUE(canvas.hasValue()) << canvas.error();

  canvas.value().clear(Color(18, 18, 22, 255));

  const PathSpline path = BuildPath();

  auto shader = BuildGradient();
  donner::backends::tiny_skia_cpp::Paint fillPaint;
  fillPaint.shader = shader;
  fillPaint.blendMode = BlendMode::kSourceOver;
  fillPaint.antiAlias = true;

  auto fillResult = canvas.value().drawPath(path, fillPaint, FillRule::kNonZero);
  ASSERT_TRUE(fillResult.hasValue()) << fillResult.error();

  Stroke stroke;
  stroke.width = 6.0f;
  stroke.lineJoin = donner::backends::tiny_skia_cpp::LineJoin::kRound;
  stroke.lineCap = donner::backends::tiny_skia_cpp::LineCap::kRound;

  donner::backends::tiny_skia_cpp::Paint strokePaint;
  strokePaint.shader = Shader::MakeSolidColor(BuildStrokeColor());
  strokePaint.blendMode = BlendMode::kSourceOver;
  strokePaint.antiAlias = true;

  auto strokeResult =
      canvas.value().strokePath(path, stroke, strokePaint, Transform::Translate(4.0, 6.0));
  ASSERT_TRUE(strokeResult.hasValue()) << strokeResult.error();

  const size_t cppStrideBytes = canvas.value().pixmap().strideBytes();
  std::vector<uint8_t> cppPixels(canvas.value().pixmap().pixels().begin(),
                                 canvas.value().pixmap().pixels().end());

  const uint32_t rustWidth = tiny_skia_rust_reference_width();
  const uint32_t rustHeight = tiny_skia_rust_reference_height();
  ASSERT_EQ(rustWidth, static_cast<uint32_t>(kWidth));
  ASSERT_EQ(rustHeight, static_cast<uint32_t>(kHeight));

  const size_t rustStrideBytes = tiny_skia_rust_reference_stride();
  std::vector<uint8_t> rustPixels(rustStrideBytes * rustHeight);
  ASSERT_TRUE(tiny_skia_rust_render_reference(rustPixels.data(), rustPixels.size()));

  ImageComparisonParams params = ImageComparisonParams::WithThreshold(0.01f, 7000);
  compareRgbaImages(rustPixels, rustStrideBytes, cppPixels, cppStrideBytes, kWidth, kHeight,
                    "tiny_skia_cpp_rust_canvas", params);
}

}  // namespace
